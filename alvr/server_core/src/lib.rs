mod bitrate;
mod c_api;
mod connection;
mod hand_gestures;
mod haptics;
mod input_mapping;
mod logging_backend;
mod sockets;
mod statistics;
mod tracking;
mod web_server;

pub use c_api::*;
pub use connection::align_foveation_center_shift;
pub use logging_backend::init_logging;
pub use tracking::HandType;

use crate::connection::VideoPacket;
use alvr_common::inputs::{
    CONTROLLER_PROFILE_INFO, LEFT_MENU_CLICK_ID, LEFT_SQUEEZE_VALUE_ID, LEFT_THUMBREST_TOUCH_ID,
    LEFT_THUMBSTICK_CLICK_ID, LEFT_THUMBSTICK_TOUCH_ID, LEFT_THUMBSTICK_X_ID, LEFT_THUMBSTICK_Y_ID,
    LEFT_TRIGGER_TOUCH_ID, LEFT_TRIGGER_VALUE_ID, LEFT_X_CLICK_ID, LEFT_X_TOUCH_ID,
    LEFT_Y_CLICK_ID, LEFT_Y_TOUCH_ID, QUEST_CONTROLLER_PROFILE_ID, RIGHT_A_CLICK_ID,
    RIGHT_A_TOUCH_ID, RIGHT_B_CLICK_ID, RIGHT_B_TOUCH_ID, RIGHT_SQUEEZE_VALUE_ID,
    RIGHT_SYSTEM_CLICK_ID, RIGHT_THUMBREST_TOUCH_ID, RIGHT_THUMBSTICK_CLICK_ID,
    RIGHT_THUMBSTICK_TOUCH_ID, RIGHT_THUMBSTICK_X_ID, RIGHT_THUMBSTICK_Y_ID,
    RIGHT_TRIGGER_TOUCH_ID, RIGHT_TRIGGER_VALUE_ID,
};
use alvr_common::{
    AlvrFoveatedEncodingParams, ConnectionState, DEVICE_ID_TO_PATH, DeviceMotion, Fov,
    HAND_LEFT_ID, HAND_RIGHT_ID, HEAD_ID, LifecycleState, Pose, ViewParams, dbg_server_core, error,
    glam::{Quat, UVec2, Vec2, Vec3},
    parking_lot::{Mutex, RwLock},
    settings_schema::Switch,
    warn,
};
use alvr_events::{EventType, HapticsEvent};
use alvr_filesystem as afs;
use alvr_packets::{
    BatteryInfo, ButtonEntry, ButtonValue, ClientConnectionsAction, DecoderInitializationConfig,
    Haptics, VideoPacketHeader,
};
use alvr_server_io::ServerSessionManager;
use alvr_session::{CodecType, H264Profile, OpenvrProperty, Settings, SteamvrHmdInitConfig};
use alvr_sockets::StreamSender;
use bitrate::{BitrateManager, DynamicEncoderParams};
use statistics::StatisticsManager;
use std::{
    collections::HashSet,
    env,
    ffi::OsStr,
    fs::File,
    io::Write,
    net::SocketAddr,
    sync::{
        Arc, LazyLock, OnceLock,
        atomic::{AtomicBool, Ordering},
        mpsc::{self, SyncSender, TrySendError},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};
use tokio::{runtime::Runtime, sync::broadcast};
use tracking::TrackingManager;

static FILESYSTEM_LAYOUT: OnceLock<afs::Layout> = OnceLock::new();

// This is lazily initialized when initializing logging or ServerCoreContext. So FILESYSTEM_LAYOUT
// needs to be initialized first using initialize_environment().
// NB: this must remain a global because only one instance should exist for the whole application
// execution time.
static SESSION_MANAGER: LazyLock<RwLock<ServerSessionManager>> = LazyLock::new(|| {
    RwLock::new(ServerSessionManager::new(
        FILESYSTEM_LAYOUT.get().map(|l| l.session()),
    ))
});

pub fn initialize_environment(layout: afs::Layout) {
    FILESYSTEM_LAYOUT.set(layout).unwrap();

    // This ensures that the session is written to disk
    SESSION_MANAGER.write().session_mut();
}

#[derive(Clone)]
pub struct ServerNegotiatedStreamingConfig {
    pub transcoding_view_resolution: UVec2,
    pub emulated_headset_view_resolution: UVec2,
    pub refresh_rate: f32,
    /// The rate the headset's panel really refreshes at, which paces
    /// SteamVR. A Quest 2 over Meta Air Link is a little under the nominal
    /// rate.
    pub display_refresh_rate: f32,
    pub foveated_encoding: Option<AlvrFoveatedEncodingParams>,
    pub codec: CodecType,
    pub h264_profile: H264Profile,
    pub use_10bit_encoder: bool,
    pub encoding_gamma: f32,
    pub enable_hdr: bool,
}

fn airlink_view_params() -> [ViewParams; 2] {
    let (up, down, outer, inner) = (1.1106_f32, 1.1917_f32, 1.2799_f32, 1.0_f32);
    let eye = |x: f32, left: f32, right: f32| ViewParams {
        pose: Pose {
            orientation: Quat::IDENTITY,
            position: Vec3::new(x, 0.0, 0.0),
        },
        fov: Fov {
            left: -left.atan(),
            right: right.atan(),
            up: up.atan(),
            down: -down.atan(),
        },
    };
    [eye(-0.0315, outer, inner), eye(0.0315, inner, outer)]
}

pub enum ServerCoreEvent {
    SetOpenvrProperty {
        device_id: u64,
        prop: OpenvrProperty,
    },
    ClientConnected(ServerNegotiatedStreamingConfig),
    ClientDisconnected,
    Battery(BatteryInfo),
    PlayspaceSync(Vec2),
    /// Guardian of a Meta Air Link headset: outline on the floor (x, z in
    /// metres, floor-relative tracking space) and play area size.
    AirLinkGuardian {
        perimeter: Vec<Vec2>,
        play_area: Option<Vec2>,
    },
    LocalViewParams([ViewParams; 2]), // In relation to head
    Tracking {
        poll_timestamp: Duration,
    },
    Buttons(Vec<ButtonEntry>), // Note: this is after mapping
    RequestIDR,
    GameRenderLatencyFeedback(Duration), // only used for SteamVR
    ShutdownPending,
    RestartPending,
    ProximityState(bool),
}

pub struct ConnectionContext {
    events_sender: mpsc::Sender<ServerCoreEvent>,
    statistics_manager: RwLock<Option<StatisticsManager>>,
    bitrate_manager: Mutex<BitrateManager>,
    tracking_manager: RwLock<TrackingManager>,
    decoder_config: Mutex<Option<DecoderInitializationConfig>>,
    video_mirror_sender: Mutex<Option<broadcast::Sender<Vec<u8>>>>,
    video_recording_file: Mutex<Option<File>>,
    connection_threads: Mutex<Vec<JoinHandle<()>>>,
    clients_to_be_removed: Mutex<HashSet<String>>,
    video_channel_sender: Mutex<Option<SyncSender<VideoPacket>>>,
    airlink_video_sender: broadcast::Sender<alvr_airlink::video::AirLinkVideoFrame>,
    haptics_sender: Mutex<Option<StreamSender<Haptics>>>,
}

pub fn create_recording_file(connection_context: &ConnectionContext, settings: &Settings) {
    let codec = settings.video.preferred_codec;
    let ext = match codec {
        CodecType::H264 => "h264",
        CodecType::Hevc => "h265",
        CodecType::AV1 => "av1",
    };

    let path = FILESYSTEM_LAYOUT.get().unwrap().log_dir.join(format!(
        "recording.{}.{ext}",
        chrono::Local::now().format("%F.%H-%M-%S")
    ));

    match File::create(path) {
        Ok(mut file) => {
            if let Some(config) = &*connection_context.decoder_config.lock() {
                file.write_all(&config.config_buffer).ok();
            }

            *connection_context.video_recording_file.lock() = Some(file);

            connection_context
                .events_sender
                .send(ServerCoreEvent::RequestIDR)
                .ok();
        }
        Err(e) => {
            error!("Failed to record video on disk: {e}");
        }
    }
}

pub fn notify_restart_driver() {
    if sysinfo::System::new_all()
        .processes_by_name(OsStr::new(&afs::dashboard_fname()))
        .next()
        .is_some()
    {
        alvr_events::send_event(EventType::ServerRequestsSelfRestart);
    } else {
        error!("Cannot restart SteamVR. No dashboard process found on local device.");
    }
}

pub fn settings() -> Settings {
    SESSION_MANAGER.read().settings().clone()
}

pub fn steamvr_hmd_init_config() -> SteamvrHmdInitConfig {
    SESSION_MANAGER
        .read()
        .session()
        .steamvr_hmd_init_config
        .clone()
}

pub fn registered_button_set() -> HashSet<u64> {
    let session_manager = SESSION_MANAGER.read();
    if let Switch::Enabled(input_mapping) = &session_manager.settings().headset.controllers {
        input_mapping::registered_button_set(&input_mapping.emulation_mode)
    } else {
        HashSet::new()
    }
}

pub struct ServerCoreContext {
    lifecycle_state: Arc<RwLock<LifecycleState>>,
    connection_context: Arc<ConnectionContext>,
    connection_thread: Arc<RwLock<Option<JoinHandle<()>>>>,
    airlink_shutdown: Mutex<Option<tokio::sync::oneshot::Sender<()>>>,
    airlink_thread: Mutex<Option<JoinHandle<()>>>,
    webserver_runtime: Option<Runtime>,
}

impl ServerCoreContext {
    pub fn new() -> (Self, mpsc::Receiver<ServerCoreEvent>) {
        dbg_server_core!("Creating");

        if SESSION_MANAGER
            .read()
            .settings()
            .extra
            .logging
            .prefer_backtrace
        {
            unsafe { env::set_var("RUST_BACKTRACE", "1") };
        }

        SESSION_MANAGER.write().clean_client_list();

        let (events_sender, events_receiver) = mpsc::channel();
        let (airlink_video_sender, _) = broadcast::channel(64);

        // Create a temporary StatisticsManager until a headset connects
        let initial_settings = SESSION_MANAGER.read().settings().clone();
        let stats = StatisticsManager::new(
            initial_settings.connection.statistics_history_size,
            Duration::from_secs_f32(1.0 / 90.0),
            if let Switch::Enabled(config) = &initial_settings.headset.controllers {
                config.steamvr_pipeline_frames
            } else {
                0.0
            },
        );

        let connection_context = Arc::new(ConnectionContext {
            events_sender,
            statistics_manager: RwLock::new(Some(stats)),
            bitrate_manager: Mutex::new(BitrateManager::new(256, 60.0)),
            tracking_manager: RwLock::new(TrackingManager::new(
                initial_settings.connection.statistics_history_size,
            )),
            decoder_config: Mutex::new(None),
            video_mirror_sender: Mutex::new(None),
            video_recording_file: Mutex::new(None),
            connection_threads: Mutex::new(Vec::new()),
            clients_to_be_removed: Mutex::new(HashSet::new()),
            video_channel_sender: Mutex::new(None),
            airlink_video_sender,
            haptics_sender: Mutex::new(None),
        });

        let webserver_runtime = Runtime::new().unwrap();
        webserver_runtime.spawn({
            let connection_context = Arc::clone(&connection_context);
            async move { alvr_common::show_err(web_server::web_server(connection_context).await) }
        });

        (
            Self {
                lifecycle_state: Arc::new(RwLock::new(LifecycleState::StartingUp)),
                connection_context,
                connection_thread: Arc::new(RwLock::new(None)),
                airlink_shutdown: Mutex::new(None),
                airlink_thread: Mutex::new(None),
                webserver_runtime: Some(webserver_runtime),
            },
            events_receiver,
        )
    }

    pub fn start_connection(&self) {
        dbg_server_core!("start_connection");

        // Note: Idle state is not used on the server side
        *self.lifecycle_state.write() = LifecycleState::Resumed;

        self.start_airlink_host();

        let connection_context = Arc::clone(&self.connection_context);
        let lifecycle_state = Arc::clone(&self.lifecycle_state);
        *self.connection_thread.write() = Some(thread::spawn(move || {
            connection::handshake_loop(connection_context, lifecycle_state);
        }));
    }

    fn start_airlink_host(&self) {
        if self.airlink_thread.lock().is_some() {
            return;
        }

        let Some(layout) = FILESYSTEM_LAYOUT.get() else {
            error!("Cannot start Air Link host before ALVR environment initialization");
            return;
        };
        let state_dir = layout.config_dir.join("airlink");
        // The name the headset lists: the one set by the user, else the
        // computer's. SteamVR's container has no HOSTNAME in its environment.
        let hostname = Some(
            SESSION_MANAGER
                .read()
                .settings()
                .connection
                .airlink_host_name
                .trim()
                .to_owned(),
        )
        .filter(|name| !name.is_empty())
        .or_else(|| {
            std::fs::read_to_string("/proc/sys/kernel/hostname")
                .ok()
                .map(|name| name.trim().to_owned())
                .filter(|name| !name.is_empty())
        })
        .or_else(|| env::var("HOSTNAME").ok())
        .unwrap_or_else(|| "alvr-host".to_owned());
        let flavor = env::var("USER").unwrap_or_else(|_| "user".to_owned());
        let endpoint_name = format!("{flavor}::{hostname}");
        let video_receiver = self.connection_context.airlink_video_sender.subscribe();
        let audio = airlink_audio();
        let settings = SESSION_MANAGER.read().settings().clone();
        let started_sender = self.connection_context.events_sender.clone();
        let started_context = Arc::clone(&self.connection_context);
        let on_streaming_started = Arc::new(move || {
            // Built at every stream start, so setting changes apply on re-entry.
            let negotiated_config =
                connection::airlink_streaming_config(SESSION_MANAGER.read().settings());
            // The statistics manager also paces SteamVR's frames (its virtual
            // vsync). The one made at start-up runs at 90 Hz; left in place
            // the PC produced about 74 frames per second for a 72 Hz
            // headset, whose frame queue overflowed all the time.
            {
                let session_manager = SESSION_MANAGER.read();
                let settings = session_manager.settings();
                *started_context.statistics_manager.write() = Some(StatisticsManager::new(
                    settings.connection.statistics_history_size,
                    // At the headset's real refresh rate, a little under the
                    // nominal one: at exactly 72 Hz a frame too many arrives
                    // every six seconds and the headset repeats frames in waves.
                    Duration::from_secs_f64(alvr_airlink::video::frame_period_s()),
                    if let Switch::Enabled(config) = &settings.headset.controllers {
                        config.steamvr_pipeline_frames
                    } else {
                        0.0
                    },
                ));
            }
            started_sender
                .send(ServerCoreEvent::ClientConnected(negotiated_config))
                .ok();
            started_sender
                .send(ServerCoreEvent::LocalViewParams(airlink_view_params()))
                .ok();
            started_sender
                .send(ServerCoreEvent::ProximityState(true))
                .ok();
            AIRLINK_STREAMING.store(true, Ordering::Relaxed);
            alvr_events::send_event(EventType::AirLinkStreaming(true));
        });
        let keyframe_sender = self.connection_context.events_sender.clone();
        let on_keyframe_needed = Arc::new(move || {
            keyframe_sender.send(ServerCoreEvent::RequestIDR).ok();
        });
        let guardian_sender = self.connection_context.events_sender.clone();
        let on_guardian_update =
            Arc::new(move |guardian: alvr_airlink::guardian::AirLinkGuardian| {
                guardian_sender
                    .send(ServerCoreEvent::AirLinkGuardian {
                        perimeter: guardian
                            .perimeter
                            .iter()
                            .map(|p| Vec2::from_array(*p))
                            .collect(),
                        play_area: guardian.play_area_or_extent().map(Vec2::from_array),
                    })
                    .ok();
            });
        let stopped_sender = self.connection_context.events_sender.clone();
        let on_streaming_stopped = Arc::new(move || {
            stopped_sender
                .send(ServerCoreEvent::ProximityState(false))
                .ok();
            stopped_sender
                .send(ServerCoreEvent::ClientDisconnected)
                .ok();
            AIRLINK_STREAMING.store(false, Ordering::Relaxed);
            alvr_events::send_event(EventType::AirLinkStreaming(false));
        });
        let tracking_context = Arc::clone(&self.connection_context);
        let tracking_events = self.connection_context.events_sender.clone();
        let tracking_origin = Instant::now();
        let headset_settings = settings.headset.clone();
        // Air Link reports controller poses only: their velocities are
        // measured here so ALVR's controller prediction works as for its client.
        let controller_velocities = Mutex::new([
            alvr_airlink::tracking::VelocityEstimator::default(),
            alvr_airlink::tracking::VelocityEstimator::default(),
        ]);
        let on_tracking_update = Arc::new(
            move |sample: alvr_airlink::tracking::AirLinkTrackingSample| {
                let pose = |pose: alvr_airlink::tracking::AirLinkPose| Pose {
                    orientation: Quat::from_xyzw(
                        pose.orientation_xyzw[0],
                        pose.orientation_xyzw[1],
                        pose.orientation_xyzw[2],
                        pose.orientation_xyzw[3],
                    )
                    .normalize(),
                    position: Vec3::from_array(pose.position_xyz),
                };
                let timestamp = tracking_origin.elapsed();
                let mut motions = vec![(
                    *HEAD_ID,
                    DeviceMotion {
                        pose: pose(sample.head),
                        ..DeviceMotion::default()
                    },
                )];
                // A controller that is asleep or out of sight is left out:
                // SteamVR then shows it as not tracked.
                let mut velocities = controller_velocities.lock();
                for ((device_id, hand), estimator) in [
                    (*HAND_LEFT_ID, sample.left_hand),
                    (*HAND_RIGHT_ID, sample.right_hand),
                ]
                .into_iter()
                .zip(velocities.iter_mut())
                {
                    if let Some(hand) = hand {
                        let (linear, angular) = estimator.update(hand, sample.head_time_s);
                        motions.push((
                            device_id,
                            DeviceMotion {
                                pose: pose(hand),
                                linear_velocity: Vec3::from_array(linear),
                                angular_velocity: Vec3::from_array(angular),
                            },
                        ));
                    } else {
                        estimator.reset();
                    }
                }
                drop(velocities);
                tracking_context
                    .tracking_manager
                    .write()
                    .report_device_motions(&headset_settings, timestamp, &motions);
                if let Some(stats) = &mut *tracking_context.statistics_manager.write() {
                    stats.report_tracking_received(timestamp);
                }
                tracking_events
                    .send(ServerCoreEvent::Tracking {
                        poll_timestamp: timestamp,
                    })
                    .ok();
            },
        );

        let button_mapper = if let Switch::Enabled(config) = &settings.headset.controllers {
            let source_buttons = CONTROLLER_PROFILE_INFO
                .get(&QUEST_CONTROLLER_PROFILE_ID)
                .map(|profile| profile.button_set.clone())
                .unwrap_or_default();
            Some(if let Some(mappings) = &config.button_mappings {
                input_mapping::ButtonMappingManager::new_manual(mappings)
            } else {
                input_mapping::ButtonMappingManager::new_automatic(
                    &source_buttons,
                    &config.emulation_mode,
                    &config.button_mapping_config,
                )
            })
        } else {
            None
        };
        let button_mapper = Arc::new(Mutex::new(button_mapper));
        let button_events = self.connection_context.events_sender.clone();
        let battery_events = self.connection_context.events_sender.clone();
        let battery_context = Arc::clone(&self.connection_context);
        // Last reported charges and when: sent on change and every 30 s.
        let battery_reported = Mutex::new(([u8::MAX; 3], Instant::now()));
        let on_controller_input = Arc::new(
            move |input: alvr_airlink::tracking::AirLinkControllerInput| {
                {
                    let mut reported = battery_reported.lock();
                    if reported.0 != input.battery_percent
                        || reported.1.elapsed() > Duration::from_secs(30)
                    {
                        *reported = (input.battery_percent, Instant::now());
                        let devices = [*HAND_LEFT_ID, *HAND_RIGHT_ID, *HEAD_ID];
                        for (device_id, percent) in devices.into_iter().zip(input.battery_percent) {
                            if percent <= 100 {
                                if let Some(stats) =
                                    &mut *battery_context.statistics_manager.write()
                                {
                                    stats.report_battery(
                                        device_id,
                                        f32::from(percent) / 100.0,
                                        false,
                                    );
                                }
                                battery_events
                                    .send(ServerCoreEvent::Battery(BatteryInfo {
                                        device_id,
                                        gauge_value: f32::from(percent) / 100.0,
                                        is_plugged: false,
                                    }))
                                    .ok();
                            }
                        }
                    }
                }
                let binary = |path_id, value| ButtonEntry {
                    path_id,
                    value: ButtonValue::Binary(value),
                };
                let scalar = |path_id, value| ButtonEntry {
                    path_id,
                    value: ButtonValue::Scalar(value),
                };
                let left = input.left;
                let right = input.right;
                let entries = [
                    binary(*LEFT_X_CLICK_ID, left.buttons & 0x100 != 0),
                    binary(*LEFT_X_TOUCH_ID, left.touches & 1 != 0),
                    binary(*LEFT_Y_CLICK_ID, left.buttons & 0x200 != 0),
                    binary(*LEFT_Y_TOUCH_ID, left.touches & 2 != 0),
                    binary(*LEFT_MENU_CLICK_ID, left.buttons & 0x4000_0000 != 0),
                    scalar(*LEFT_SQUEEZE_VALUE_ID, left.grip),
                    scalar(*LEFT_TRIGGER_VALUE_ID, left.trigger),
                    binary(*LEFT_TRIGGER_TOUCH_ID, left.touches & 0x10 != 0),
                    scalar(*LEFT_THUMBSTICK_X_ID, left.thumbstick_xy[0]),
                    scalar(*LEFT_THUMBSTICK_Y_ID, left.thumbstick_xy[1]),
                    binary(*LEFT_THUMBSTICK_CLICK_ID, left.buttons & 0x400 != 0),
                    binary(*LEFT_THUMBSTICK_TOUCH_ID, left.touches & 4 != 0),
                    binary(*LEFT_THUMBREST_TOUCH_ID, left.touches & 8 != 0),
                    binary(*RIGHT_A_CLICK_ID, right.buttons & 1 != 0),
                    binary(*RIGHT_A_TOUCH_ID, right.touches & 1 != 0),
                    binary(*RIGHT_B_CLICK_ID, right.buttons & 2 != 0),
                    binary(*RIGHT_B_TOUCH_ID, right.touches & 2 != 0),
                    binary(*RIGHT_SYSTEM_CLICK_ID, right.buttons & 0x0100_0000 != 0),
                    scalar(*RIGHT_SQUEEZE_VALUE_ID, right.grip),
                    scalar(*RIGHT_TRIGGER_VALUE_ID, right.trigger),
                    binary(*RIGHT_TRIGGER_TOUCH_ID, right.touches & 0x10 != 0),
                    scalar(*RIGHT_THUMBSTICK_X_ID, right.thumbstick_xy[0]),
                    scalar(*RIGHT_THUMBSTICK_Y_ID, right.thumbstick_xy[1]),
                    binary(*RIGHT_THUMBSTICK_CLICK_ID, right.buttons & 4 != 0),
                    binary(*RIGHT_THUMBSTICK_TOUCH_ID, right.touches & 4 != 0),
                    binary(*RIGHT_THUMBREST_TOUCH_ID, right.touches & 8 != 0),
                ];
                let mapped = button_mapper
                    .lock()
                    .as_mut()
                    .map(|mapper| {
                        entries
                            .into_iter()
                            .flat_map(|entry| mapper.map_button(&entry))
                            .collect::<Vec<_>>()
                    })
                    .unwrap_or_default();
                if !mapped.is_empty() {
                    button_events.send(ServerCoreEvent::Buttons(mapped)).ok();
                }
            },
        );
        let advertise_address = alvr_system_info::local_ip();
        if advertise_address.is_unspecified() {
            error!("Cannot advertise Air Link host: local IP address is unspecified");
            return;
        }

        let (shutdown_sender, shutdown_receiver) = tokio::sync::oneshot::channel();
        *self.airlink_shutdown.lock() = Some(shutdown_sender);
        *self.airlink_thread.lock() = Some(thread::spawn(move || {
            let runtime = match Runtime::new() {
                Ok(runtime) => runtime,
                Err(error) => {
                    error!("Cannot start Air Link runtime: {error}");
                    return;
                }
            };
            runtime.block_on(async move {
                let address = SocketAddr::from(([0, 0, 0, 0], 0));
                let host = match alvr_airlink::host::AirLinkHost::bind(
                    state_dir,
                    address,
                    advertise_address,
                    &hostname,
                    &endpoint_name,
                    video_receiver,
                    audio,
                    on_streaming_started,
                    on_streaming_stopped,
                    on_tracking_update,
                    on_controller_input,
                    on_keyframe_needed,
                    on_guardian_update,
                )
                .await
                {
                    Ok(host) => host,
                    Err(error) => {
                        error!("Cannot start Meta Air Link host: {error}");
                        return;
                    }
                };
                // Follows the host's switch in the settings and tells the
                // dashboard which headset is connected: at every change, and
                // every few seconds for a dashboard opened meanwhile.
                tokio::spawn(async {
                    let mut reported = None;
                    let mut ticks_since_report = 0u32;
                    let mut pairing_reported = None;
                    let mut ticks_since_pairing_report = 0u32;
                    loop {
                        {
                            let session_manager = SESSION_MANAGER.read();
                            let audio = &session_manager.settings().audio;
                            AIRLINK_GAME_AUDIO.store(audio.game_audio.enabled(), Ordering::Relaxed);
                            AIRLINK_MICROPHONE.store(audio.microphone.enabled(), Ordering::Relaxed);
                        }
                        // The headsets the user accepted are the trusted
                        // entries of the client list that carry an Air Link id.
                        alvr_airlink::host::set_known_headsets(
                            SESSION_MANAGER
                                .read()
                                .client_list()
                                .iter()
                                .filter(|(_, client)| client.trusted)
                                .filter_map(|(name, _)| airlink_headset_id(name))
                                .collect(),
                        );
                        let pairing = alvr_airlink::host::pairing_request();
                        ticks_since_pairing_report += 1;
                        if pairing != pairing_reported
                            || (pairing.is_some() && ticks_since_pairing_report >= 3)
                        {
                            alvr_events::send_event(EventType::AirLinkPairing {
                                address: pairing
                                    .as_ref()
                                    .map(|request| request.address.ip().to_string()),
                                headset: pairing
                                    .as_ref()
                                    .map(|request| request.headset_name.clone()),
                                pairing_code: pairing
                                    .as_ref()
                                    .and_then(|request| request.pairing_code),
                            });
                            pairing_reported = pairing;
                            ticks_since_pairing_report = 0;
                        }
                        alvr_airlink::host::set_enabled(
                            SESSION_MANAGER.read().settings().connection.airlink_host,
                        );
                        let status = alvr_airlink::host::headset_status();
                        ticks_since_report += 1;
                        if status != reported || (status.is_some() && ticks_since_report >= 3) {
                            alvr_events::send_event(EventType::AirLinkHeadset {
                                address: status.map(|status| status.address.ip().to_string()),
                                streaming: status.is_some_and(|status| status.streaming),
                            });
                            reported = status;
                            ticks_since_report = 0;
                        }
                        tokio::time::sleep(Duration::from_secs(1)).await;
                    }
                });
                if let Err(error) = host.run(shutdown_receiver).await {
                    error!("Meta Air Link host stopped: {error}");
                }
            });
        }));
    }

    pub fn get_device_motion(
        &self,
        device_id: u64,
        sample_timestamp: Duration,
    ) -> Option<DeviceMotion> {
        dbg_server_core!("get_device_motion: dev={device_id} sample_ts={sample_timestamp:?}");

        self.connection_context
            .tracking_manager
            .read()
            .get_device_motion(device_id, sample_timestamp)
    }

    pub fn get_hand_skeleton(
        &self,
        hand_type: HandType,
        timestamp: Duration,
    ) -> Option<[Pose; 26]> {
        dbg_server_core!("get_hand_skeleton: hand={hand_type:?} ts={timestamp:?}");

        self.connection_context
            .tracking_manager
            .read()
            .get_hand_skeleton(hand_type, timestamp)
            .copied()
    }

    /// Return head-local gaze for an exact retained tracking timestamp, with -Z along the gaze.
    /// Returns None if the sample has no gaze or is no longer in the bounded history.
    pub fn get_combined_eye_gaze(&self, sample_timestamp: Duration) -> Option<Quat> {
        dbg_server_core!("get_combined_eye_gaze: sample_ts={sample_timestamp:?}");

        self.connection_context
            .tracking_manager
            .read()
            .get_combined_eye_gaze(sample_timestamp)
    }

    pub fn get_motion_to_photon_latency(&self) -> Duration {
        dbg_server_core!("get_motion_to_photon_latency");

        let latency = self
            .connection_context
            .statistics_manager
            .read()
            .as_ref()
            .map(|stats| stats.motion_to_photon_latency_average())
            .unwrap_or_default();

        let max_prediction =
            Duration::from_millis(SESSION_MANAGER.read().settings().headset.max_prediction_ms);

        if latency > max_prediction {
            warn!("Latency is too high. Clamping prediction");

            max_prediction
        } else {
            latency
        }
    }

    pub fn get_tracker_pose_time_offset(&self) -> Duration {
        dbg_server_core!("get_tracker_pose_time_offset");

        self.connection_context
            .statistics_manager
            .read()
            .as_ref()
            .map(|stats| stats.tracker_pose_time_offset())
            .unwrap_or_default()
    }

    pub fn send_haptics(&self, haptics: Haptics) {
        dbg_server_core!("send_haptics");

        let haptics_config = {
            let session_manager_lock = SESSION_MANAGER.read();

            if session_manager_lock.settings().extra.logging.log_haptics {
                alvr_events::send_event(EventType::Haptics(HapticsEvent {
                    path: DEVICE_ID_TO_PATH.get(&haptics.device_id).map_or_else(
                        || format!("Unknown (ID: {:#16x})", haptics.device_id),
                        |p| (*p).to_owned(),
                    ),
                    duration: haptics.duration,
                    frequency: haptics.frequency,
                    amplitude: haptics.amplitude,
                }))
            }

            session_manager_lock
                .settings()
                .headset
                .controllers
                .as_option()
                .and_then(|c| c.haptics.as_option().cloned())
        };

        let Some(config) = haptics_config else {
            return;
        };
        let haptics = haptics::map_haptics(&config, haptics);

        // An Air Link headset takes them through its own host.
        let airlink_hand = if haptics.device_id == *HAND_LEFT_ID {
            Some(alvr_airlink::haptics::AirLinkHand::Left)
        } else if haptics.device_id == *HAND_RIGHT_ID {
            Some(alvr_airlink::haptics::AirLinkHand::Right)
        } else {
            None
        };
        if let Some(hand) = airlink_hand {
            alvr_airlink::haptics::send_haptic(alvr_airlink::haptics::AirLinkHaptic {
                hand,
                amplitude: haptics.amplitude,
                frequency: 1.0,
                duration: haptics.duration,
            });
        }

        if let Some(sender) = &mut *self.connection_context.haptics_sender.lock() {
            sender.send_header(&haptics).ok();
        }
    }

    pub fn set_video_config_nals(&self, config_buffer: Vec<u8>, codec: CodecType) {
        dbg_server_core!("set_video_config_nals");

        if let Some(sender) = &*self.connection_context.video_mirror_sender.lock() {
            sender.send(config_buffer.clone()).ok();
        }

        if let Some(file) = &mut *self.connection_context.video_recording_file.lock() {
            file.write_all(&config_buffer).ok();
        }

        *self.connection_context.decoder_config.lock() = Some(DecoderInitializationConfig {
            codec,
            config_buffer,
            ext_str: String::new(),
        });
    }

    pub fn send_video_nal(
        &self,
        timestamp: Duration,
        global_view_params: [ViewParams; 2],
        foveation_center_shifts: Option<[[f32; 2]; 2]>,
        is_idr: bool,
        nal_buffer: Vec<u8>,
    ) {
        dbg_server_core!("send_video_nal");

        static AIRLINK_STREAM_CORRUPTED: AtomicBool = AtomicBool::new(true);
        if is_idr {
            AIRLINK_STREAM_CORRUPTED.store(false, Ordering::SeqCst);
        }
        if !AIRLINK_STREAM_CORRUPTED.load(Ordering::SeqCst) {
            let pose = self
                .connection_context
                .tracking_manager
                .read()
                .get_device_motion(*HEAD_ID, timestamp)
                .map_or(global_view_params[0].pose, |motion| motion.pose);
            let position = pose.position.to_array();
            let orientation = pose.orientation.to_array();
            // The encoder's parameter sets were split off the keyframe and
            // stored as the decoder config; the headset needs them in band.
            let mut nal = Vec::new();
            if is_idr && let Some(config) = &*self.connection_context.decoder_config.lock() {
                nal = alvr_airlink::video::without_delimiters_and_sei(&config.config_buffer);
            }
            nal.extend(alvr_airlink::video::without_delimiters_and_sei(&nal_buffer));
            let frame = alvr_airlink::video::AirLinkVideoFrame {
                is_idr,
                nal,
                head_pose: [
                    orientation[0],
                    orientation[1],
                    orientation[2],
                    orientation[3],
                    position[0],
                    position[1],
                    position[2],
                ],
            };
            // Debugging aid: `~/.config/alvr/airlink_video_dump` names a file
            // that receives the stream exactly as it is handed to the host.
            // Next to it, `<file>.times` gets one line per frame: the wall
            // clock in milliseconds, to find a moment in the dump.
            static DUMP: LazyLock<Option<Mutex<(std::fs::File, std::fs::File)>>> =
                LazyLock::new(|| {
                    let home = std::env::var_os("HOME")?;
                    let path = std::fs::read_to_string(
                        std::path::Path::new(&home).join(".config/alvr/airlink_video_dump"),
                    )
                    .ok()?;
                    let path = path.trim();
                    Some(Mutex::new((
                        std::fs::File::create(path).ok()?,
                        std::fs::File::create(format!("{path}.times")).ok()?,
                    )))
                });
            if let Some(files) = &*DUMP {
                use std::io::Write;
                let mut files = files.lock();
                files.0.write_all(&frame.nal).ok();
                let now_ms = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map_or(0, |elapsed| elapsed.as_millis());
                writeln!(files.1, "{now_ms}").ok();
            }
            self.connection_context
                .airlink_video_sender
                .send(frame)
                .ok();

            // An Air Link headset sends no statistics of its own, and the
            // code below, for ALVR's client, does not run without its stream.
            if self
                .connection_context
                .video_channel_sender
                .lock()
                .is_none()
                && let Some(stats) = &mut *self.connection_context.statistics_manager.write()
            {
                stats.report_frame_encoded(timestamp, nal_buffer.len());
                stats.report_frame_sent_without_client_statistics(timestamp);
            }
        }

        // start in the corrupts state, the client didn't receive the initial IDR yet.
        static STREAM_CORRUPTED: AtomicBool = AtomicBool::new(true);
        static LAST_IDR_INSTANT: LazyLock<Mutex<Instant>> =
            LazyLock::new(|| Mutex::new(Instant::now()));

        if let Some(sender) = &*self.connection_context.video_channel_sender.lock() {
            let buffer_size = nal_buffer.len();

            if is_idr {
                STREAM_CORRUPTED.store(false, Ordering::SeqCst);
            }

            if let Switch::Enabled(config) = &SESSION_MANAGER
                .read()
                .settings()
                .extra
                .capture
                .rolling_video_files
                && Instant::now()
                    > *LAST_IDR_INSTANT.lock() + Duration::from_secs(config.duration_s)
            {
                self.connection_context
                    .events_sender
                    .send(ServerCoreEvent::RequestIDR)
                    .ok();

                if is_idr {
                    create_recording_file(
                        &self.connection_context,
                        SESSION_MANAGER.read().settings(),
                    );
                    *LAST_IDR_INSTANT.lock() = Instant::now();
                }
            }

            if !STREAM_CORRUPTED.load(Ordering::SeqCst)
                || !SESSION_MANAGER
                    .read()
                    .settings()
                    .connection
                    .avoid_video_glitching
            {
                if let Some(sender) = &*self.connection_context.video_mirror_sender.lock() {
                    sender.send(nal_buffer.clone()).ok();
                }

                if let Some(file) = &mut *self.connection_context.video_recording_file.lock() {
                    file.write_all(&nal_buffer).ok();
                }

                let sender_result = sender.try_send(VideoPacket {
                    header: VideoPacketHeader {
                        timestamp,
                        global_view_params,
                        foveation_center_shifts,
                        is_idr,
                    },
                    payload: nal_buffer,
                });
                if matches!(sender_result, Err(TrySendError::Full(_))) {
                    STREAM_CORRUPTED.store(true, Ordering::SeqCst);
                    self.connection_context
                        .events_sender
                        .send(ServerCoreEvent::RequestIDR)
                        .ok();
                    warn!("Dropping video packet. Reason: Can't push to network");
                }
            } else {
                warn!("Dropping video packet. Reason: Waiting for IDR frame");
            }

            if let Some(stats) = &mut *self.connection_context.statistics_manager.write() {
                let encoder_latency = stats.report_frame_encoded(timestamp, buffer_size);

                self.connection_context
                    .bitrate_manager
                    .lock()
                    .report_frame_encoded(timestamp, encoder_latency, buffer_size);
            }
        }
    }

    pub fn get_dynamic_encoder_params(&self) -> Option<DynamicEncoderParams> {
        dbg_server_core!("get_dynamic_encoder_params");

        let pair = {
            let session_manager_lock = SESSION_MANAGER.read();
            self.connection_context
                .bitrate_manager
                .lock()
                .get_encoder_params(&session_manager_lock.settings().video.bitrate)
        };

        pair.map(|(params, stats)| {
            if let Some(stats_manager) = &mut *self.connection_context.statistics_manager.write() {
                stats_manager.report_throughput_stats(stats);
            }
            params
        })
    }

    pub fn report_composed(&self, target_timestamp: Duration, offset: Duration) {
        dbg_server_core!("report_composed");

        if let Some(stats) = &mut *self.connection_context.statistics_manager.write() {
            stats.report_frame_composed(target_timestamp, offset);
        }
    }

    pub fn report_present(&self, target_timestamp: Duration, offset: Duration) {
        dbg_server_core!("report_present");

        if let Some(stats) = &mut *self.connection_context.statistics_manager.write() {
            stats.report_frame_present(target_timestamp, offset);
        }

        let session_manager_lock = SESSION_MANAGER.read();
        self.connection_context
            .bitrate_manager
            .lock()
            .report_frame_present(
                &session_manager_lock
                    .settings()
                    .video
                    .bitrate
                    .adapt_to_framerate,
            );
    }

    pub fn duration_until_next_vsync(&self) -> Option<Duration> {
        dbg_server_core!("duration_until_next_vsync");

        self.connection_context
            .statistics_manager
            .write()
            .as_mut()
            .map(|stats| stats.duration_until_next_vsync())
    }
}

impl Drop for ServerCoreContext {
    fn drop(&mut self) {
        dbg_server_core!("Drop");

        // Invoke connection runtimes shutdown
        *self.lifecycle_state.write() = LifecycleState::ShuttingDown;

        if let Some(shutdown) = self.airlink_shutdown.lock().take() {
            shutdown.send(()).ok();
        }
        if let Some(thread) = self.airlink_thread.lock().take() {
            dbg_server_core!("Joining Meta Air Link host thread");
            thread.join().ok();
        }

        dbg_server_core!("Setting clients as Disconnecting");
        {
            let mut session_manager_lock = SESSION_MANAGER.write();

            let hostnames = session_manager_lock
                .client_list()
                .iter()
                .filter(|&(_, info)| {
                    !matches!(
                        info.connection_state,
                        ConnectionState::Disconnected | ConnectionState::Disconnecting
                    )
                })
                .map(|(hostname, _)| hostname.clone())
                .collect::<Vec<_>>();

            for hostname in hostnames {
                session_manager_lock.update_client_connections(
                    hostname,
                    ClientConnectionsAction::SetConnectionState(ConnectionState::Disconnecting),
                );
            }
        }

        dbg_server_core!("Joining connection thread");
        if let Some(thread) = self.connection_thread.write().take() {
            thread.join().ok();
        }

        // apply openvr config for the next launch
        dbg_server_core!("Setting restart settings cache");
        {
            let mut session_manager_lock = SESSION_MANAGER.write();
            let new_steamvr_hmd_init_config = session_manager_lock
                .session()
                .steamvr_hmd_init_config
                .clone();
            let settings = session_manager_lock.session().to_settings();
            let new_hash =
                connection::compute_restart_settings_hash(&new_steamvr_hmd_init_config, &settings);
            let mut session = session_manager_lock.session_mut();
            session.steamvr_hmd_init_config = new_steamvr_hmd_init_config;
            session.restart_settings_hash = new_hash;
        }

        // todo: check if this is still needed
        while SESSION_MANAGER
            .read()
            .client_list()
            .iter()
            .any(|(_, info)| info.connection_state != ConnectionState::Disconnected)
        {
            thread::sleep(Duration::from_millis(100));
        }

        self.webserver_runtime.take();
    }
}

// "Headset speaker" and "Headset microphone" of the settings, for Air Link.
static AIRLINK_GAME_AUDIO: AtomicBool = AtomicBool::new(true);
static AIRLINK_MICROPHONE: AtomicBool = AtomicBool::new(true);
static AIRLINK_STREAMING: AtomicBool = AtomicBool::new(false);

/// Game audio and microphone for a Meta Air Link headset, through the same
/// "ALVR Audio" and "ALVR Microphone" devices ALVR's client gets: what plays
/// on the first is sent to the headset, the headset's microphone feeds the
/// second. As with the client, the devices exist while the headset streams.
/// The audio thread ends with the host.
#[cfg(target_os = "linux")]
fn airlink_audio() -> alvr_airlink::audio::AirLinkAudio {
    use alvr_airlink::audio::{SAMPLE_RATE, SPEAKER_CHUNK_BYTES};
    use std::collections::VecDeque;

    // Chunks the host has not taken are dropped: 80 ms may wait.
    let (speaker_sender, speaker_receiver) = tokio::sync::mpsc::channel::<Vec<u8>>(8);
    let microphone_queue = Arc::new(Mutex::new(VecDeque::<f32>::new()));

    let on_microphone = Arc::new({
        let queue = Arc::clone(&microphone_queue);
        move |samples: &[u8]| {
            if !AIRLINK_MICROPHONE.load(Ordering::Relaxed) {
                return;
            }
            let mut queue = queue.lock();
            queue.extend(
                samples
                    .chunks_exact(2)
                    .map(|pair| f32::from(i16::from_le_bytes([pair[0], pair[1]])) / 32768.0),
            );
            // Nobody records from the microphone, or playback fell behind:
            // keep the newest 200 ms.
            let limit = SAMPLE_RATE as usize / 5;
            if queue.len() > limit {
                let excess = queue.len() - limit;
                queue.drain(..excess);
            }
        }
    });

    thread::spawn({
        let is_running_sender = speaker_sender.clone();
        move || {
            while !is_running_sender.is_closed() {
                if !AIRLINK_STREAMING.load(Ordering::Relaxed) {
                    thread::sleep(Duration::from_millis(200));
                    continue;
                }
                let speaker_sender = speaker_sender.clone();
                let mut pending = Vec::<u8>::new();
                // Silence is sent for a second after the last sound, then
                // nothing.
                let mut silent_chunks = 0u32;
                alvr_audio::linux::airlink_audio_loop(
                    || !is_running_sender.is_closed() && AIRLINK_STREAMING.load(Ordering::Relaxed),
                    move |samples| {
                        pending.extend_from_slice(samples);
                        while pending.len() >= SPEAKER_CHUNK_BYTES {
                            let rest = pending.split_off(SPEAKER_CHUNK_BYTES);
                            let chunk = std::mem::replace(&mut pending, rest);
                            if chunk.iter().all(|byte| *byte == 0) {
                                silent_chunks = silent_chunks.saturating_add(1);
                            } else {
                                silent_chunks = 0;
                            }
                            if silent_chunks <= 100 && AIRLINK_GAME_AUDIO.load(Ordering::Relaxed) {
                                speaker_sender.try_send(chunk).ok();
                            }
                        }
                    },
                    Arc::clone(&microphone_queue),
                );
            }
        }
    });

    alvr_airlink::audio::AirLinkAudio {
        speaker_receiver,
        on_microphone,
    }
}

#[cfg(not(target_os = "linux"))]
fn airlink_audio() -> alvr_airlink::audio::AirLinkAudio {
    // Without a sender the host's speaker channel reads as closed.
    let (_, speaker_receiver) = tokio::sync::mpsc::channel(1);
    alvr_airlink::audio::AirLinkAudio {
        speaker_receiver,
        on_microphone: Arc::new(|_| {}),
    }
}

/// Air Link headsets sit in the client list under this name: the prefix and
/// the headset's DISCO endpoint id in hexadecimal.
pub const AIRLINK_CLIENT_PREFIX: &str = "airlink-";

pub fn airlink_client_name(headset_id: [u8; 16]) -> String {
    let hex: String = headset_id
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect();
    format!("{AIRLINK_CLIENT_PREFIX}{hex}")
}

fn airlink_headset_id(client_name: &str) -> Option<[u8; 16]> {
    let hex = client_name.strip_prefix(AIRLINK_CLIENT_PREFIX)?;
    if hex.len() != 32 {
        return None;
    }
    let mut id = [0; 16];
    for (index, byte) in id.iter_mut().enumerate() {
        *byte = u8::from_str_radix(hex.get(index * 2..index * 2 + 2)?, 16).ok()?;
    }
    Some(id)
}
