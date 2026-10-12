use alvr_common::{ConnectionError, anyhow::{Result, bail}, debug, error, parking_lot::Mutex};
use alvr_session::AudioBufferingConfig;
use alvr_sockets::{StreamReceiver, StreamSender};

use std::os::unix::fs::FileTypeExt;
use std::{
    collections::VecDeque,
    fs, io,
    path::Path,
    process::Command,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread::{self, sleep},
    time::Duration,
};

const AIRLINK_SPEAKERS_SINK: &str = "mhl_airlink_speakers";
const AIRLINK_MICROPHONE_SINK: &str = "mhl_airlink_microphone_bus";
const AIRLINK_MICROPHONE_SOURCE: &str = "mhl_airlink_microphone";

fn pactl(args: &[&str]) -> Result<String> {
    let executable = std::env::var_os("MHL_PACTL").unwrap_or_else(|| "pactl".into());
    let output = Command::new(executable).args(args).output()?;
    if !output.status.success() {
        let error = String::from_utf8_lossy(&output.stderr);
        bail!("pactl {} failed: {}", args.join(" "), error.trim());
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn pipewire_module_ids() -> Result<Vec<(u32, String)>> {
    let modules = pactl(&["list", "short", "modules"])?
        .lines()
        .filter_map(|line| {
            let mut fields = line.splitn(3, '\t');
            let id = fields.next()?.parse().ok()?;
            let module = fields.next()?;
            let arguments = fields.next().unwrap_or_default();
            Some((id, format!("{module} {arguments}")))
        })
        .collect();
    Ok(modules)
}

fn airlink_module_ids() -> Result<Vec<u32>> {
    Ok(pipewire_module_ids()?
        .into_iter()
        .filter_map(|(id, description)| {
            [
                format!("sink_name={AIRLINK_SPEAKERS_SINK}"),
                format!("sink_name={AIRLINK_MICROPHONE_SINK}"),
                format!("source_name={AIRLINK_MICROPHONE_SOURCE}"),
            ]
            .iter()
            .any(|name| description.contains(name))
            .then_some(id)
        })
        .collect())
}

pub struct AirLinkDevices;

impl AirLinkDevices {
    pub fn is_enabled() -> Result<bool> {
        let modules = airlink_module_ids()?;
        let sinks = pactl(&["list", "short", "sinks"])?;
        let sources = pactl(&["list", "short", "sources"])?;
        Ok(modules.len() == 3
            && sinks
                .lines()
                .any(|line| line.split('\t').nth(1) == Some(AIRLINK_SPEAKERS_SINK))
            && sinks
                .lines()
                .any(|line| line.split('\t').nth(1) == Some(AIRLINK_MICROPHONE_SINK))
            && sources
                .lines()
                .any(|line| line.split('\t').nth(1) == Some(AIRLINK_MICROPHONE_SOURCE)))
        }

    pub fn set_enabled(enabled: bool) -> Result<bool> {
        if enabled {
            if Self::is_enabled()? {
                return Ok(true);
            }

            // Remove only a partial AirLink setup left by an interrupted start.
            for id in airlink_module_ids()?.into_iter().rev() {
                pactl(&["unload-module", &id.to_string()])?;
            }

            let modules = [
                (
                    "module-null-sink",
                    vec![
                        format!("sink_name={AIRLINK_SPEAKERS_SINK}"),
                        "format=s16le".into(),
                        "rate=48000".into(),
                        "channels=2".into(),
                        "channel_map=front-left,front-right".into(),
                        "sink_properties=device.description=AirLink_Speakers".into(),
                    ],
                ),
                (
                    "module-null-sink",
                    vec![
                        format!("sink_name={AIRLINK_MICROPHONE_SINK}"),
                        "format=s16le".into(),
                        "rate=48000".into(),
                        "channels=1".into(),
                        "channel_map=mono".into(),
                        "sink_properties=device.description=AirLink_Microphone_Input".into(),
                    ],
                ),
                (
                    "module-remap-source",
                    vec![
                        format!("master={AIRLINK_MICROPHONE_SINK}.monitor"),
                        format!("source_name={AIRLINK_MICROPHONE_SOURCE}"),
                        "master_channel_map=mono".into(),
                        "channel_map=mono".into(),
                        "source_properties=device.description=AirLink_Microphone".into(),
                    ],
                ),
            ];
            let mut loaded = Vec::new();
            for (module, args) in modules {
                let mut command_args = vec!["load-module", module];
                command_args.extend(args.iter().map(String::as_str));
                let id = match pactl(&command_args) {
                    Ok(id) => match id.parse::<u32>() {
                        Ok(id) => id,
                        Err(_) => {
                            for loaded_id in loaded.iter().rev() {
                                let loaded_id = loaded_id.to_string();
                                let _ = pactl(&["unload-module", &loaded_id]);
                            }
                            bail!("pactl returned an invalid module id: {id:?}");
                        }
                    },
                    Err(error) => {
                        for loaded_id in loaded.iter().rev() {
                            let loaded_id = loaded_id.to_string();
                            let _ = pactl(&["unload-module", &loaded_id]);
                        }
                        return Err(error);
                    }
                };
                loaded.push(id);
            }

            if !Self::is_enabled()? {
                for id in loaded.iter().rev() {
                    let id = id.to_string();
                    let _ = pactl(&["unload-module", &id]);
                }
                bail!("PipeWire did not expose all AirLink audio endpoints");
            }
            Ok(true)
        } else {
            for id in airlink_module_ids()?.into_iter().rev() {
                let id = id.to_string();
                pactl(&["unload-module", &id])?;
            }
            Ok(false)
        }
    }
}

use pipewire::{
    channel::Receiver,
    context::ContextRc,
    core::CoreRc,
    keys,
    main_loop::MainLoopRc,
    properties,
    spa::{
        param::audio::{AudioFormat, AudioInfoRaw},
        pod::{self, Pod, Value, serialize::PodSerializer},
        utils::Direction,
    },
    stream::{StreamFlags, StreamListener, StreamRc, StreamState},
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
