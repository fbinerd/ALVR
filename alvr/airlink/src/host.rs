//! Runtime owner for the Meta Air Link DISCO listener.

use crate::{
    disco::{
        self, CertExchange, PairingRequest, TopicCreateRequest, CONTROL_TOPIC,
        MSG_AWAITING_USER_REQ, MSG_CLOCK_SYNC_FINISH, MSG_CLOCK_SYNC_INIT, MSG_COMMITMENT,
        MSG_CONNECT_REQ, MSG_DISCONNECT_REQ, MSG_EXCHANGING_CERT_REQ, MSG_PAIRING_REQ,
        MSG_TOPIC_CREATE_REQ, PAIRING_TOPIC,
    },
    discovery::AirLinkAdvertiser,
    identity::HostIdentity,
    ripc,
    transport::{self, AirLinkListener},
};
use crate::guardian::AirLinkGuardian;
use crate::video::AirLinkVideoFrame;
use crate::tracking::{AirLinkControllerInput, AirLinkPose, AirLinkTrackingSample};
use ring::rand::{SecureRandom, SystemRandom};
use std::{
    io::{Error, ErrorKind},
    net::{IpAddr, SocketAddr},
    path::Path,
    sync::Arc,
};
use tokio::net::TcpStream;
use tokio::sync::{broadcast, oneshot};
use tokio_rustls::server::TlsStream;

/// MeshChannel is the first stream channel the headset creates.
const MESH_TOPIC: u32 = 2;

static ENABLED: std::sync::LazyLock<tokio::sync::watch::Sender<bool>> =
    std::sync::LazyLock::new(|| tokio::sync::watch::channel(true).0);
static STATUS: std::sync::Mutex<Option<AirLinkHeadsetStatus>> = std::sync::Mutex::new(None);

static PAIRING_REQUEST: std::sync::Mutex<Option<AirLinkPairingRequest>> =
    std::sync::Mutex::new(None);
// DISCO endpoint ids of the headsets the user accepted. The certificate
// binding is checked separately against persistent state before auto trust.
static KNOWN_HEADSETS: std::sync::Mutex<Vec<[u8; 16]>> = std::sync::Mutex::new(Vec::new());

/// A headset that waits for the user to let it in: one that pairs for the
/// first time, or one that remembers this host while the host does not know
/// it (any more).
#[derive(Clone, PartialEq)]
pub struct AirLinkPairingRequest {
    pub address: SocketAddr,
    pub headset_id: [u8; 16],
    pub headset_name: String,
    pub pairing_code: Option<u32>,
}

/// The headsets that connect without a question. Any other is held until
/// the user accepts it, and one taken off the list is dropped.
pub fn set_known_headsets(headset_ids: Vec<[u8; 16]>) {
    *KNOWN_HEADSETS.lock().unwrap() = headset_ids;
}

// Counter of answers and the last one: accepted or not.
static PAIRING_ANSWER: std::sync::LazyLock<tokio::sync::watch::Sender<(u64, bool)>> =
    std::sync::LazyLock::new(|| tokio::sync::watch::channel((0, false)).0);

/// The headset that waits for the user to accept its pairing on the PC.
pub fn pairing_request() -> Option<AirLinkPairingRequest> {
    PAIRING_REQUEST.lock().unwrap().clone()
}

/// The user's answer to `pairing_request`.
pub fn answer_pairing(accept: bool) {
    PAIRING_ANSWER.send_modify(|answer| *answer = (answer.0 + 1, accept));
}

/// The headset connected to the host.
#[derive(Clone, Copy, PartialEq)]
pub struct AirLinkHeadsetStatus {
    pub address: SocketAddr,
    pub streaming: bool,
}

/// Switches the host on or off. Off, it is not advertised, drops the headset
/// it serves and turns new connections away.
pub fn set_enabled(enabled: bool) {
    ENABLED.send_if_modified(|current| std::mem::replace(current, enabled) != enabled);
}

pub fn headset_status() -> Option<AirLinkHeadsetStatus> {
    *STATUS.lock().unwrap()
}
const AUDIO_TOPIC: u32 = 3;
const AUDIO_CONTROL_TOPIC: u32 = 4;

// What Meta Horizon Link 208 sends on the AudioControlChannel when the stream
// channels exist and again later in the session (capture of 2026-10-12,
// 13:05, at 16.5 s and 122.6 s). Not decoded.
const AUDIO_CONTROL_START: [u8; 39] = [
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x24, 0x02, 0, 0, 0, 0x01, 0, 0,
    0, 0x06, 0, 0, 0, 0xb9, 0x92, 0x43, 0x6c, 0x02, 0x01,
];
/// Side-by-side picture size announced in the GPU stream channel config.
const STANDING_HEAD_HEIGHT: f32 = 1.6;
const IDLE_POSE: AirLinkPose = AirLinkPose {
    orientation_xyzw: [0.0, 0.0, 0.0, 1.0],
    position_xyz: [0.0, STANDING_HEAD_HEIGHT, 0.0],
};
/// Head pose reported until the headset's own tracking arrives.
const IDLE_TRACKING_SAMPLE: AirLinkTrackingSample = AirLinkTrackingSample {
    head: IDLE_POSE,
    head_time_s: 0.0,
    left_hand: None,
    right_hand: None,
};

pub struct AirLinkHost {
    listener: AirLinkListener,
    advertiser: Option<AirLinkAdvertiser>,
    hostname: String,
    advertise_address: IpAddr,
    identity: HostIdentity,
    endpoint_name: String,
    video_receiver: broadcast::Receiver<AirLinkVideoFrame>,
    audio: crate::audio::AirLinkAudio,
    on_streaming_started: Arc<dyn Fn() + Send + Sync>,
    on_streaming_stopped: Arc<dyn Fn() + Send + Sync>,
    on_keyframe_needed: Arc<dyn Fn() + Send + Sync>,
    on_guardian_update: Arc<dyn Fn(AirLinkGuardian) + Send + Sync>,
    on_tracking_update: Arc<dyn Fn(AirLinkTrackingSample) + Send + Sync>,
    on_controller_input: Arc<dyn Fn(AirLinkControllerInput) + Send + Sync>,
    streaming_active: bool,
}

#[derive(Default)]
struct RuntimeServiceState {
    streaming_invite_accepted: bool,
    reverse_connect_started: bool,
    reverse_client_id: Option<u32>,
    bootstrap_index: usize,
    streaming_route: Option<ripc::RuntimeIpcHeader>,
    oaf_client_route: Option<ripc::RuntimeIpcHeader>,
    gpu_stream_ready: bool,
    screen_is_on_sent: bool,
    buffer_pool_acked: bool,
    pending_gpu_connect_response: Option<Vec<u8>>,
    mesh_sent: bool,
    openxr_runtime_sent: bool,
    guardian: AirLinkGuardian,
}

impl AirLinkHost {
    fn headset_is_known(&self, headset_id: [u8; 16], certificate: Option<&[u8]>) -> bool {
        KNOWN_HEADSETS.lock().unwrap().contains(&headset_id)
            && certificate.is_some_and(|certificate| {
                self.identity
                    .matches_trusted_headset_certificate(headset_id, certificate)
            })
    }

    pub async fn bind(
        state_dir: impl AsRef<Path>,
        address: SocketAddr,
        advertise_address: IpAddr,
        hostname: &str,
        endpoint_name: &str,
        video_receiver: broadcast::Receiver<AirLinkVideoFrame>,
        audio: crate::audio::AirLinkAudio,
        on_streaming_started: Arc<dyn Fn() + Send + Sync>,
        on_streaming_stopped: Arc<dyn Fn() + Send + Sync>,
        on_tracking_update: Arc<dyn Fn(AirLinkTrackingSample) + Send + Sync>,
        on_controller_input: Arc<dyn Fn(AirLinkControllerInput) + Send + Sync>,
        on_keyframe_needed: Arc<dyn Fn() + Send + Sync>,
        on_guardian_update: Arc<dyn Fn(AirLinkGuardian) + Send + Sync>,
    ) -> Result<Self, Box<dyn std::error::Error + Send + Sync>> {
        let identity = HostIdentity::load_or_create(state_dir)?;
        let (certificate, private_key) = identity.rustls_identity();
        let listener = AirLinkListener::bind(address, certificate, private_key).await?;
        let port = listener.local_addr()?.port();
        let advertiser = AirLinkAdvertiser::register(
            hostname,
            endpoint_name,
            identity.endpoint_uuid(),
            advertise_address,
            port,
        )?;

        log::info!("Meta Air Link host listening on {address} (advertised port {port})");
        Ok(Self {
            listener,
            advertiser: Some(advertiser),
            hostname: hostname.to_owned(),
            advertise_address,
            identity,
            endpoint_name: endpoint_name.to_owned(),
            video_receiver,
            audio,
            on_streaming_started,
            on_streaming_stopped,
            on_tracking_update,
            on_controller_input,
            on_keyframe_needed,
            on_guardian_update,
            streaming_active: false,
        })
    }

    pub fn local_addr(&self) -> std::io::Result<SocketAddr> {
        self.listener.local_addr()
    }

    pub async fn run(
        mut self,
        mut shutdown: oneshot::Receiver<()>,
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let mut enabled = ENABLED.subscribe();
        loop {
            if !*enabled.borrow_and_update() {
                if self.advertiser.take().is_some() {
                    log::info!("Meta Air Link host switched off");
                }
                tokio::select! {
                    _ = &mut shutdown => return Ok(()),
                    changed = enabled.changed() => {
                        if changed.is_err() {
                            return Ok(());
                        }
                    }
                    // A headset that remembers the host still finds its port.
                    _ = self.listener.accept() => {}
                }
                continue;
            }
            if self.advertiser.is_none() {
                self.advertiser = Some(AirLinkAdvertiser::register(
                    &self.hostname,
                    &self.endpoint_name,
                    self.identity.endpoint_uuid(),
                    self.advertise_address,
                    self.listener.local_addr()?.port(),
                )?);
                log::info!("Meta Air Link host switched on");
            }
            let accepted = tokio::select! {
                _ = &mut shutdown => return Ok(()),
                _ = enabled.changed() => continue,
                result = self.listener.accept() => result,
            };
            let (mut stream, peer) = match accepted {
                Ok(connection) => connection,
                Err(error) => {
                    log::warn!("Meta Air Link TLS accept failed: {error}");
                    continue;
                }
            };
            let headset_certificate = transport::peer_certificate(&stream);
            log::info!("Meta Air Link TLS session established from {peer}");
            *STATUS.lock().unwrap() = Some(AirLinkHeadsetStatus {
                address: peer,
                streaming: false,
            });
            let result = tokio::select! {
                _ = &mut shutdown => return Ok(()),
                _ = enabled.changed() => Err("the host was switched off".into()),
                result = self.serve_connection(&mut stream, headset_certificate.as_deref()) => result,
            };
            *STATUS.lock().unwrap() = None;
            *PAIRING_REQUEST.lock().unwrap() = None;
            if let Err(error) = result {
                log::warn!("Meta Air Link connection from {peer} ended: {error}");
            }
            if self.streaming_active {
                (self.on_streaming_stopped)();
                self.streaming_active = false;
            }
        }
    }

    async fn serve_connection(
        &mut self,
        stream: &mut TlsStream<TcpStream>,
        headset_certificate: Option<&[u8]>,
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let mut gpu_buffer_handshake_ack_sent = false;
        let mut gpu_initial_frame_sent = false;
        let mut quest_endpoint_uuid = None;
        let mut runtime_service = RuntimeServiceState::default();
        let mut next_video_frame_id = 2u32;
        let mut frame_reader = transport::FrameReader::default();
        let mut last_keyframe_request = std::time::Instant::now();
        let mut skipped_before_keyframe = 0u32;
        let mut headset_offset_ns = 0i64;
        // The headset sends its first tracking packet only after video has
        // started, and ALVR drops every frame that has no head pose.
        let mut tracking_received = false;
        // Height of the floor in the headset's tracking space. SteamVR gets
        // floor-relative poses; frame metadata goes back to tracking space.
        // The headset reports it with the controller input. The Guardian
        // polygon's height is no use: it is in another space (the user,
        // seated, came out 0.5 m above the floor).
        let mut reported_floor_y: Option<f32> = None;
        let mut head_predictor = crate::tracking::HeadPredictor::new(head_prediction_ms());
        let mut floor_y: Option<f32> = None;
        let mut floor_logged_at = None;
        let mut idle_pose_timer = tokio::time::interval(std::time::Duration::from_micros(13_889));
        idle_pose_timer.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        // The keyframe asked for when the stream becomes ready can be lost:
        // ALVR drops a frame whose head pose it does not have yet, and then
        // forwards nothing until it has seen a keyframe. Keep asking.
        let mut keyframe_retry_timer =
            tokio::time::interval(std::time::Duration::from_millis(500));
        keyframe_retry_timer.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        // Game audio recorded while nobody listened is old by now.
        while self.audio.speaker_receiver.try_recv().is_ok() {}
        let mut speaker_open = true;
        let mut speaker_packets = 0u64;
        let mut display_grid_s: Option<f64> = None;
        let mut display_lead_s = display_lead_ms() / 1000.0;
        let frame_report_trace = std::env::var_os("HOME").is_some_and(|home| {
            std::path::Path::new(&home)
                .join(".config/alvr/airlink_frame_reports")
                .exists()
        });
        let mut frame_reports: Vec<Vec<u64>> = Vec::new();
        let mut frame_reports_since = std::time::Instant::now();
        // Pairing waits for the user's answer in the dashboard.
        let mut pairing_answer = PAIRING_ANSWER.subscribe();
        let mut pairing_pending = false;
        let mut pairing_code = None;
        // The headset asked for the answer itself (first pairing) and waits
        // for an acknowledgement; otherwise only its messages are held back.
        let mut pairing_awaits_ack = false;
        let mut headset: Option<([u8; 16], String)> = None;
        let mut approved = false;
        let mut approved_at: Option<std::time::Instant> = None;
        let mut held: std::collections::VecDeque<(u32, Vec<u8>)> =
            std::collections::VecDeque::new();
        let peer_address = STATUS.lock().unwrap().map(|status| status.address);
        let mut channel_handshake_acked = [false; 15];
        loop {
            // What an accepted headset sent while it waited comes first.
            let held_message = if approved { held.pop_front() } else { None };
            let (topic, payload) = if let Some(message) = held_message {
                message
            } else { tokio::select! {
                changed = pairing_answer.changed(), if pairing_pending => {
                    if changed.is_err() {
                        continue;
                    }
                    let accepted = pairing_answer.borrow_and_update().1;
                    pairing_pending = false;
                    *PAIRING_REQUEST.lock().unwrap() = None;
                    if accepted {
                        let headset_id = headset
                            .as_ref()
                            .map(|(id, _)| *id)
                            .ok_or("cannot trust a headset without a DISCO endpoint id")?;
                        let certificate = headset_certificate
                            .ok_or("cannot trust a headset without a TLS client certificate")?;
                        if pairing_code.is_none() {
                            return Err("cannot trust a headset before its pairing code is available".into());
                        }
                        // Bind the self-declared endpoint id to the certificate
                        // proven by TLS before acknowledging the pairing.
                        self.identity
                            .trust_headset_certificate(headset_id, certificate)?;
                        log::info!("Air Link headset accepted by the user");
                        approved = true;
                        approved_at = Some(std::time::Instant::now());
                        if std::mem::take(&mut pairing_awaits_ack) {
                            self.send(stream, PAIRING_TOPIC, &disco::pack_awaiting_user_ack())
                                .await?;
                        }
                        continue;
                    }
                    if pairing_awaits_ack {
                        self.send(stream, PAIRING_TOPIC, &disco::pack_awaiting_user_nack())
                            .await?;
                    }
                    return Err("the user refused the headset".into());
                }
                chunk = self.audio.speaker_receiver.recv(), if speaker_open => {
                    match chunk {
                        // The headset plays what it gets from the first video
                        // frame on; before that the channel may not exist.
                        Some(samples) if next_video_frame_id > 2 => {
                            if speaker_packets == 0 {
                                self.send(stream, AUDIO_CONTROL_TOPIC, &AUDIO_CONTROL_START)
                                    .await?;
                            }
                            let packet = crate::audio::pack_speaker_packet(
                                &samples,
                                monotonic_nanos(),
                            );
                            self.send(stream, AUDIO_TOPIC, &packet).await?;
                            speaker_packets += 1;
                            if speaker_packets == 1 || speaker_packets % 3000 == 0 {
                                log::info!("Air Link game audio: {speaker_packets} packets sent");
                            }
                        }
                        Some(_) => {}
                        None => speaker_open = false,
                    }
                    continue;
                }
                _ = keyframe_retry_timer.tick(),
                    if runtime_service.gpu_stream_ready && next_video_frame_id == 2 =>
                {
                    if last_keyframe_request.elapsed().as_millis() >= 500 {
                        log::info!("Air Link video has not started: asking for a keyframe");
                        (self.on_keyframe_needed)();
                        last_keyframe_request = std::time::Instant::now();
                    }
                    continue;
                }
                _ = idle_pose_timer.tick(),
                    if runtime_service.gpu_stream_ready && !tracking_received =>
                {
                    (self.on_tracking_update)(IDLE_TRACKING_SAMPLE);
                    continue;
                }
                received = self.video_receiver.recv() => {
                    match received {
                        Ok(mut frame) if runtime_service.gpu_stream_ready => {
                            frame.head_pose[5] += reported_floor_y.or(floor_y).unwrap_or(0.0);
                            if !runtime_service.screen_is_on_sent {
                                if let Some(route) = runtime_service.oaf_client_route {
                                    let event = protocol_result(ripc::pack_oaf_broadcast_event(
                                        route,
                                        "SCREEN_IS_ON_UPDATE",
                                        "\t\t\"on\" : true",
                                        unix_timestamp_ms(),
                                    ))?;
                                    self.send(stream, ripc::CLIENT_TRAFFIC_TOPIC, &event).await?;
                                    runtime_service.screen_is_on_sent = true;
                                    log::info!("sent Air Link SCREEN_IS_ON_UPDATE");
                                }
                            }
                            // The headset's decoder has to start on a keyframe.
                            if next_video_frame_id == 2 && !frame.is_idr {
                                skipped_before_keyframe += 1;
                                if last_keyframe_request.elapsed().as_millis() > 500 {
                                    log::info!(
                                        "Air Link video waiting for a keyframe: \
                                         {skipped_before_keyframe} frames skipped"
                                    );
                                    (self.on_keyframe_needed)();
                                    last_keyframe_request = std::time::Instant::now();
                                }
                                continue;
                            }
                            if next_video_frame_id == 2 {
                                log::info!(
                                    "Air Link video starts: keyframe of {} bytes after \
                                     {skipped_before_keyframe} skipped frames",
                                    frame.nal.len()
                                );
                            }
                            if !runtime_service.mesh_sent {
                                // Without the rectify mesh the headset decodes every
                                // frame and keeps showing its loading environment.
                                let mesh = crate::mesh::pack(crate::video::stream_size());
                                self.send(stream, MESH_TOPIC, &mesh).await?;
                                runtime_service.mesh_sent = true;
                                log::info!("sent Air Link rectify mesh: {} bytes", mesh.len());
                            }
                            // A headset the user took off the list goes, once
                            // the list has had time to take in a new one.
                            if next_video_frame_id % 72 == 0
                                && approved_at.is_none_or(|at| at.elapsed().as_secs() >= 5)
                                && headset.as_ref().is_some_and(|(id, _)| !self.headset_is_known(*id, headset_certificate))
                            {
                                return Err("the headset was removed from the list".into());
                            }
                            // The trial values can be changed while streaming.
                            if next_video_frame_id % 36 == 0 {
                                head_predictor.set_lead_ms(head_prediction_ms());
                                display_lead_s = display_lead_ms() / 1000.0;
                            }
                            let host_now_ns = monotonic_nanos();
                            // One refresh period after the previous frame's
                            // slot; whole periods are skipped or repeated
                            // when the PC missed frames or drifted away.
                            let headset_now_s =
                                (host_now_ns as i128 + i128::from(headset_offset_ns)) as f64 / 1e9;
                            let period_s = crate::video::frame_period_s();
                            let display_base_s = match display_grid_s {
                                Some(previous) => {
                                    let next = previous + period_s;
                                    // Positive: the frame left later than
                                    // its slot.
                                    let error = headset_now_s - next;
                                    if error.abs() > 4.0 * period_s {
                                        // A stall or a clock step: start over.
                                        headset_now_s
                                    } else if error > 0.75 * period_s {
                                        // The PC missed frames: their slots
                                        // pass unused.
                                        next + (error / period_s).round() * period_s
                                    } else {
                                        // Follow the PC's clock slowly, so
                                        // send jitter does not reach the
                                        // display times.
                                        next + 0.01 * error
                                    }
                                }
                                None => headset_now_s,
                            };
                            display_grid_s = Some(display_base_s);
                            let packet = frame.pack(
                                next_video_frame_id,
                                host_now_ns,
                                headset_offset_ns,
                                display_base_s + display_lead_s,
                            );
                            self.send(stream, ripc::GPU_BUFFER_STREAM_TOPIC, &packet).await?;
                            if next_video_frame_id >= 110 && !runtime_service.openxr_runtime_sent {
                                if let Some(route) = runtime_service.oaf_client_route {
                                    let event = protocol_result(ripc::pack_oaf_broadcast_event(
                                        route,
                                        "OPENXR_RUNTIME_UPDATE",
                                        "\t\t\"isOculus\" : true,\n\t\t\"runtimeName\" : \"Oculus OpenXR\"",
                                        unix_timestamp_ms(),
                                    ))?;
                                    self.send(stream, ripc::CLIENT_TRAFFIC_TOPIC, &event).await?;
                                    runtime_service.openxr_runtime_sent = true;
                                    log::info!("sent Air Link OPENXR_RUNTIME_UPDATE");
                                }
                            }
                            // Windows numbers frames 2, 4, 5, ...; frame 3 never appears.
                            next_video_frame_id = if next_video_frame_id == 2 {
                                4
                            } else {
                                next_video_frame_id.saturating_add(1)
                            };
                        }
                        Ok(_) | Err(broadcast::error::RecvError::Lagged(_)) => {}
                        Err(broadcast::error::RecvError::Closed) => {
                            return Err(Error::new(
                                ErrorKind::BrokenPipe,
                                "ALVR video source closed",
                            )
                            .into());
                        }
                    }
                    continue;
                }
                incoming = frame_reader.read_frame(stream) => incoming?,
            } };
            // Only connecting and pairing go on for a headset the user has
            // not accepted: the rest waits for the answer in the dashboard.
            if !approved && topic != CONTROL_TOPIC && topic != PAIRING_TOPIC {
                if headset.as_ref().is_some_and(|(id, _)| self.headset_is_known(*id, headset_certificate)) {
                    approved = true;
                } else {
                    if !pairing_pending
                        && let (Some((id, name)), Some(address)) = (&headset, peer_address)
                    {
                        pairing_pending = true;
                        pairing_answer.borrow_and_update();
                        *PAIRING_REQUEST.lock().unwrap() = Some(AirLinkPairingRequest {
                            address,
                            headset_id: *id,
                            headset_name: name.clone(),
                            pairing_code,
                        });
                        log::info!("Air Link headset {name} at {address} is not known: waiting for the user");
                    }
                    // A headset left waiting gives up and connects again.
                    if held.len() < 256 {
                        held.push_back((topic, payload));
                    }
                    continue;
                }
            }
            if topic == ripc::SERVER_TRAFFIC_TOPIC || topic == ripc::CLIENT_TRAFFIC_TOPIC {
                trace_frame("rx", topic, &payload);
            }
            let header = match disco::message_header(&payload) {
                Ok(header) => header,
                Err(_) => continue,
            };
            let message = header.1;

            if topic == CONTROL_TOPIC {
                match message {
                    MSG_CONNECT_REQ => {
                        let request = protocol_result(disco::ConnectRequest::unpack(&payload))?;
                        quest_endpoint_uuid = Some(request.endpoint_uuid);
                        approved = self.headset_is_known(request.endpoint_uuid, headset_certificate);
                        headset = Some((request.endpoint_uuid, request.endpoint_name.clone()));
                        let version = disco::negotiate_protocol(
                            &request,
                            disco::PROTOCOL_VERSION_RANGE,
                        )
                        .ok_or_else(|| {
                            Error::new(ErrorKind::InvalidData, "no common DISCO protocol version")
                        })?;
                        let response = protocol_result(disco::pack_connect_ack(
                            version,
                            parse_uuid_bytes(self.identity.endpoint_uuid())?,
                            &self.endpoint_name,
                            disco::ENDPOINT_TYPE,
                            disco::ENDPOINT_VERSION,
                        ))?;
                        self.send(stream, topic, &response).await?;
                    }
                    MSG_CLOCK_SYNC_INIT => {
                        let client_time = protocol_result(disco::unpack_clock_sync_init(&payload))?;
                        let receive_time = monotonic_nanos();
                        headset_offset_ns = client_time as i64 - receive_time as i64;
                        let send_time = monotonic_nanos();
                        let response = disco::pack_clock_sync_reply(
                            client_time,
                            receive_time,
                            send_time,
                        );
                        self.send(stream, topic, &response).await?;
                    }
                    MSG_CLOCK_SYNC_FINISH => {}
                    MSG_DISCONNECT_REQ => {
                        self.send(stream, topic, &disco::pack_disconnect_ack()).await?;
                        return Ok(());
                    }
                    MSG_TOPIC_CREATE_REQ => {
                        let request = protocol_result(TopicCreateRequest::unpack(&payload))?;
                        let response = protocol_result(request.pack_ack(0))?;
                        self.send(stream, topic, &response).await?;
                    }
                    _ => log::debug!("Unhandled Air Link control message 0x{message:08x}"),
                }
            } else if topic == PAIRING_TOPIC {
                match message {
                    MSG_PAIRING_REQ => {
                        let _request = protocol_result(PairingRequest::unpack(&payload))?;
                        self.send(stream, topic, &disco::pack_pairing_ack()).await?;
                    }
                    MSG_EXCHANGING_CERT_REQ => {
                        let peer = protocol_result(CertExchange::unpack(&payload))?;
                        let signature_size = peer.signature.len().max(256);
                        let response = unsigned_cert_exchange(signature_size)?;
                        if let Some(certificate) = headset_certificate {
                            let code = disco::pairing_code(
                                certificate,
                                &peer.random,
                                self.identity.certificate_der(),
                                &response.random,
                            );
                            pairing_code = Some((code % 1_000_000) as u32);
                            log::info!("Air Link pairing code {:06} (0x{code:016x})", code % 1_000_000);
                            // A request may already be visible from the earlier
                            // connection message. Publish the computed code too.
                            if let Some(request) = PAIRING_REQUEST.lock().unwrap().as_mut() {
                                request.pairing_code = pairing_code;
                            }
                        } else {
                            log::warn!("Air Link headset sent no TLS client certificate; cannot show a pairing code");
                        }
                        let response = protocol_result(
                            response.pack(disco::MSG_EXCHANGING_CERT_ACK),
                        )?;
                        self.send(stream, topic, &response).await?;
                    }
                    MSG_AWAITING_USER_REQ => {
                        // Answered when the user accepts or refuses in the
                        // dashboard, not before.
                        // A headset the user already accepted pairs again
                        // without a question (it forgot this host).
                        if approved {
                            self.send(stream, topic, &disco::pack_awaiting_user_ack()).await?;
                            continue;
                        }
                        pairing_awaits_ack = true;
                        if !pairing_pending
                            && let (Some((id, name)), Some(address)) = (&headset, peer_address)
                        {
                            pairing_pending = true;
                            pairing_answer.borrow_and_update();
                            *PAIRING_REQUEST.lock().unwrap() = Some(AirLinkPairingRequest {
                                address,
                                headset_id: *id,
                                headset_name: name.clone(),
                                pairing_code,
                            });
                            log::info!("Air Link pairing asked by {name} at {address}: waiting for the user");
                        }
                    }
                    MSG_COMMITMENT => {
                        log::debug!("Air Link pairing commitment received");
                    }
                    _ => log::debug!("Unhandled Air Link pairing message 0x{message:08x}"),
                }
            } else if topic == ripc::GPU_BUFFER_STREAM_TOPIC {
                // The headset's report for each frame. Its fields are not
                // decoded yet: with tracing on, their medians are logged
                // once a second to relate them to what the headset shows.
                if frame_report_trace
                    && let Some((_, fields)) = crate::video::unpack_frame_report(&payload)
                {
                    frame_reports.push(fields);
                    if frame_reports_since.elapsed().as_secs() >= 1 {
                        let width = frame_reports.iter().map(Vec::len).max().unwrap_or(0);
                        let medians: Vec<u64> = (0..width)
                            .map(|index| {
                                let mut column: Vec<u64> = frame_reports
                                    .iter()
                                    .filter_map(|fields| fields.get(index).copied())
                                    .collect();
                                column.sort_unstable();
                                column[column.len() / 2]
                            })
                            .collect();
                        log::info!(
                            "Air Link frame reports: {} medians {medians:?}",
                            frame_reports.len()
                        );
                        frame_reports.clear();
                        frame_reports_since = std::time::Instant::now();
                    }
                }
            } else if (2..=14).contains(&topic) {
                if let Some(response) = ripc::channel_buffer_handshake_ack(&payload) {
                    // The headset sends the handshake twice and Horizon
                    // answers once. After a second answer the headset takes
                    // nothing more from the PC on that channel: no rectify
                    // mesh, no game audio.
                    if !std::mem::replace(&mut channel_handshake_acked[topic as usize], true) {
                        self.send(stream, topic, response).await?;
                    }
                } else if topic == AUDIO_TOPIC {
                    if let Some(samples) = crate::audio::unpack_microphone_packet(&payload) {
                        (self.audio.on_microphone)(samples);
                    }
                } else if topic == 11 {
                    if let Some(mut sample) = crate::tracking::unpack_tracking(&payload) {
                        sample.head = head_predictor.predict(sample.head, sample.head_time_s);
                        if !tracking_received {
                            tracking_received = true;
                            log::info!("Air Link tracking starts");
                        }
                        // Until the headset has reported the floor, assume the
                        // user stands when the stream starts.
                        let floor = reported_floor_y.unwrap_or_else(|| {
                            *floor_y.get_or_insert_with(|| {
                                let floor = sample.head.position_xyz[1] - STANDING_HEAD_HEIGHT;
                                log::info!(
                                    "Air Link floor assumed at y={floor:.3} m in tracking space"
                                );
                                floor
                            })
                        });
                        sample.head.position_xyz[1] -= floor;
                        if floor_logged_at.is_none_or(|at: std::time::Instant| {
                            at.elapsed().as_secs() >= 30
                        }) {
                            floor_logged_at = Some(std::time::Instant::now());
                            log::info!(
                                "Air Link head {:.2} m above the floor (floor y={floor:.3}, {})",
                                sample.head.position_xyz[1],
                                if reported_floor_y.is_some() {
                                    "reported by the headset"
                                } else {
                                    "assumed"
                                }
                            );
                        }
                        for hand in [&mut sample.left_hand, &mut sample.right_hand] {
                            if let Some(hand) = hand {
                                hand.position_xyz[1] -= floor;
                            }
                        }
                        (self.on_tracking_update)(sample);
                    }
                } else if topic == 12 {
                    if let Some(input) = crate::tracking::unpack_controller_input(&payload) {
                        if let Some(height) = input.origin_height {
                            reported_floor_y = Some(-height);
                        }
                        (self.on_controller_input)(input);
                    }
                }
            } else if (topic == ripc::SERVER_TRAFFIC_TOPIC
                || topic == ripc::CLIENT_TRAFFIC_TOPIC)
                && ripc::is_handshake(&payload)
            {
                let handshake = protocol_result(ripc::HandshakeMessage::unpack(&payload))?;
                match (topic, handshake.handshake_pass) {
                    (ripc::CLIENT_TRAFFIC_TOPIC, ripc::PASS_CLIENT_CAPABILITIES) => {
                        let server_capabilities = ripc::HandshakeMessage {
                            reflect_version: handshake.reflect_version,
                            handshake_pass: ripc::PASS_SERVER_CAPABILITIES,
                            payload: handshake.payload.clone(),
                        };
                        let response = protocol_result(server_capabilities.pack())?;
                        self.send(stream, topic, &response).await?;
                        let reverse_client_capabilities = ripc::HandshakeMessage {
                            reflect_version: handshake.reflect_version,
                            handshake_pass: ripc::PASS_CLIENT_CAPABILITIES,
                            payload: handshake.payload,
                        };
                        let response = protocol_result(reverse_client_capabilities.pack())?;
                        self.send(stream, ripc::SERVER_TRAFFIC_TOPIC, &response).await?;
                    }
                    (ripc::SERVER_TRAFFIC_TOPIC, ripc::PASS_SERVER_CAPABILITIES) => {
                        let agreed_capabilities = ripc::HandshakeMessage {
                            reflect_version: handshake.reflect_version,
                            handshake_pass: ripc::PASS_AGREED_CAPABILITIES,
                            payload: handshake.payload,
                        };
                        let response = protocol_result(agreed_capabilities.pack())?;
                        self.send(stream, ripc::SERVER_TRAFFIC_TOPIC, &response).await?;
                    }
                    _ => log::debug!(
                        "Unhandled Air Link RIPC handshake topic={topic} pass={}",
                        handshake.handshake_pass
                    ),
                }
            } else if topic == ripc::SERVER_TRAFFIC_TOPIC
                || topic == ripc::CLIENT_TRAFFIC_TOPIC
            {
                if let Some(response) = ripc::gpu_buffer_handshake_ack(&payload) {
                    if !gpu_buffer_handshake_ack_sent {
                        if !gpu_initial_frame_sent {
                            self.send(
                                stream,
                                ripc::GPU_BUFFER_STREAM_TOPIC,
                                &ripc::gpu_buffer_stream_initial_frame(),
                            )
                            .await?;
                            gpu_initial_frame_sent = true;
                        }
                        self.send(stream, topic, &response).await?;
                        gpu_buffer_handshake_ack_sent = true;
                        runtime_service.buffer_pool_acked = true;
                        if let Some(pending) = runtime_service.pending_gpu_connect_response.take()
                        {
                            self.send(stream, ripc::CLIENT_TRAFFIC_TOPIC, &pending).await?;
                            runtime_service.gpu_stream_ready = true;
                            log::info!("Air Link GPU stream ready");
                            // Frames encoded so far were dropped; start on a
                            // fresh keyframe.
                            (self.on_keyframe_needed)();
                        }
                    }
                } else if let Ok(message) = ripc::RuntimeIpcMessage::unpack(&payload) {
                    self.handle_runtime_ipc(
                        stream, topic, message, &mut runtime_service, quest_endpoint_uuid,
                    ).await?;
                }
            }
        }
    }

    async fn handle_runtime_ipc(
        &mut self,
        stream: &mut TlsStream<TcpStream>,
        topic: u32,
        message: ripc::RuntimeIpcMessage,
        runtime_service: &mut RuntimeServiceState,
        quest_endpoint_uuid: Option<[u8; 16]>,
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let header = message.header;
        if header.message_direction == ripc::MESSAGE_DIRECTION_RESPONSE {
            if topic == ripc::SERVER_TRAFFIC_TOPIC
                && header.message_type == ripc::MESSAGE_TYPE_CONNECT_TO_REMOTE_SERVER
                && header.request_id == 1
                && runtime_service.reverse_client_id.is_some()
                && ripc::response_success(&message.payload)
            {
                let client_id = runtime_service.reverse_client_id.unwrap();
                let endpoint_uuid = quest_endpoint_uuid.ok_or_else(|| {
                    Error::new(ErrorKind::InvalidData, "Quest endpoint UUID is unavailable")
                })?;
                let request = protocol_result(ripc::pack_runtime_service_reverse_connect(
                    &endpoint_uuid, client_id, 2,
                ))?;
                self.send(stream, ripc::SERVER_TRAFFIC_TOPIC, &request).await?;
            } else if topic == ripc::SERVER_TRAFFIC_TOPIC
                && header.message_type == ripc::MESSAGE_TYPE_CONNECT_TO_REMOTE_SERVER
                && header.request_id == 2
                && runtime_service.reverse_client_id.is_some()
                && ripc::response_success(&message.payload)
            {
                let client_id = runtime_service.reverse_client_id.unwrap();
                let method = ripc::RUNTIME_SERVICE_BOOTSTRAP_METHODS[0];
                let request = protocol_result(ripc::pack_runtime_service_bootstrap(
                    client_id, 3, method, &[],
                ))?;
                self.send(stream, ripc::SERVER_TRAFFIC_TOPIC, &request).await?;
                runtime_service.bootstrap_index = 1;
            } else if topic == ripc::SERVER_TRAFFIC_TOPIC
                && header.message_type == ripc::MESSAGE_TYPE_CLIENT_EVENT
                && header.request_id >= 3
                && runtime_service.reverse_client_id == Some(header.client_id)
                && runtime_service.bootstrap_index < ripc::RUNTIME_SERVICE_BOOTSTRAP_METHODS.len()
            {
                if runtime_service.guardian.update(&message.payload) {
                    let guardian = &runtime_service.guardian;
                    log::info!(
                        "Air Link Guardian: floor {:?}, {} outline points, play area {:?}",
                        guardian.floor_y,
                        guardian.perimeter.len(),
                        guardian.play_area
                    );
                    (self.on_guardian_update)(guardian.clone());
                }
                let index = runtime_service.bootstrap_index;
                let next_method = ripc::RUNTIME_SERVICE_BOOTSTRAP_METHODS[index];
                if index == 6 {
                    if let Some(route) = runtime_service.streaming_route {
                        let event = protocol_result(ripc::pack_gpu_stream_channel_config_event(route))?;
                        self.send(stream, ripc::CLIENT_TRAFFIC_TOPIC, &event).await?;
                        let level = crate::video::sharpening_level();
                        let event = protocol_result(ripc::pack_sharpening_event(route, level))?;
                        self.send(stream, ripc::CLIENT_TRAFFIC_TOPIC, &event).await?;
                        log::info!("sent Air Link sharpening level {level}");
                    } else {
                        log::warn!("Air Link GPU config event skipped: no streaming invite route");
                    }
                }
                let request = protocol_result(ripc::pack_runtime_service_bootstrap(
                    header.client_id,
                    header.request_id + 1,
                    next_method,
                    if index == 6 { &ripc::RUNTIME_SERVICE_GPU_PREPARE_REQUEST } else { &[] },
                ))?;
                self.send(stream, ripc::SERVER_TRAFFIC_TOPIC, &request).await?;
                runtime_service.bootstrap_index += 1;
            }
            return Ok(());
        }
        if header.message_direction != ripc::MESSAGE_DIRECTION_REQUEST {
            return Ok(());
        }

        match header.message_type {
            ripc::MESSAGE_TYPE_CONNECT_TO_REMOTE_SERVER => {
                if topic == ripc::CLIENT_TRAFFIC_TOPIC
                    && message.payload.windows(b"OafRipcServer".len()).any(|w| w == b"OafRipcServer")
                {
                    runtime_service.oaf_client_route = Some(header);
                }
                let rpc = ripc::RuntimeRpcRequest::unpack(&message.payload).ok();
                if let Some(rpc) = rpc.filter(|rpc| {
                    rpc.method_hash == ripc::RPC_METHOD_SESSION_INVITE_OK_ACK
                        || rpc.method_hash == ripc::RPC_METHOD_SESSION_INVITE_OK_ACK_LEGACY
                }) {
                    self.respond_session_invite_ok(stream, topic, header, rpc)
                        .await?;
                    if runtime_service.streaming_invite_accepted
                        && !runtime_service.reverse_connect_started
                    {
                        self.start_reverse_runtime_service(
                            stream, runtime_service, quest_endpoint_uuid,
                        ).await?;
                    }
                } else {
                    let response = protocol_result(
                        ripc::pack_connect_to_remote_server_response(header),
                    )?;
                    self.send(stream, topic, &response).await?;
                }
            }
            ripc::MESSAGE_TYPE_CLIENT_EVENT => {
                if let Ok(rpc) = ripc::RuntimeRpcRequest::unpack(&message.payload) {
                    if rpc.method_hash == ripc::RPC_METHOD_SESSION_INVITE_OK_ACK
                        || rpc.method_hash == ripc::RPC_METHOD_SESSION_INVITE_OK_ACK_LEGACY
                    {
                        self.respond_session_invite_ok(stream, topic, header, rpc).await?;
                        if runtime_service.streaming_invite_accepted
                            && !runtime_service.reverse_connect_started
                        {
                            self.start_reverse_runtime_service(
                                stream, runtime_service, quest_endpoint_uuid,
                            ).await?;
                        }
                        return Ok(());
                    }
                    if rpc.method_hash == ripc::GPU_STREAM_CHANNEL_DID_CONNECT_METHOD
                        || rpc.method_hash == 0xc1ae_5831
                    {
                        let response = protocol_result(ripc::pack_runtime_rpc_success_response(
                            header, rpc, ripc::GPU_STREAM_CHANNEL_DID_CONNECT_RESPONSE,
                        ))?;
                        if runtime_service.buffer_pool_acked {
                            self.send(stream, topic, &response).await?;
                            runtime_service.gpu_stream_ready = true;
                            (self.on_keyframe_needed)();
                        } else {
                            // Windows answers this call only after it has
                            // acknowledged the channel's buffer-pool handshake.
                            // Video sent before that exchange is lost, the
                            // keyframe included.
                            runtime_service.pending_gpu_connect_response = Some(response);
                        }
                        return Ok(());
                    }
                }
                let event = protocol_result(ripc::ClientEvent::unpack(&message.payload))?;
                match event.handler_id {
                    ripc::CLIENT_EVENT_PERIODIC_REPORT => {
                        let response = protocol_result(ripc::pack_success_response(
                            header,
                            1,
                            ripc::RPC_RESPONSE_TYPE_PERIODIC_REPORT,
                        ))?;
                        self.send(stream, topic, &response).await?;
                    }
                    ripc::CLIENT_EVENT_NOTIFY_CLIENT_IS_READY => {
                        let response = protocol_result(ripc::pack_success_response(
                            header,
                            1,
                            ripc::RPC_RESPONSE_TYPE_NOTIFY_CLIENT_IS_READY,
                        ))?;
                        self.send(stream, topic, &response).await?;
                    }
                    ripc::CLIENT_EVENT_SESSION_GOT_INVITED => {
                        if event.event_type_hash != ripc::REFLECT_INVITE_MESSAGE_TYPE_HASH {
                            log::warn!("Air Link session invite has unexpected type hash");
                            return Ok(());
                        }
                        let (session_type, session_id) =
                            protocol_result(ripc::extract_invite(&event.event_data))?;
                        if session_type != 1 && session_type != 3 {
                            log::warn!("Air Link session type {session_type} is unsupported");
                            return Ok(());
                        }
                        let response = protocol_result(ripc::pack_success_response(
                            header,
                            1,
                            ripc::RPC_RESPONSE_TYPE_SESSION_GOT_INVITED,
                        ))?;
                        self.send(stream, topic, &response).await?;
                        if session_type == 3 {
                            runtime_service.streaming_route = Some(header);
                        }
                        tokio::time::sleep(std::time::Duration::from_millis(860)).await;
                        let accepted = protocol_result(ripc::pack_session_invite_accepted_event(
                            header,
                            &session_id,
                            session_type,
                        ))?;
                        self.send(stream, topic, &accepted).await?;
                        if session_type == 3 {
                            runtime_service.streaming_invite_accepted = true;
                            if !self.streaming_active {
                                (self.on_streaming_started)();
                                self.streaming_active = true;
                                if let Some(status) = STATUS.lock().unwrap().as_mut() {
                                    status.streaming = true;
                                }
                            }
                        }
                    }
                    _ => {
                        log::debug!("Unhandled Air Link client event 0x{:08x}", event.handler_id);
                    }
                }
            }
            ripc::MESSAGE_TYPE_CHANNEL_CREATE => {
                let channel =
                    protocol_result(ripc::ChannelCreateRequest::unpack(&message.payload))?;
                let response = protocol_result(ripc::pack_channel_create_response(header, &channel))?;
                self.send(stream, topic, &response).await?;
            }
            _ => log::debug!(
                "Unhandled RuntimeIPC request type {} on topic {topic}",
                header.message_type
            ),
        }
        Ok(())
    }

    async fn respond_session_invite_ok(
        &self,
        stream: &mut TlsStream<TcpStream>,
        topic: u32,
        header: ripc::RuntimeIpcHeader,
        rpc: ripc::RuntimeRpcRequest,
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let response = protocol_result(ripc::pack_runtime_rpc_success_response(
            header,
            rpc,
            ripc::RPC_RESPONSE_TYPE_SESSION_INVITE_OK_ACK,
        ))?;
        self.send(stream, topic, &response).await
    }

    async fn start_reverse_runtime_service(
        &self,
        stream: &mut TlsStream<TcpStream>,
        state: &mut RuntimeServiceState,
        quest_endpoint_uuid: Option<[u8; 16]>,
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let endpoint_uuid = quest_endpoint_uuid.ok_or_else(|| {
            Error::new(ErrorKind::InvalidData, "Quest endpoint UUID is unavailable")
        })?;
        let random = SystemRandom::new();
        let mut create_bytes = [0; 4];
        random.fill(&mut create_bytes).map_err(|_| Error::other("system random generator failed"))?;
        let create_client_id = u32::from_le_bytes(create_bytes).max(1);
        let mut reverse_bytes = [0; 4];
        random.fill(&mut reverse_bytes).map_err(|_| Error::other("system random generator failed"))?;
        let reverse_client_id = u32::from_le_bytes(reverse_bytes).max(1);
        let create = protocol_result(ripc::pack_runtime_service_client_create(create_client_id))?;
        self.send(stream, ripc::SERVER_TRAFFIC_TOPIC, &create).await?;
        let connect = protocol_result(ripc::pack_runtime_service_reverse_connect(
            &endpoint_uuid, 0xf000_000c, 1,
        ))?;
        self.send(stream, ripc::SERVER_TRAFFIC_TOPIC, &connect).await?;
        state.reverse_client_id = Some(reverse_client_id);
        state.reverse_connect_started = true;
        log::info!("Started native Air Link RuntimeServiceServer reverse connection");
        Ok(())
    }

    async fn send(
        &self,
        stream: &mut TlsStream<TcpStream>,
        topic: u32,
        payload: &[u8],
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        if topic == ripc::SERVER_TRAFFIC_TOPIC
            || topic == ripc::CLIENT_TRAFFIC_TOPIC
            || (topic == MESH_TOPIC && payload.len() > 28)
        {
            trace_frame("tx", topic, payload);
        }
        transport::write_frame(stream, topic, payload).await
    }
}

/// One line per RuntimeIPC frame while tracing is enabled: enough to
/// follow a session next to the headset's log and to the reference host.
fn trace_frame(direction: &str, topic: u32, payload: &[u8]) {
    static ENABLED: std::sync::LazyLock<bool> =
        std::sync::LazyLock::new(|| {
            // SteamVR starts the driver, so a marker file is easier than an
            // environment variable: touch ~/.config/alvr/airlink_trace.
            std::env::var_os("ALVR_AIRLINK_TRACE").is_some()
                || std::env::var_os("HOME").is_some_and(|home| {
                    Path::new(&home).join(".config/alvr/airlink_trace").exists()
                })
        });
    if !*ENABLED {
        return;
    }
    let head: String = payload
        .iter()
        .skip(16)
        .take(40)
        .map(|byte| format!("{byte:02x}"))
        .collect();
    log::info!("airlink {direction} topic={topic} size={} {head}", payload.len());
}

fn parse_uuid_bytes(value: &str) -> Result<[u8; 16], Box<dyn std::error::Error + Send + Sync>> {
    Ok(uuid::Uuid::parse_str(value)?.into_bytes())
}

fn protocol_result<T>(
    result: Result<T, &'static str>,
) -> Result<T, Box<dyn std::error::Error + Send + Sync>> {
    result.map_err(|error| Error::new(ErrorKind::InvalidData, error).into())
}

fn unix_timestamp_ms() -> u128 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
}

#[cfg(unix)]
/// Extra time between a frame's send time and the display time written into
/// it, in milliseconds: 8, or what `~/.config/alvr/airlink_display_lead_ms`
/// says. With none the headset logged frames ready 1.7 to 6.7 ms before their
/// release (`window_min_release_margin_ms`), against 10.7 to 12.7 ms with
/// Horizon, and repeated frames for a second whenever video and display
/// slipped by one frame.
fn display_lead_ms() -> f64 {
    std::env::var_os("HOME")
        .and_then(|home| {
            std::fs::read_to_string(
                std::path::Path::new(&home).join(".config/alvr/airlink_display_lead_ms"),
            )
            .ok()
        })
        .and_then(|text| text.trim().parse().ok())
        .unwrap_or(8.0)
}

/// How far ahead the head pose is predicted, in milliseconds: 40, or what
/// `ALVR_AIRLINK_PREDICT_MS` or `~/.config/alvr/airlink_predict_ms` says
/// (0 turns prediction off), and never over ALVR's "Maximum prediction".
fn head_prediction_ms() -> f64 {
    uncapped_head_prediction_ms().min(crate::video::head_prediction_cap_ms() as f64)
}

fn uncapped_head_prediction_ms() -> f64 {
    std::env::var("ALVR_AIRLINK_PREDICT_MS")
        .ok()
        .or_else(|| {
            let home = std::env::var_os("HOME")?;
            std::fs::read_to_string(
                std::path::Path::new(&home).join(".config/alvr/airlink_predict_ms"),
            )
            .ok()
        })
        .and_then(|text| text.trim().parse().ok())
        .unwrap_or(40.0)
}

fn monotonic_nanos() -> u64 {
    let mut value = std::mem::MaybeUninit::<libc::timespec>::uninit();
    // SAFETY: clock_gettime initializes the supplied timespec on success.
    let result = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, value.as_mut_ptr()) };
    if result == 0 {
        // SAFETY: the successful clock_gettime call initialized the value.
        let value = unsafe { value.assume_init() };
        (value.tv_sec as u64)
            .saturating_mul(1_000_000_000)
            .saturating_add(value.tv_nsec as u64)
    } else {
        0
    }
}

#[cfg(not(unix))]
fn monotonic_nanos() -> u64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos() as u64
}

fn unsigned_cert_exchange(
    signature_size: usize,
) -> Result<CertExchange, Box<dyn std::error::Error + Send + Sync>> {
    let random = SystemRandom::new();
    let mut signature = vec![0; signature_size];
    let mut nonce = [0; 32];
    random
        .fill(&mut signature)
        .map_err(|_| Error::other("system random generator failed"))?;
    random
        .fill(&mut nonce)
        .map_err(|_| Error::other("system random generator failed"))?;
    Ok(CertExchange {
        metadata: Vec::new(),
        signature,
        random: nonce,
    })
}
