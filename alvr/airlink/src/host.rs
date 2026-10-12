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
use crate::video::AirLinkVideoFrame;
use crate::tracking::{AirLinkControllerInput, AirLinkTrackingSample};
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

pub struct AirLinkHost {
    listener: AirLinkListener,
    _advertiser: AirLinkAdvertiser,
    identity: HostIdentity,
    endpoint_name: String,
    video_receiver: broadcast::Receiver<AirLinkVideoFrame>,
    on_streaming_started: Arc<dyn Fn() + Send + Sync>,
    on_streaming_stopped: Arc<dyn Fn() + Send + Sync>,
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
    gpu_stream_ready: bool,
}

impl AirLinkHost {
    pub async fn bind(
        state_dir: impl AsRef<Path>,
        address: SocketAddr,
        advertise_address: IpAddr,
        hostname: &str,
        endpoint_name: &str,
        video_receiver: broadcast::Receiver<AirLinkVideoFrame>,
        on_streaming_started: Arc<dyn Fn() + Send + Sync>,
        on_streaming_stopped: Arc<dyn Fn() + Send + Sync>,
        on_tracking_update: Arc<dyn Fn(AirLinkTrackingSample) + Send + Sync>,
        on_controller_input: Arc<dyn Fn(AirLinkControllerInput) + Send + Sync>,
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
            _advertiser: advertiser,
            identity,
            endpoint_name: endpoint_name.to_owned(),
            video_receiver,
            on_streaming_started,
            on_streaming_stopped,
            on_tracking_update,
            on_controller_input,
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
        loop {
            let accepted = tokio::select! {
                _ = &mut shutdown => return Ok(()),
                result = self.listener.accept() => result,
            };
            let (mut stream, peer) = match accepted {
                Ok(connection) => connection,
                Err(error) => {
                    log::warn!("Meta Air Link TLS accept failed: {error}");
                    continue;
                }
            };
            log::info!("Meta Air Link TLS session established from {peer}");
            let result = tokio::select! {
                _ = &mut shutdown => return Ok(()),
                result = self.serve_connection(&mut stream) => result,
            };
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
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let mut gpu_buffer_handshake_ack_sent = false;
        let mut gpu_initial_frame_sent = false;
        let mut quest_endpoint_uuid = None;
        let mut runtime_service = RuntimeServiceState::default();
        let mut next_video_frame_id = 2u32;
        let mut headset_offset_ns = 0i64;
        loop {
            let (topic, payload) = tokio::select! {
                received = self.video_receiver.recv() => {
                    match received {
                        Ok(frame) if runtime_service.gpu_stream_ready => {
                            let packet = frame.pack(
                                next_video_frame_id,
                                monotonic_nanos(),
                                headset_offset_ns,
                            );
                            self.send(stream, ripc::GPU_BUFFER_STREAM_TOPIC, &packet).await?;
                            next_video_frame_id = next_video_frame_id.saturating_add(1);
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
                incoming = transport::read_frame(stream) => incoming?,
            };
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
                        let response = protocol_result(
                            response.pack(disco::MSG_EXCHANGING_CERT_ACK),
                        )?;
                        self.send(stream, topic, &response).await?;
                    }
                    MSG_AWAITING_USER_REQ => {
                        self.send(stream, topic, &disco::pack_awaiting_user_ack()).await?;
                    }
                    MSG_COMMITMENT => {
                        log::debug!("Air Link pairing commitment received");
                    }
                    _ => log::debug!("Unhandled Air Link pairing message 0x{message:08x}"),
                }
            } else if (2..=14).contains(&topic) {
                if let Some(response) = ripc::channel_buffer_handshake_ack(&payload) {
                    self.send(stream, topic, response).await?;
                } else if topic == 11 {
                    if let Some(sample) = crate::tracking::unpack_tracking(&payload) {
                        (self.on_tracking_update)(sample);
                    }
                } else if topic == 12 {
                    if let Some(input) = crate::tracking::unpack_controller_input(&payload) {
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
                                &ripc::GPU_BUFFER_STREAM_INITIAL_FRAME,
                            )
                            .await?;
                            gpu_initial_frame_sent = true;
                        }
                        self.send(stream, topic, &response).await?;
                        gpu_buffer_handshake_ack_sent = true;
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
                let index = runtime_service.bootstrap_index;
                let next_method = ripc::RUNTIME_SERVICE_BOOTSTRAP_METHODS[index];
                if index == 6 {
                    if let Some(route) = runtime_service.streaming_route {
                        let event = protocol_result(ripc::pack_gpu_stream_channel_config_event(route))?;
                        self.send(stream, ripc::CLIENT_TRAFFIC_TOPIC, &event).await?;
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
                    runtime_service.streaming_route = Some(header);
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
                        self.send(stream, topic, &response).await?;
                        runtime_service.gpu_stream_ready = true;
                        return Ok(());
                    }
                }
                let event = protocol_result(ripc::ClientEvent::unpack(&message.payload))?;
                match event.handler_id {
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
        transport::write_frame(stream, topic, payload).await
    }
}

fn parse_uuid_bytes(value: &str) -> Result<[u8; 16], Box<dyn std::error::Error + Send + Sync>> {
    Ok(uuid::Uuid::parse_str(value)?.into_bytes())
}

fn protocol_result<T>(
    result: Result<T, &'static str>,
) -> Result<T, Box<dyn std::error::Error + Send + Sync>> {
    result.map_err(|error| Error::new(ErrorKind::InvalidData, error).into())
}

#[cfg(unix)]
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
