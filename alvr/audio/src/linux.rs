use alvr_common::{
    ConnectionError,
    anyhow::{Result, anyhow, bail},
    debug, error,
    parking_lot::Mutex,
};
use alvr_session::{AudioBufferingConfig, StreamingAudioDevice};
use alvr_sockets::{StreamReceiver, StreamSender};

use std::os::unix::fs::FileTypeExt;
use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    collections::VecDeque,
    fs, io,
    path::Path,
    rc::Rc,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread::{self, sleep},
    time::Duration,
};

// Node names of the streams ALVR makes while a headset streams (see below).
const ALVR_CLIENT_SINK: &str = "ALVR Audio";
const ALVR_CLIENT_SOURCE: &str = "ALVR Microphone";

// Keys of PipeWire's "default" metadata, kept by the session manager
// (WirePlumber or pipewire-media-session). The effective default is read;
// the configured one is written, as desktop sound settings and pactl do.
const DEFAULT_SINK_KEY: &str = "default.audio.sink";
const DEFAULT_SOURCE_KEY: &str = "default.audio.source";
const CONFIGURED_SINK_KEY: &str = "default.configured.audio.sink";
const CONFIGURED_SOURCE_KEY: &str = "default.configured.audio.source";

#[derive(Clone, Copy, PartialEq)]
pub enum DeviceKind {
    Output,
    Input,
}

impl DeviceKind {
    fn default_key(self) -> &'static str {
        match self {
            DeviceKind::Output => DEFAULT_SINK_KEY,
            DeviceKind::Input => DEFAULT_SOURCE_KEY,
        }
    }

    fn configured_key(self) -> &'static str {
        match self {
            DeviceKind::Output => CONFIGURED_SINK_KEY,
            DeviceKind::Input => CONFIGURED_SOURCE_KEY,
        }
    }

    fn headset_device(self) -> &'static str {
        match self {
            DeviceKind::Output => ALVR_CLIENT_SINK,
            DeviceKind::Input => ALVR_CLIENT_SOURCE,
        }
    }
}

struct AudioNode {
    kind: DeviceKind,
    name: String,
    description: String,
}

#[derive(Default)]
struct GraphState {
    nodes: Vec<AudioNode>,
    default_metadata: Option<GlobalObject<PropertiesBox>>,
    // Metadata key -> node name.
    defaults: HashMap<String, String>,
}

// The node name inside a default-device metadata value, {"name": "..."}.
fn name_from_metadata_value(value: &str) -> Option<String> {
    let after_key = value.split_once("\"name\"")?.1;
    let after_colon = after_key.trim_start().strip_prefix(':')?.trim_start();
    let mut name = String::new();
    let mut chars = after_colon.strip_prefix('"')?.chars();
    while let Some(c) = chars.next() {
        match c {
            '"' => return Some(name),
            '\\' => name.push(chars.next()?),
            c => name.push(c),
        }
    }
    None
}

fn metadata_value_for_name(name: &str) -> String {
    let escaped = name.replace('\\', "\\\\").replace('"', "\\\"");
    format!("{{\"name\":\"{escaped}\"}}")
}

/// Waits until the server has answered everything asked so far.
fn roundtrip(core: &CoreRc, mainloop: &MainLoopRc) -> Result<()> {
    let done = Rc::new(Cell::new(false));
    let pending = core.sync(0)?;
    let _listener = core
        .add_listener_local()
        .done({
            let done = done.clone();
            let mainloop = mainloop.clone();
            move |id, seq| {
                if id == pipewire::core::PW_ID_CORE && seq == pending {
                    done.set(true);
                    mainloop.quit();
                }
            }
        })
        .register();
    while !done.get() {
        mainloop.run();
    }
    Ok(())
}

/// Connects to PipeWire, reads the audio devices and the default-device
/// metadata, and runs `action` with them; the metadata object is there to
/// change the defaults. A short-lived connection: called about once a second
/// by the dashboard at most.
fn with_audio_graph<R>(action: impl FnOnce(&GraphState, Option<&Metadata>) -> R) -> Result<R> {
    let mainloop = MainLoopRc::new(None)?;
    let context = ContextRc::new(&mainloop, None)?;
    let core = context.connect_rc(None)?;
    let registry = core.get_registry_rc()?;

    let state = Rc::new(RefCell::new(GraphState::default()));
    let _registry_listener = registry
        .add_listener_local()
        .global({
            let state = state.clone();
            move |global| {
                let Some(props) = global.props else {
                    return;
                };
                match global.type_ {
                    ObjectType::Node => {
                        let kind = match props.get(*keys::MEDIA_CLASS) {
                            Some("Audio/Sink") => DeviceKind::Output,
                            Some("Audio/Source") => DeviceKind::Input,
                            _ => return,
                        };
                        let Some(name) = props.get(*keys::NODE_NAME) else {
                            return;
                        };
                        let description = props
                            .get(*keys::NODE_DESCRIPTION)
                            .or_else(|| props.get(*keys::NODE_NICK))
                            .unwrap_or(name);
                        state.borrow_mut().nodes.push(AudioNode {
                            kind,
                            name: name.to_owned(),
                            description: description.to_owned(),
                        });
                    }
                    ObjectType::Metadata if props.get("metadata.name") == Some("default") => {
                        state.borrow_mut().default_metadata = Some(global.to_owned());
                    }
                    _ => (),
                }
            }
        })
        .register();
    roundtrip(&core, &mainloop)?;

    let metadata_global = state.borrow_mut().default_metadata.take();
    let metadata = match &metadata_global {
        Some(global) => Some(registry.bind::<Metadata, _>(global)?),
        None => None,
    };
    let _metadata_listener = metadata.as_ref().map(|metadata| {
        let state = state.clone();
        metadata
            .add_listener_local()
            .property(move |subject, key, _, value| {
                if subject == 0
                    && let (Some(key), Some(value)) = (key, value)
                    && let Some(name) = name_from_metadata_value(value)
                {
                    state.borrow_mut().defaults.insert(key.to_owned(), name);
                }
                0
            })
            .register()
    });
    if metadata.is_some() {
        roundtrip(&core, &mainloop)?;
    }

    let result = action(&state.borrow(), metadata.as_ref());
    if metadata.is_some() {
        // Sends what `action` changed before the connection closes.
        roundtrip(&core, &mainloop)?;
    }
    Ok(result)
}

fn set_default(metadata: &Metadata, kind: DeviceKind, name: &str) {
    metadata.set_property(
        0,
        kind.configured_key(),
        Some("Spa:String:JSON"),
        Some(&metadata_value_for_name(name)),
    );
}

/// Whether the system's default devices can be followed and changed: a
/// PipeWire server with a session manager keeping the "default" metadata.
/// Without it the streaming device settings do nothing, as before them.
pub fn streaming_devices_supported() -> bool {
    with_audio_graph(|_, metadata| metadata.is_some()).unwrap_or(false)
}

/// The system's default output and input as they were with no headset
/// streaming: what "System default" means and what comes back afterwards.
/// Either may be missing: a computer without a microphone has no default
/// input.
static IDLE_DEFAULTS: Mutex<Option<(Option<String>, Option<String>)>> = Mutex::new(None);

/// To be called now and then while no headset streams. The session manager
/// makes a device that just appeared the default by itself, so asking at the
/// moment streaming starts is too late: it answers with ALVR's own devices.
pub fn remember_idle_default_devices() {
    let defaults = with_audio_graph(|state, _| {
        (
            state.defaults.get(DEFAULT_SINK_KEY).cloned(),
            state.defaults.get(DEFAULT_SOURCE_KEY).cloned(),
        )
    });
    if let Ok((sink, source)) = defaults
        && (sink.is_some() || source.is_some())
        && sink.as_deref() != Some(ALVR_CLIENT_SINK)
        && source.as_deref() != Some(ALVR_CLIENT_SOURCE)
    {
        *IDLE_DEFAULTS.lock() = Some((sink, source));
    }
}

/// Name and description of the system's outputs or inputs, without ALVR's
/// own devices.
pub fn list_audio_devices(kind: DeviceKind) -> Result<Vec<(String, String)>> {
    with_audio_graph(|state, _| {
        state
            .nodes
            .iter()
            .filter(|node| {
                node.kind == kind
                    && ![ALVR_CLIENT_SINK, ALVR_CLIENT_SOURCE]
                        .iter()
                        .any(|own| node.name == *own || node.description == *own)
            })
            .map(|node| (node.name.clone(), node.description.clone()))
            .collect()
    })
}

/// The node to make the default for `selection` (`None`: its switch is off),
/// or `None` when nothing is to change.
fn target_device(
    state: &GraphState,
    kind: DeviceKind,
    selection: Option<&StreamingAudioDevice>,
    idle_default: Option<&String>,
) -> Result<Option<String>> {
    let present = |wanted: &str| {
        state
            .nodes
            .iter()
            .find(|node| node.kind == kind && (node.name == wanted || node.description == wanted))
            .map(|node| node.name.clone())
    };
    match selection {
        Some(StreamingAudioDevice::Headset) => present(kind.headset_device())
            .map(Some)
            .ok_or_else(|| anyhow!("\"{}\" is not there yet", kind.headset_device())),
        Some(StreamingAudioDevice::Custom(wanted)) => present(wanted)
            .map(Some)
            .ok_or_else(|| anyhow!("the device \"{wanted}\" is not there")),
        // "System default" and a switch that is off both mean the device of
        // before streaming, put back as the session manager moves to ALVR's
        // device on its own.
        Some(StreamingAudioDevice::SystemDefault) | None => Ok(idle_default.cloned()),
    }
}

/// Makes the chosen devices the system's default output and input for the
/// time a headset streams. Fails while a device is not there yet, as ALVR's
/// are for a moment after streaming starts: call again. Calling it again
/// later is also what undoes a change the session manager made meanwhile.
/// The defaults of before streaming go to `recovery_file`, so that a
/// dashboard that does not get to restore them (a crash) can do it at its
/// next start.
pub fn streaming_select_default_devices(
    speakers: Option<&StreamingAudioDevice>,
    microphone: Option<&StreamingAudioDevice>,
    recovery_file: &Path,
) -> Result<()> {
    let idle = IDLE_DEFAULTS.lock().clone();
    if let Some((sink, source)) = &idle
        && !recovery_file.exists()
    {
        let sink = sink.as_deref().unwrap_or_default();
        let source = source.as_deref().unwrap_or_default();
        fs::write(recovery_file, format!("{sink}\n{source}\n")).ok();
    }
    with_audio_graph(|state, metadata| {
        let Some(metadata) = metadata else {
            bail!("no default-device metadata (no PipeWire session manager)");
        };
        let mut result = Ok(());
        for (kind, selection, idle_default) in [
            (
                DeviceKind::Output,
                speakers,
                idle.as_ref().and_then(|(sink, _)| sink.as_ref()),
            ),
            (
                DeviceKind::Input,
                microphone,
                idle.as_ref().and_then(|(_, source)| source.as_ref()),
            ),
        ] {
            match target_device(state, kind, selection, idle_default) {
                Ok(Some(target)) => {
                    if state.defaults.get(kind.default_key()) != Some(&target) {
                        set_default(metadata, kind, &target);
                    }
                }
                Ok(None) => (),
                Err(e) => result = Err(e),
            }
        }
        result
    })?
}

fn restore_defaults(sink: Option<&str>, source: Option<&str>) {
    with_audio_graph(|_, metadata| {
        if let Some(metadata) = metadata {
            if let Some(sink) = sink.filter(|name| !name.is_empty()) {
                set_default(metadata, DeviceKind::Output, sink);
            }
            if let Some(source) = source.filter(|name| !name.is_empty()) {
                set_default(metadata, DeviceKind::Input, source);
            }
        }
    })
    .ok();
}

/// Puts the defaults of before streaming back.
pub fn streaming_restore_default_devices(recovery_file: &Path) {
    if let Some((sink, source)) = IDLE_DEFAULTS.lock().clone() {
        restore_defaults(sink.as_deref(), source.as_deref());
    }
    fs::remove_file(recovery_file).ok();
}

/// At the dashboard's start: a recovery file left behind means the last
/// stream ended without the defaults being put back (the dashboard crashed
/// or was killed); put them back now.
pub fn recover_default_devices(recovery_file: &Path) {
    if let Ok(text) = fs::read_to_string(recovery_file) {
        let mut lines = text.lines();
        restore_defaults(lines.next(), lines.next());
        fs::remove_file(recovery_file).ok();
    }
}

use pipewire::{
    channel::Receiver,
    context::ContextRc,
    core::CoreRc,
    keys,
    main_loop::MainLoopRc,
    metadata::Metadata,
    properties::{self, PropertiesBox},
    registry::GlobalObject,
    spa::{
        param::audio::{AudioFormat, AudioInfoRaw},
        pod::{self, Pod, Value, serialize::PodSerializer},
        utils::Direction,
    },
    stream::{StreamFlags, StreamListener, StreamRc, StreamState},
    types::ObjectType,
};

pub fn try_load_pipewire() -> Result<()> {
    if let Err(e) = probe_pipewire() {
        if !matches!(e, pipewire::Error::CreationFailed) {
            return Err(e.into());
        }
        error!("Could not initialize PipeWire.");

        let is_under_flatpak = std::env::var("FLATPAK_ID").is_ok();
        let is_pw_socket_available =
            std::env::var("XDG_RUNTIME_DIR").is_ok_and(|xdg_runtime_dir| {
                let pw_socket_path = Path::new(&xdg_runtime_dir).join("pipewire-0");
                fs::metadata(&pw_socket_path).is_ok_and(|m| m.file_type().is_socket())
            });

        if is_under_flatpak && !is_pw_socket_available {
            error!(
                "Please visit the following page to find help on how to fix broken audio on flatpak."
            );
            error!(
                "https://github.com/alvr-org/ALVR/wiki/Installing-ALVR-and-using-SteamVR-on-Linux-through-Flatpak#failed-to-create-pipewire-errors"
            );
        }
        error!("Make sure PipeWire is installed on your system, running and it's version is at least 0.3.49.
        To retry, please restart SteamVR with ALVR.");
    }
    Ok(())
}

fn probe_pipewire() -> Result<(), pipewire::Error> {
    let mainloop = MainLoopRc::new(None)?;
    let context = ContextRc::new(&mainloop, None)?;
    context.connect(None)?;
    Ok(())
}

#[derive(Clone, Copy)]
pub struct AudioInfo {
    pub sample_rate: u32,
    pub channel_count: u32,
}

struct Terminate;

// fixme: Opening pavucontrol while audio is actively streaming
//  will cause audio cut out for short time,
//  possibly related to fast state changes caused by pavucontrol
static MIC_STREAMING: AtomicBool = AtomicBool::new(false);

pub fn audio_loop(
    is_running: impl Fn() -> bool,
    sender: StreamSender<()>,
    speaker_info: Option<AudioInfo>,
    receiver: &mut StreamReceiver<()>,
    mic_info: Option<(AudioInfo, AudioBufferingConfig)>,
) {
    let sample_queue = Arc::new(Mutex::new(VecDeque::new()));
    MIC_STREAMING.store(false, Ordering::Relaxed);

    let (pw_sender, pw_receiver) = pipewire::channel::channel();

    // Stall pipewire startup until we're actually streaming to not cause latency by packet buildup
    if !is_running() {
        return;
    }

    let pw_thread = thread::spawn({
        let sample_queue = sample_queue.clone();
        let mic_info = mic_info.as_ref().map(|(info, _)| *info);

        move || {
            if let Err(e) = pw_main_loop(pw_receiver, sender, speaker_info, sample_queue, mic_info)
            {
                error!("Unhandled pipewire audio device error, please report it on GitHub: {e}");
            }
            debug!("Pipewire audio loop exiting");
        }
    });

    while is_running() {
        if let Some((mic_info, buffering)) = &mic_info {
            let rate = mic_info.sample_rate as usize;

            let batch_frames_count = rate * buffering.batch_ms as usize / 1000;
            let average_buffer_frames_count = rate * buffering.average_buffering_ms as usize / 1000;

            if let Err(e) = crate::receive_samples_loop(
                || is_running() && MIC_STREAMING.load(Ordering::Relaxed),
                receiver,
                sample_queue.clone(),
                mic_info.channel_count as usize,
                batch_frames_count,
                average_buffer_frames_count,
            ) {
                error!("Receive samples loop encountered error {e:?}");
            }

            // if we end up here then no consumer is currently connected to the output
            // so discard audio packets to not cause a buildup
            if matches!(
                receiver.recv(Duration::from_millis(500)),
                Err(ConnectionError::Other(_))
            ) {
                break;
            }
        } else {
            sleep(Duration::from_millis(500));
        }
    }

    if pw_sender.send(Terminate).is_err() {
        error!(
            "Couldn't send pipewire termination signal, deinitializing forcefully.
                Restart the VR app to reinitialize the audio device."
        );

        unsafe { pipewire::deinit() };
    }

    pw_thread.join().ok();
}

fn pw_main_loop(
    pw_receiver: Receiver<Terminate>,
    audio_sender: StreamSender<()>,
    speaker_info: Option<AudioInfo>,
    sample_queue: Arc<Mutex<VecDeque<f32>>>,
    mic_info: Option<AudioInfo>,
) -> Result<(), pipewire::Error> {
    debug!("Starting pipewire thread");
    let mainloop = MainLoopRc::new(None)?;

    let _receiver = pw_receiver.attach(mainloop.as_ref(), {
        let mainloop = mainloop.clone();
        move |_| mainloop.quit()
    });

    let context = ContextRc::new(&mainloop, None)?;
    let pw_core = context.connect_rc(None)?;

    let _speaker = if let Some(info) = speaker_info {
        debug!("Creating pw output audio stream");
        Some(create_speaker_stream(
            &pw_core,
            audio_sender,
            info.sample_rate,
            info.channel_count,
        )?)
    } else {
        None
    };

    let _mic = if let Some(info) = mic_info {
        debug!("Creating pw microphone stream");
        Some(create_mic_stream(
            &pw_core,
            sample_queue,
            info.sample_rate,
            info.channel_count,
        )?)
    } else {
        None
    };

    debug!("Running pipewire thread");
    mainloop.run();

    Ok(())
}

fn audio_info_to_vec(audio_info: AudioInfoRaw) -> Vec<u8> {
    PodSerializer::serialize(
        io::Cursor::new(Vec::new()),
        &Value::Object(pod::Object {
            type_: libspa_sys::SPA_TYPE_OBJECT_Format,
            id: libspa_sys::SPA_PARAM_EnumFormat,
            properties: audio_info.into(),
        }),
    )
    .unwrap()
    .0
    .into_inner()
}

fn create_speaker_stream(
    pw_core: &CoreRc,
    mut sender: StreamSender<()>,
    sample_rate: u32,
    channel_count: u32,
) -> Result<(StreamRc, StreamListener<i16>), pipewire::Error> {
    let stream = StreamRc::new(
        pw_core.clone(),
        "alvr-audio",
        properties::properties! {
            *keys::NODE_NAME => "ALVR Audio",
            *keys::MEDIA_NAME => "alvr-audio",
            *keys::MEDIA_TYPE => "Audio",
            *keys::MEDIA_CATEGORY => "Capture",
            *keys::MEDIA_CLASS => "Audio/Sink",
            *keys::MEDIA_ROLE => "Game",
        },
    )?;

    let listener: StreamListener<i16> = stream
        .add_local_listener()
        .process(move |stream, _| {
            if let Some(mut pw_buf) = stream.dequeue_buffer()
                && let Some(pw_buf) = pw_buf.datas_mut().first_mut()
            {
                let size = pw_buf.chunk_mut().size() as usize;

                if let Some(data) = pw_buf.data() {
                    // Data is given as s16le in the correct layout by pipewire already,
                    // no need to do conversions
                    sender.send_header_with_payload(&(), &data[0..size]).ok();
                }
            }
        })
        .register()?;

    let mut audio_info = AudioInfoRaw::new();
    audio_info.set_format(AudioFormat::S16LE);
    audio_info.set_rate(sample_rate);
    audio_info.set_channels(channel_count);

    stream.connect(
        Direction::Input,
        None,
        StreamFlags::AUTOCONNECT | StreamFlags::MAP_BUFFERS,
        &mut [Pod::from_bytes(&audio_info_to_vec(audio_info)).unwrap()],
    )?;

    Ok((stream, listener))
}

fn create_mic_stream(
    pw_core: &CoreRc,
    sample_queue: Arc<Mutex<VecDeque<f32>>>,
    sample_rate: u32,
    channel_count: u32,
) -> Result<(StreamRc, StreamListener<f32>), pipewire::Error> {
    let stream = StreamRc::new(
        pw_core.clone(),
        "alvr-mic",
        properties::properties! {
            *keys::NODE_NAME => "ALVR Microphone",
            *keys::MEDIA_NAME => "alvr-mic",
            *keys::MEDIA_TYPE => "Audio",
            *keys::MEDIA_CATEGORY => "Playback",
            *keys::MEDIA_CLASS => "Audio/Source",
            *keys::MEDIA_ROLE => "Communication",
        },
    )?;

    let chan_size = std::mem::size_of::<f32>();
    let listener: StreamListener<f32> = stream
        .add_local_listener()
        .state_changed(move |_, _, _, new_state| {
            MIC_STREAMING.store(new_state == StreamState::Streaming, Ordering::Relaxed);
        })
        .process(move |stream, _| {
            fill_pw_buf(
                sample_queue.clone(),
                chan_size,
                channel_count as usize,
                stream,
            );
        })
        .register()?;

    let mut audio_info = AudioInfoRaw::new();
    audio_info.set_format(AudioFormat::F32LE);
    audio_info.set_rate(sample_rate);
    audio_info.set_channels(channel_count);

    stream.connect(
        Direction::Output,
        None,
        StreamFlags::AUTOCONNECT | StreamFlags::MAP_BUFFERS,
        &mut [Pod::from_bytes(&audio_info_to_vec(audio_info)).unwrap()],
    )?;

    Ok((stream, listener))
}

fn fill_pw_buf(
    sample_queue: Arc<Mutex<VecDeque<f32>>>,
    chan_size: usize,
    chan_count: usize,
    stream: &pipewire::stream::Stream,
) {
    if let Some(mut pw_buf) = stream.dequeue_buffer() {
        let requested = pw_buf.requested() as usize;

        if let Some(pw_data) = pw_buf.datas_mut().first_mut()
            && let Some(slice) = pw_data.data()
            && let Some(mut samples) = sample_queue.try_lock()
        {
            // TODO: Would it be more correct to try to split it up over multiple datas?
            // Or is the requested size already right for the first data chunk?

            let mut it = slice
                .chunks_exact_mut(chan_size)
                .take(requested * chan_count);
            let pw_sample_count = it.len();

            let (front, back) = samples.as_slices();
            let copy_sample =
                |(chunk, sample): (&mut [u8], &f32)| chunk.copy_from_slice(&sample.to_le_bytes());

            // Split up so the compiler actually optimizes this properly
            it.by_ref().zip(front).for_each(copy_sample);
            it.zip(back).for_each(copy_sample);

            let sample_count = pw_sample_count.min(samples.len());
            drop(samples.drain(..sample_count));

            let chunk = pw_data.chunk_mut();
            *chunk.offset_mut() = 0;
            *chunk.stride_mut() = (chan_size * chan_count) as _;
            *chunk.size_mut() = (sample_count * chan_size) as _;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{metadata_value_for_name, name_from_metadata_value};

    // Reads this machine's PipeWire graph; changes nothing. Run by hand:
    // cargo test -p alvr_audio --lib -- --ignored --nocapture
    #[test]
    #[ignore]
    fn live_graph() {
        use super::{
            DeviceKind, list_audio_devices, streaming_devices_supported, with_audio_graph,
        };
        println!("supported: {}", streaming_devices_supported());
        println!("outputs: {:?}", list_audio_devices(DeviceKind::Output));
        println!("inputs: {:?}", list_audio_devices(DeviceKind::Input));
        println!(
            "defaults: {:?}",
            with_audio_graph(|state, _| state.defaults.clone())
        );
    }

    #[test]
    fn default_device_metadata_values() {
        assert_eq!(
            name_from_metadata_value(r#"{ "name": "alsa_output.pci-0000_00_1f.3.analog-stereo" }"#)
                .as_deref(),
            Some("alsa_output.pci-0000_00_1f.3.analog-stereo")
        );
        assert_eq!(name_from_metadata_value("{}"), None);
        for name in ["ALVR Audio", r#"odd "quoted" \ name"#] {
            assert_eq!(
                name_from_metadata_value(&metadata_value_for_name(name)).as_deref(),
                Some(name)
            );
        }
    }
}
