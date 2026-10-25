mod video;

use alvr_client_core::{ClientCapabilities, ClientCoreContext, ClientCoreEvent};
use alvr_common::{
    DeviceMotion, Fov, HAND_RIGHT_ID, HEAD_ID, Pose, RelaxedAtomic, ViewParams,
    glam::{Quat, UVec2, Vec3},
    inputs::{BUTTON_INFO, QUEST_CONTROLLER_PROFILE_ID, RIGHT_SYSTEM_CLICK_ID},
    parking_lot::{Mutex, RwLock},
};
use alvr_packets::{ButtonEntry, ButtonValue, FaceData, TrackingData};
use alvr_session::CodecType;
use eframe::{
    Frame, NativeOptions,
    egui::{CentralPanel, RichText, Slider, Ui, ViewportBuilder},
};
use std::{
    f32::consts::{FRAC_PI_2, PI},
    sync::{
        Arc,
        mpsc::{self, TryRecvError},
    },
    thread,
    time::{Duration, Instant},
};

#[derive(Clone, PartialEq)]
struct WindowInput {
    height: f32,
    yaw: f32,
    pitch: f32,
    use_random_orientation: bool,
    emulated_decode_ms: u64,
    emulated_compositor_ms: u64,
    emulated_vsync_ms: u64,
}

impl Default for WindowInput {
    fn default() -> Self {
        Self {
            height: 1.5,
            yaw: 0.0,
            pitch: 0.0,
            // A scripted sweep (MOCK_SWEEP) should be repeatable.
            use_random_orientation: std::env::var_os("MOCK_SWEEP").is_none()
                && std::env::var_os("MOCK_SWAY").is_none(),
            emulated_decode_ms: 5,
            emulated_compositor_ms: 1,
            emulated_vsync_ms: 25,
        }
    }
}

#[derive(Clone)]
struct WindowOutput {
    hud_message: String,
    connected: bool,
    fps: f32,
    resolution: UVec2,
    decoder_codec: Option<CodecType>,
    current_frame_timestamp: Duration,
}

impl Default for WindowOutput {
    fn default() -> Self {
        Self {
            hud_message: "".into(),
            connected: false,
            fps: 1.0,
            resolution: UVec2::ZERO,
            decoder_codec: None,
            current_frame_timestamp: Duration::ZERO,
        }
    }
}

pub struct Window {
    input: WindowInput,
    input_sender: mpsc::Sender<WindowInput>,
    output: WindowOutput,
    output_receiver: mpsc::Receiver<WindowOutput>,
}

impl Window {
    fn new(
        input_sender: mpsc::Sender<WindowInput>,
        output_receiver: mpsc::Receiver<WindowOutput>,
    ) -> Self {
        Self {
            input: WindowInput::default(),
            input_sender,
            output: WindowOutput::default(),
            output_receiver,
        }
    }
}

impl eframe::App for Window {
    fn ui(&mut self, ui: &mut Ui, _: &mut Frame) {
        while let Ok(output) = self.output_receiver.try_recv() {
            self.output = output;
        }

        let mut input = self.input.clone();

        CentralPanel::default().show(ui, |ui| {
            ui.vertical_centered(|ui| {
                ui.heading(RichText::new(&self.output.hud_message));
            });
            ui.label(format!("Connected: {}", self.output.connected));
            ui.label(format!("FPS: {}", self.output.fps));
            ui.label(format!("View resolution: {}", self.output.resolution));
            ui.label(format!("Codec: {:?}", self.output.decoder_codec));
            ui.label(format!(
                "Current frame: {:?}",
                self.output.current_frame_timestamp
            ));
            ui.add_space(10.0);
            ui.horizontal(|ui| {
                ui.label("Height:");
                ui.add(Slider::new(&mut input.height, 0.0..=2.0));
            });
            ui.horizontal(|ui| {
                ui.label("Yaw:");
                ui.add(Slider::new(&mut input.yaw, -PI..=PI));
            });
            ui.horizontal(|ui| {
                ui.label("Pitch:");
                ui.add(Slider::new(&mut input.pitch, -FRAC_PI_2..=FRAC_PI_2));
            });
            ui.checkbox(
                &mut input.use_random_orientation,
                "Use randomized orientation offset",
            );
        });

        if input != self.input {
            self.input = input;

            self.input_sender.send(self.input.clone()).ok();
        }

        ui.request_repaint();
    }
}

fn mock_view_params() -> [ViewParams; 2] {
    if std::env::var("MOCK_DUMMY_VIEWS").as_deref() == Ok("1") {
        alvr_common::info!("Using dummy view params (MOCK_DUMMY_VIEWS=1)");
        return [ViewParams::DUMMY; 2];
    }

    let ipd = std::env::var("MOCK_IPD")
        .ok()
        .and_then(|s| s.trim().parse::<f32>().ok())
        .unwrap_or(0.063);

    let left_pose = Pose {
        orientation: Quat::IDENTITY,
        position: Vec3::new(-ipd / 2.0, 0.0, 0.0),
    };
    let right_pose = Pose {
        orientation: Quat::IDENTITY,
        position: Vec3::new(ipd / 2.0, 0.0, 0.0),
    };

    // Quest-2-like asymmetric FOV default:
    // Outer: ~52 deg, Inner: ~42 deg, Up: ~47 deg, Down: ~45 deg
    let mut left_fov = Fov {
        left: -(52.0_f32).to_radians(),
        right: (42.0_f32).to_radians(),
        up: (47.0_f32).to_radians(),
        down: -(45.0_f32).to_radians(),
    };
    let mut right_fov = Fov {
        left: -(42.0_f32).to_radians(),
        right: (52.0_f32).to_radians(),
        up: (47.0_f32).to_radians(),
        down: -(45.0_f32).to_radians(),
    };

    if let Ok(fov_str) = std::env::var("MOCK_FOV") {
        let parts = fov_str
            .split(',')
            .map(|s| s.trim().parse::<f32>())
            .collect::<Result<Vec<f32>, _>>();
        if let Ok(vals) = parts {
            let to_rad = |v: f32| if v.abs() > std::f32::consts::PI { v.to_radians() } else { v };
            if vals.len() == 1 {
                let v = to_rad(vals[0]).abs();
                left_fov = Fov {
                    left: -v,
                    right: v,
                    up: v,
                    down: -v,
                };
                right_fov = Fov {
                    left: -v,
                    right: v,
                    up: v,
                    down: -v,
                };
            } else if vals.len() == 4 {
                let outer = to_rad(vals[0]).abs();
                let inner = to_rad(vals[1]).abs();
                let up = to_rad(vals[2]).abs();
                let down = -to_rad(vals[3]).abs();
                left_fov = Fov {
                    left: -outer,
                    right: inner,
                    up,
                    down,
                };
                right_fov = Fov {
                    left: -inner,
                    right: outer,
                    up,
                    down,
                };
            } else if vals.len() == 8 {
                left_fov = Fov {
                    left: to_rad(vals[0]),
                    right: to_rad(vals[1]),
                    up: to_rad(vals[2]),
                    down: to_rad(vals[3]),
                };
                right_fov = Fov {
                    left: to_rad(vals[4]),
                    right: to_rad(vals[5]),
                    up: to_rad(vals[6]),
                    down: to_rad(vals[7]),
                };
            }
        }
    }

    alvr_common::info!(
        "Mock client view params: IPD = {} m, Left FOV = {:?}, Right FOV = {:?}",
        ipd,
        left_fov,
        right_fov
    );

    [
        ViewParams {
            pose: left_pose,
            fov: left_fov,
        },
        ViewParams {
            pose: right_pose,
            fov: right_fov,
        },
    ]
}

fn tracking_thread(
    context: Arc<ClientCoreContext>,
    streaming: Arc<RelaxedAtomic>,
    fps: f32,
    input: Arc<RwLock<WindowInput>>,
) {
    let timestamp_origin = Instant::now();
    context.send_view_params(mock_view_params());
    let sweep = video::Sweep::from_env();
    let sway = video::Sway::from_env();
    let hand_sweep = video::HandSweep::from_env();
    if hand_sweep.is_some() {
        let input_ids = BUTTON_INFO
            .iter()
            .filter_map(|(id, info)| (info.device_id == *HAND_RIGHT_ID).then_some(*id))
            .collect();
        context.send_active_interaction_profile(
            *HAND_RIGHT_ID,
            *QUEST_CONTROLLER_PROFILE_ID,
            input_ids,
        );
        alvr_common::info!("Mock right controller sweep enabled");
    }
    let mut pose_log = video::PoseLog::open();
    // Test-only handshake: the runner requests a normal controller System
    // click only after observing an open SteamVR dashboard. No automatic
    // startup toggle, since that would open an already closed dashboard.
    let system_click_file = std::env::var_os("MOCK_SYSTEM_CLICK_FILE").map(std::path::PathBuf::from);
    let mut system_release_at = None;
    let mut next_command_check = Instant::now();

    let mut loop_deadline = Instant::now();
    while streaming.value() {
        let now = Instant::now();
        if system_release_at.is_some_and(|deadline| now >= deadline) {
            context.send_buttons(vec![ButtonEntry {
                path_id: *RIGHT_SYSTEM_CLICK_ID,
                value: ButtonValue::Binary(false),
            }]);
            system_release_at = None;
        }
        if hand_sweep.is_some() && system_release_at.is_none() && now >= next_command_check {
            next_command_check = now + Duration::from_millis(100);
            if let Some(path) = &system_click_file
                && std::fs::remove_file(path).is_ok()
            {
                context.send_buttons(vec![ButtonEntry {
                    path_id: *RIGHT_SYSTEM_CLICK_ID,
                    value: ButtonValue::Binary(true),
                }]);
                system_release_at = Some(now + Duration::from_millis(100));
            }
        }
        let input_lock = input.read();

        let poll_timestamp = timestamp_origin.elapsed();
        let (sweep_yaw, yaw_velocity) = sweep.map_or((0.0, 0.0), |s| s.at(poll_timestamp));
        let yaw = input_lock.yaw + sweep_yaw;
        let (sway_x, x_velocity) = sway.map_or((0.0, 0.0), |s| s.at(poll_timestamp));
        let (hand_x, hand_x_velocity) = hand_sweep.map_or((0.0, 0.0), |s| s.at(poll_timestamp));
        pose_log.write(
            poll_timestamp,
            yaw,
            input_lock.pitch,
            yaw_velocity,
            sway_x,
            hand_x,
        );

        let mut orientation = Quat::from_rotation_y(yaw) * Quat::from_rotation_x(input_lock.pitch);

        if input_lock.use_random_orientation {
            orientation *= Quat::from_rotation_z(rand::random::<f32>() * 0.001);
        }

        let position = Vec3::new(sway_x, input_lock.height, 0.0);

        let mut device_motions = vec![(
            *HEAD_ID,
            DeviceMotion {
                pose: Pose {
                    orientation,
                    position,
                },
                linear_velocity: Vec3::new(x_velocity, 0.0, 0.0),
                angular_velocity: Vec3::new(0.0, yaw_velocity, 0.0),
            },
        )];
        if hand_sweep.is_some() {
            device_motions.push((
                *HAND_RIGHT_ID,
                DeviceMotion {
                    pose: Pose {
                        orientation: Quat::IDENTITY,
                        position: Vec3::new(hand_x, input_lock.height - 0.25, -0.45),
                    },
                    linear_velocity: Vec3::new(hand_x_velocity, 0.0, 0.0),
                    angular_velocity: Vec3::ZERO,
                },
            ));
        }

        context.send_tracking(TrackingData {
            poll_timestamp,
            device_motions,
            hand_skeletons: [None, None],
            face: FaceData::default(),
            body: None,
        });

        drop(input_lock);

        loop_deadline += Duration::from_secs_f32(1.0 / fps / 3.0);
        thread::sleep(loop_deadline.saturating_duration_since(Instant::now()))
    }
    if system_release_at.is_some() {
        context.send_buttons(vec![ButtonEntry {
            path_id: *RIGHT_SYSTEM_CLICK_ID,
            value: ButtonValue::Binary(false),
        }]);
    }
}

fn client_thread(
    output_sender: mpsc::Sender<WindowOutput>,
    input_receiver: mpsc::Receiver<WindowInput>,
) {
    let capabilities = ClientCapabilities {
        platform: alvr_system_info::platform(None, None),
        default_view_resolution: UVec2::new(1920, 1832),
        max_view_resolution: UVec2::new(1920, 1832),
        refresh_rates: vec![60.0, 72.0, 80.0, 90.0, 120.0],
        foveated_encoding: false,
        encoder_high_profile: false,
        encoder_10_bits: false,
        encoder_av1: false,
        prefer_10bit: false,
        preferred_encoding_gamma: 1.0,
        prefer_hdr: false,
    };
    let client_core_context = Arc::new(ClientCoreContext::new(capabilities, vec![]));

    client_core_context.resume();

    let streaming = Arc::new(RelaxedAtomic::new(false));
    let got_decoder_config = Arc::new(RelaxedAtomic::new(false));

    // Received video goes to video::VideoSink (viewer window and recording).
    let video_origin = Instant::now();
    let video_sink = Arc::new(Mutex::new(None::<video::VideoSink>));
    let latest_frame = Arc::new(Mutex::new(Duration::ZERO));
    {
        let sink = Arc::clone(&video_sink);
        let latest = Arc::clone(&latest_frame);
        let context = Arc::downgrade(&client_core_context);
        client_core_context.set_decoder_input_callback(Box::new(move |timestamp, nal| {
            let submitted = sink
                .lock()
                .as_ref()
                .is_some_and(|sink| sink.push(timestamp, nal));
            if submitted {
                *latest.lock() = timestamp;
                if let Some(context) = context.upgrade() {
                    context.report_frame_decoded(timestamp);
                }
            }
            submitted
        }));
    }
    let mut maybe_tracking_thread = None;

    let mut window_output = WindowOutput::default();
    let window_input = Arc::new(RwLock::new(WindowInput::default()));

    let mut deadline = Instant::now();
    'main_loop: loop {
        let input_lock = window_input.read();

        while let Some(event) = client_core_context.poll_event() {
            match event {
                ClientCoreEvent::UpdateHudMessage(message) => {
                    window_output.hud_message = message;
                }
                ClientCoreEvent::StreamingStarted(config) => {
                    window_output.fps = config.negotiated_config.refresh_rate_hint;
                    window_output.connected = true;
                    window_output.resolution = config.negotiated_config.view_resolution;

                    streaming.set(true);

                    let context = Arc::clone(&client_core_context);
                    let streaming = Arc::clone(&streaming);
                    let input = Arc::clone(&window_input);
                    maybe_tracking_thread = Some(thread::spawn(move || {
                        tracking_thread(
                            context,
                            streaming,
                            config.negotiated_config.refresh_rate_hint,
                            input,
                        )
                    }));
                }
                ClientCoreEvent::StreamingStopped => {
                    streaming.set(false);
                    got_decoder_config.set(false);

                    if let Some(thread) = maybe_tracking_thread.take() {
                        thread.join().ok();
                    }

                    window_output.fps = 1.0;
                    window_output.connected = false;
                    window_output.resolution = UVec2::ZERO;
                    window_output.decoder_codec = None;
                    video_sink.lock().take();
                }
                ClientCoreEvent::DecoderConfig { codec, config_nal } => {
                    // Later configs are ignored until reconnection.
                    if !got_decoder_config.value() {
                        *video_sink.lock() = Some(video::VideoSink::start(
                            codec,
                            &config_nal,
                            window_output.fps,
                            video_origin,
                        ));
                    }
                    got_decoder_config.set(true);

                    window_output.decoder_codec = Some(codec);
                }
                ClientCoreEvent::Haptics { .. } | ClientCoreEvent::RealTimeConfig(_) => (),
            }

            output_sender.send(window_output.clone()).ok();
        }

        thread::sleep(Duration::from_millis(3));

        window_output.current_frame_timestamp = *latest_frame.lock();
        client_core_context.report_compositor_start(window_output.current_frame_timestamp);

        thread::sleep(Duration::from_millis(input_lock.emulated_compositor_ms));

        client_core_context.report_submit(
            window_output.current_frame_timestamp,
            Duration::from_millis(input_lock.emulated_vsync_ms),
        );

        drop(input_lock);

        match input_receiver.try_recv() {
            Ok(input) => *window_input.write() = input,
            Err(TryRecvError::Disconnected) => break 'main_loop,
            Err(TryRecvError::Empty) => (),
        }

        deadline += Duration::from_secs_f32(1.0 / window_output.fps);
        thread::sleep(deadline.saturating_duration_since(Instant::now()));
    }

    streaming.set(false);
    if let Some(thread) = maybe_tracking_thread {
        thread.join().unwrap();
    }
    video_sink.lock().take();

    client_core_context.pause()

    // client_core_context destroy is called here on drop
}

fn main() {
    env_logger::init();

    let (input_sender, input_receiver) = mpsc::channel::<WindowInput>();
    let (output_sender, output_receiver) = mpsc::channel::<WindowOutput>();

    let client_thread = thread::spawn(|| {
        client_thread(output_sender, input_receiver);
    });

    eframe::run_native(
        "Mock client",
        NativeOptions {
            viewport: ViewportBuilder::default().with_inner_size((400.0, 400.0)),
            ..Default::default()
        },
        Box::new(|_| Ok(Box::new(Window::new(input_sender, output_receiver)))),
    )
    .ok();

    client_thread.join().unwrap();
}
