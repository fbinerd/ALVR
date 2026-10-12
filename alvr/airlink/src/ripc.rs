//! RuntimeIPC transport framing used by the Meta Air Link host.

const HANDSHAKE_TAG: u32 = 0xff75_3234;
const HANDSHAKE_HEADER_SIZE: u64 = 8;

pub const SERVER_TRAFFIC_TOPIC: u32 = 0;
pub const CLIENT_TRAFFIC_TOPIC: u32 = 1;
pub const PASS_CLIENT_CAPABILITIES: u16 = 1;
pub const PASS_SERVER_CAPABILITIES: u16 = 2;
pub const PASS_AGREED_CAPABILITIES: u16 = 3;
pub const GPU_BUFFER_STREAM_TOPIC: u32 = 15;
pub const MESSAGE_TYPE_CONNECT_TO_REMOTE_SERVER: u32 = 1;
pub const MESSAGE_TYPE_CLIENT_EVENT: u32 = 2;
pub const MESSAGE_TYPE_SESSION_EVENT: u32 = 11;
pub const MESSAGE_TYPE_CHANNEL_CREATE: u32 = 15;
pub const MESSAGE_TYPE_CLIENT_CREATE: u32 = 4;
pub const MESSAGE_DIRECTION_REQUEST: u8 = 0;
pub const MESSAGE_DIRECTION_RESPONSE: u8 = 1;
pub const CLIENT_EVENT_NOTIFY_CLIENT_IS_READY: u32 = 0xe205_7b8e;
pub const CLIENT_EVENT_SESSION_GOT_INVITED: u32 = 0xf797_a5a6;
pub const CLIENT_EVENT_SESSION_INVITE_ACCEPTED: u32 = 0x25f8_9b91;
pub const REFLECT_INVITE_MESSAGE_TYPE_HASH: u32 = 0x5253_77b9;
pub const RPC_RESPONSE_TYPE_NOTIFY_CLIENT_IS_READY: u32 = 0x2f6f_239d;
pub const RPC_RESPONSE_TYPE_SESSION_GOT_INVITED: u32 = 0xf94e_6db5;
pub const REFLECT_INVITE_OK_MESSAGE_TYPE_HASH: u32 = 0x1b13_3613;
pub const RPC_METHOD_SESSION_INVITE_OK_ACK: u32 = 0x58c8_6d54;
pub const RPC_METHOD_SESSION_INVITE_OK_ACK_LEGACY: u32 = 0x06c8_6661;
pub const RPC_RESPONSE_TYPE_SESSION_INVITE_OK_ACK: u32 = 0x2730_1990;
pub const GPU_STREAM_CHANNEL_DID_CREATE_HANDLER: u32 = 0x5b64_f8db;
pub const GPU_STREAM_CHANNEL_DID_CREATE_TYPE: u32 = 0xb67a_b64c;
pub const GPU_STREAM_CHANNEL_DID_CONNECT_METHOD: u32 = 0x01aa_1146;
pub const GPU_STREAM_CHANNEL_DID_CONNECT_RESPONSE: u32 = 0xbc8a_5740;
pub const RUNTIME_SERVICE_BOOTSTRAP_METHODS: [u32; 7] = [
    0x680c_c30a, 0x3ed8_3bbe, 0x62a4_153c, 0x680c_c30a,
    0x3ed8_3bbe, 0x62a4_153c, 0xec6c_96c7,
];
pub const RUNTIME_SERVICE_GPU_PREPARE_REQUEST: [u8; 5] = [0x08, 0x6c, 0x16, 0, 0];

const CHANNEL_BUFFER_HANDSHAKE: [u8; 28] = [
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 5, 0, 0, 0, 1, 1, 0, 0, 0,
];
const GPU_BUFFER_HANDSHAKE_SIZE: usize = 70;
const GPU_BUFFER_HANDSHAKE_HEADER_SIZE: u64 = 42;
const GPU_BUFFER_HANDSHAKE_PAYLOAD_SIZE: u64 = 12;
const GPU_BUFFER_HANDSHAKE_ACK_OFFSET: usize = 53;

pub const GPU_BUFFER_STREAM_INITIAL_FRAME: [u8; 23] = [
    0x17, 0x9c, 0x04, 0, 0, 0, 0, 0x0f, 0x15, 0x02, 0x15, 0xc0, 0x39, 0x15, 0x80, 0x1e, 0x15,
    0x04, 0x15, 0x04, 0x15, 0, 0,
];

const MESSAGE_HEADER_SIZE: usize = 32;
pub const GPU_BUFFER_CHANNEL_NAME: &str = "gpu_buffer_stream_channel";
const RUNTIME_SERVICE_CLIENT_CREATE_PAYLOAD_HEX: &str = concat!(
    "1b000000289f04c117000000636f6d2e6f63756c75732e73797374656d647269",
    "7665723000000000defe742c000000636f6d2e6f63756c75732e767272756e74",
    "696d65736572766963652e567252756e74696d655365727669636500000000"
);

fn reflect_string(value: &str) -> Vec<u8> {
    let mut result = Vec::with_capacity(value.len() + 4);
    result.extend_from_slice(&(value.len() as u32).to_le_bytes());
    result.extend_from_slice(value.as_bytes());
    result
}

fn tagged_records(records: &[(u32, Vec<u8>)]) -> Result<Vec<u8>, &'static str> {
    let mut result = Vec::new();
    for (field_id, value) in records {
        tagged_record(*field_id, value, &mut result)?;
    }
    result.extend_from_slice(&[0; 4]);
    Ok(result)
}

pub fn pack_runtime_service_client_create(client_id: u32) -> Result<Vec<u8>, &'static str> {
    let payload = decode_hex(RUNTIME_SERVICE_CLIENT_CREATE_PAYLOAD_HEX)?;
    let header = RuntimeIpcHeader {
        message_type: MESSAGE_TYPE_CLIENT_CREATE,
        message_size: payload.len() as u32,
        client_id,
        server_id: 9,
        one_way: false,
        request_id: 0,
        memory_id: 0,
        message_direction: MESSAGE_DIRECTION_REQUEST,
        rpc_failure_status: 0,
    };
    RuntimeIpcMessage::pack(header, &payload)
}

pub fn pack_runtime_service_reverse_connect(
    quest_uuid: &[u8; 16], client_id: u32, request_id: u32,
) -> Result<Vec<u8>, &'static str> {
    let endpoint = uuid::Uuid::from_bytes(*quest_uuid);
    let mut custom_data = vec![16, 0, 0, 0];
    custom_data.extend_from_slice(&[5, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]);
    let payload = tagged_records(&[
        (0xc104_9f28, reflect_string("com.oculus.systemdriver")),
        (0x6a63_18fb, reflect_string("com.oculus.vrruntimeservice")),
        (0x9877_44f3, reflect_string("RuntimeServiceServer")),
        (0x69cb_fee9, reflect_string(&format!("highwind_FixedLan_{endpoint}"))),
        (0x7f71_62ed, 15u32.to_le_bytes().to_vec()),
        (0x3169_63db, reflect_string("")),
        (0x7240_121b, vec![0]),
        (0xe741_bc02, 5u32.to_le_bytes().to_vec()),
        (0x470a_0fac, reflect_string("")),
        (0xe5b0_fdd0, custom_data),
    ])?;
    let header = RuntimeIpcHeader {
        message_type: MESSAGE_TYPE_CONNECT_TO_REMOTE_SERVER,
        message_size: payload.len() as u32,
        client_id,
        server_id: 12,
        one_way: false,
        request_id,
        memory_id: 0,
        message_direction: MESSAGE_DIRECTION_REQUEST,
        rpc_failure_status: 0,
    };
    RuntimeIpcMessage::pack(header, &payload)
}

pub fn pack_runtime_service_bootstrap(
    client_id: u32, request_id: u32, method_hash: u32, serialized_request: &[u8],
) -> Result<Vec<u8>, &'static str> {
    let mut payload = Vec::with_capacity(7 + serialized_request.len());
    payload.extend_from_slice(&5u16.to_le_bytes());
    payload.extend_from_slice(&method_hash.to_le_bytes());
    payload.push(0x60);
    payload.extend_from_slice(serialized_request);
    let header = RuntimeIpcHeader {
        message_type: MESSAGE_TYPE_CLIENT_EVENT,
        message_size: payload.len() as u32,
        client_id,
        server_id: 12,
        one_way: false,
        request_id,
        memory_id: method_hash,
        message_direction: MESSAGE_DIRECTION_REQUEST,
        rpc_failure_status: 0,
    };
    RuntimeIpcMessage::pack(header, &payload)
}

pub fn response_success(payload: &[u8]) -> bool {
    let mut offset = 0;
    while offset + 8 <= payload.len() {
        let size = u32::from_le_bytes(payload[offset..offset + 4].try_into().unwrap()) as usize;
        if size == 0 { return false; }
        let field = u32::from_le_bytes(payload[offset + 4..offset + 8].try_into().unwrap());
        let end = match (offset + 8).checked_add(size) { Some(end) if end <= payload.len() => end, _ => return false };
        if field == 0xf47b_aaea { return size == 1 && payload[offset + 8] == 1; }
        offset = end;
    }
    false
}

pub fn pack_gpu_stream_channel_config_event(route: RuntimeIpcHeader) -> Result<Vec<u8>, &'static str> {
    let mut payload = Vec::from(2u16.to_le_bytes());
    payload.extend_from_slice(&GPU_STREAM_CHANNEL_DID_CREATE_HANDLER.to_le_bytes());
    payload.extend_from_slice(&GPU_STREAM_CHANNEL_DID_CREATE_TYPE.to_le_bytes());
    payload.extend_from_slice(&[0x3a, 0x60, 0x0e, 0, 0, 0x80, 7, 0, 0, 3, 0, 0, 0, 0x20, 1, 0, 0, 0, 1, 0, 2, 0, 1, 0, 0x48, 0, 0, 0, 0, 1]);
    let header = RuntimeIpcHeader {
        message_type: MESSAGE_TYPE_SESSION_EVENT,
        message_size: payload.len() as u32,
        client_id: route.client_id,
        server_id: route.server_id,
        one_way: false,
        request_id: 0,
        memory_id: 0,
        message_direction: MESSAGE_DIRECTION_RESPONSE,
        rpc_failure_status: 0,
    };
    RuntimeIpcMessage::pack(header, &payload)
}

const GPU_BUFFER_CHANNEL_RESPONSE_BODY_HEX: &str = concat!(
    "a006196770755f6275666665725f73747265616d5f6368616e6e656c",
    "6dee56c1d805196770755f6275666665725f73747265616d5f6368616e6e656c",
    "0000000006fa0200031100000000000001000000030000000001d20200000000",
    "600e000080070000018e02000000000080010103780dd8491dd914e86f0c6805",
    "02020403070311031b0301000000040400000200000000000000000000000000",
    "000001f3846b4800000001000000000000009a99993f0fff3434000000000000",
    "000000000000809698008096980000e1f505809698000400000001361e3d600e",
    "0000800700000010000070080000000000000308000001000004000100000400",
    "01000000001b5317000002000000030000000200000001100000040004000000",
    "001b5318000002000000030000000200000001100000040004000000001b5319",
    "000002000000030000000200000001100000040004000000001b541700000200",
    "0000030000000200000001100000040004000000001b0c93af12000000000300",
    "0000020000000110041000000400000000000e000000"
);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RuntimeIpcHeader {
    pub message_type: u32,
    pub message_size: u32,
    pub client_id: u32,
    pub server_id: u32,
    pub one_way: bool,
    pub request_id: u32,
    pub memory_id: u32,
    pub message_direction: u8,
    pub rpc_failure_status: u32,
}

impl RuntimeIpcHeader {
    fn unpack(data: &[u8]) -> Result<Self, &'static str> {
        if data.len() != MESSAGE_HEADER_SIZE || data[0] != 0x3e || data[26] != 0 {
            return Err("invalid RuntimeIPC message header");
        }
        Ok(Self {
            message_type: u32::from_le_bytes(data[1..5].try_into().unwrap()),
            message_size: u32::from_le_bytes(data[5..9].try_into().unwrap()),
            client_id: u32::from_le_bytes(data[9..13].try_into().unwrap()),
            server_id: u32::from_le_bytes(data[13..17].try_into().unwrap()),
            one_way: data[17] != 0,
            request_id: u32::from_le_bytes(data[18..22].try_into().unwrap()),
            memory_id: u32::from_le_bytes(data[22..26].try_into().unwrap()),
            message_direction: data[27],
            rpc_failure_status: u32::from_le_bytes(data[28..32].try_into().unwrap()),
        })
    }

    fn pack(self) -> [u8; MESSAGE_HEADER_SIZE] {
        let mut data = [0; MESSAGE_HEADER_SIZE];
        data[0] = 0x3e;
        data[1..5].copy_from_slice(&self.message_type.to_le_bytes());
        data[5..9].copy_from_slice(&self.message_size.to_le_bytes());
        data[9..13].copy_from_slice(&self.client_id.to_le_bytes());
        data[13..17].copy_from_slice(&self.server_id.to_le_bytes());
        data[17] = u8::from(self.one_way);
        data[18..22].copy_from_slice(&self.request_id.to_le_bytes());
        data[22..26].copy_from_slice(&self.memory_id.to_le_bytes());
        data[27] = self.message_direction;
        data[28..32].copy_from_slice(&self.rpc_failure_status.to_le_bytes());
        data
    }

    fn response_to(self, message_type: u32, message_size: usize) -> Result<Self, &'static str> {
        Ok(Self {
            message_type,
            message_size: u32::try_from(message_size).map_err(|_| "RIPC message too large")?,
            client_id: self.client_id,
            server_id: self.server_id,
            one_way: false,
            request_id: self.request_id,
            memory_id: self.memory_id,
            message_direction: MESSAGE_DIRECTION_RESPONSE,
            rpc_failure_status: 0,
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RuntimeIpcMessage {
    pub header: RuntimeIpcHeader,
    pub payload: Vec<u8>,
}

impl RuntimeIpcMessage {
    pub fn unpack(data: &[u8]) -> Result<Self, &'static str> {
        if data.len() < 16 + MESSAGE_HEADER_SIZE {
            return Err("RuntimeIPC message is truncated");
        }
        let header_size = u64::from_le_bytes(data[..8].try_into().unwrap()) as usize;
        let payload_size = u64::from_le_bytes(data[8..16].try_into().unwrap()) as usize;
        let end_header = 16usize
            .checked_add(header_size)
            .ok_or("RuntimeIPC header size overflow")?;
        let end_payload = end_header
            .checked_add(payload_size)
            .ok_or("RuntimeIPC payload size overflow")?;
        if header_size != MESSAGE_HEADER_SIZE || end_payload != data.len() {
            return Err("RuntimeIPC message size mismatch");
        }
        let header = RuntimeIpcHeader::unpack(&data[16..end_header])?;
        if header.message_size as usize != payload_size {
            return Err("RuntimeIPC payload size disagrees with message header");
        }
        Ok(Self {
            header,
            payload: data[end_header..].to_vec(),
        })
    }

    pub fn pack(header: RuntimeIpcHeader, payload: &[u8]) -> Result<Vec<u8>, &'static str> {
        let payload_size = u64::try_from(payload.len()).map_err(|_| "RIPC payload too large")?;
        let mut data = Vec::with_capacity(16 + MESSAGE_HEADER_SIZE + payload.len());
        data.extend_from_slice(&(MESSAGE_HEADER_SIZE as u64).to_le_bytes());
        data.extend_from_slice(&payload_size.to_le_bytes());
        data.extend_from_slice(&header.pack());
        data.extend_from_slice(payload);
        Ok(data)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ClientEvent {
    pub reflect_version: u16,
    pub handler_id: u32,
    pub event_type_hash: u32,
    pub event_data: Vec<u8>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RuntimeRpcRequest {
    pub helper_protocol_version: u16,
    pub method_hash: u32,
    pub flags: Option<u8>,
}

impl RuntimeRpcRequest {
    pub fn unpack(data: &[u8]) -> Result<Self, &'static str> {
        if data.len() < 7 {
            return Err("RuntimeIPC RPC request is truncated");
        }
        let helper_protocol_version = u16::from_le_bytes([data[0], data[1]]);
        let method_hash = u32::from_le_bytes(data[2..6].try_into().unwrap());
        if helper_protocol_version < 2 {
            if data.len() < 10 {
                return Err("legacy RuntimeIPC RPC request is truncated");
            }
            Ok(Self {
                helper_protocol_version,
                method_hash,
                flags: None,
            })
        } else {
            Ok(Self {
                helper_protocol_version,
                method_hash,
                flags: Some(data[6]),
            })
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChannelCreateRequest {
    pub client_name: String,
    pub server_name: String,
    pub channel_name: String,
    pub descriptor: Vec<u8>,
}

impl ChannelCreateRequest {
    pub fn unpack(data: &[u8]) -> Result<Self, &'static str> {
        let (object_size, mut offset) = unpack_compact_size(data, 0)?;
        if object_size != data.len() - offset {
            return Err("RIPC channel request has invalid compact size");
        }
        let mut names = Vec::with_capacity(3);
        for _ in 0..3 {
            let size = *data.get(offset).ok_or("RIPC channel request is truncated")? as usize;
            offset += 1;
            let end = offset.checked_add(size).ok_or("channel name size overflow")?;
            let value = data.get(offset..end).ok_or("channel name is truncated")?;
            names.push(
                std::str::from_utf8(value)
                    .map_err(|_| "channel name is not UTF-8")?
                    .to_owned(),
            );
            offset = end;
        }
        Ok(Self {
            client_name: names.remove(0),
            server_name: names.remove(0),
            channel_name: names.remove(0),
            descriptor: data[offset..].to_vec(),
        })
    }
}

impl ClientEvent {
    pub fn unpack(data: &[u8]) -> Result<Self, &'static str> {
        if data.len() < 10 {
            return Err("RuntimeIPC client event is truncated");
        }
        Ok(Self {
            reflect_version: u16::from_le_bytes([data[0], data[1]]),
            handler_id: u32::from_le_bytes(data[2..6].try_into().unwrap()),
            event_type_hash: u32::from_le_bytes(data[6..10].try_into().unwrap()),
            event_data: data[10..].to_vec(),
        })
    }
}

pub fn reflect_field_id(type_name: &str, field_name: &str) -> u32 {
    let mut value = 0x1505u32;
    for byte in type_name.bytes().chain(field_name.bytes()) {
        let shifted = ((value << 5) & 0x7fff_ffe0).wrapping_add(value & 0x7fff_ffff);
        value = (((value << 5) ^ value ^ u32::from(byte) ^ shifted) & 0x8000_0000)
            ^ u32::from(byte).wrapping_add(shifted & 0x7fff_ffff);
    }
    value
}

fn tagged_record(field_id: u32, value: &[u8], output: &mut Vec<u8>) -> Result<(), &'static str> {
    let size = u32::try_from(value.len()).map_err(|_| "RIPC field is too large")?;
    output.extend_from_slice(&size.to_le_bytes());
    output.extend_from_slice(&field_id.to_le_bytes());
    output.extend_from_slice(value);
    Ok(())
}

pub fn unpack_tagged_records(data: &[u8]) -> Result<Vec<(u32, Vec<u8>)>, &'static str> {
    let mut records = Vec::new();
    let mut offset = 0usize;
    loop {
        let size_bytes = data
            .get(offset..offset + 4)
            .ok_or("truncated RIPC tagged record size")?;
        let size = u32::from_le_bytes(size_bytes.try_into().unwrap()) as usize;
        offset += 4;
        if size == 0 {
            if offset != data.len() {
                return Err("trailing bytes after RIPC tagged record terminator");
            }
            return Ok(records);
        }
        let field_end = offset.checked_add(4).ok_or("RIPC field offset overflow")?;
        let value_end = field_end
            .checked_add(size)
            .ok_or("RIPC field size overflow")?;
        if value_end > data.len() {
            return Err("truncated RIPC tagged record");
        }
        let field_id = u32::from_le_bytes(data[offset..field_end].try_into().unwrap());
        records.push((field_id, data[field_end..value_end].to_vec()));
        offset = value_end;
    }
}

fn pack_success_object() -> Result<Vec<u8>, &'static str> {
    let mut output = Vec::new();
    tagged_record(reflect_field_id("bool", "success"), &[1], &mut output)?;
    output.extend_from_slice(&[0; 4]);
    Ok(output)
}

pub fn pack_connect_to_remote_server_response(
    request: RuntimeIpcHeader,
) -> Result<Vec<u8>, &'static str> {
    let mut payload = Vec::new();
    tagged_record(reflect_field_id("bool", "Success"), &[1], &mut payload)?;
    tagged_record(
        reflect_field_id("bool", "clientInitializedOffline"),
        &[0],
        &mut payload,
    )?;
    payload.extend_from_slice(&[0; 4]);
    let header = request.response_to(MESSAGE_TYPE_CONNECT_TO_REMOTE_SERVER, payload.len())?;
    RuntimeIpcMessage::pack(header, &payload)
}

pub fn pack_success_response(
    request: RuntimeIpcHeader,
    helper_protocol_version: u16,
    response_type_hash: u32,
) -> Result<Vec<u8>, &'static str> {
    let response_object = pack_success_object()?;
    let mut payload = Vec::with_capacity(6 + response_object.len());
    payload.extend_from_slice(&helper_protocol_version.to_le_bytes());
    payload.extend_from_slice(&response_type_hash.to_le_bytes());
    payload.extend_from_slice(&response_object);
    let header = request.response_to(request.message_type, payload.len())?;
    RuntimeIpcMessage::pack(header, &payload)
}

pub fn pack_runtime_rpc_success_response(
    request: RuntimeIpcHeader,
    rpc: RuntimeRpcRequest,
    response_type_hash: u32,
) -> Result<Vec<u8>, &'static str> {
    let reflect_v2 = rpc.helper_protocol_version >= 5
        && rpc.flags.is_some_and(|flags| flags & 0x20 != 0);
    let serialized_response = if reflect_v2 {
        vec![2, 1]
    } else {
        pack_success_object()?
    };
    let mut payload = Vec::with_capacity(7 + serialized_response.len());
    payload.extend_from_slice(&rpc.helper_protocol_version.to_le_bytes());
    if rpc.helper_protocol_version > 4 {
        payload.push(u8::from(reflect_v2));
    }
    payload.extend_from_slice(&response_type_hash.to_le_bytes());
    payload.extend_from_slice(&serialized_response);
    let header = request.response_to(request.message_type, payload.len())?;
    RuntimeIpcMessage::pack(header, &payload)
}

pub fn pack_channel_create_response(
    request: RuntimeIpcHeader,
    channel: &ChannelCreateRequest,
) -> Result<Vec<u8>, &'static str> {
    if channel.client_name != "LinkStreamingClient" || channel.server_name != "LinkDiscoServer" {
        return Err("unsupported RIPC channel client/server pair");
    }
    let payload = if channel.channel_name == GPU_BUFFER_CHANNEL_NAME {
        decode_hex(GPU_BUFFER_CHANNEL_RESPONSE_BODY_HEX)?
    } else {
        let (type_hash, channel_id) = channel_type_info(&channel.channel_name)
            .ok_or("unsupported RIPC channel type")?;
        if channel.descriptor.len() < 8 {
            return Err("RIPC channel descriptor is shorter than its request token");
        }
        let name = channel.channel_name.as_bytes();
        let mut response_body = Vec::with_capacity(1 + name.len() + 4 + channel.descriptor.len() + 4);
        response_body.push(u8::try_from(name.len()).map_err(|_| "RIPC channel name too long")?);
        response_body.extend_from_slice(name);
        response_body.extend_from_slice(&type_hash.to_le_bytes());
        response_body.extend_from_slice(&channel.descriptor[..channel.descriptor.len() - 8]);
        response_body.extend_from_slice(&channel_id.to_le_bytes());
        let mut packed = Vec::new();
        pack_compact_size(response_body.len(), &mut packed)?;
        packed.extend_from_slice(&response_body);
        packed
    };
    let header = request.response_to(MESSAGE_TYPE_CHANNEL_CREATE, payload.len())?;
    RuntimeIpcMessage::pack(header, &payload)
}

fn channel_type_info(name: &str) -> Option<(u32, u32)> {
    Some(match name {
        "MeshChannel" => (0x0e4c_b12b, 1),
        "AudioChannel" => (0xa4b2_8c90, 2),
        "AudioControlChannel" => (0x923c_9cf1, 3),
        "HapticChannel" => (0xbcf5_02d7, 4),
        "HandsChannel" => (0x231e_baec, 5),
        "SkeletonChannel" => (0xdc1b_a4a3, 6),
        "UserSettingsSyncChannel" => (0x750e_5f2b, 7),
        "CameraStreamChannel" => (0xf3e8_2753, 8),
        "InputControlChannel" => (0x3b8e_636f, 9),
        "TrackingChannel" => (0x3188_f751, 10),
        "DeviceInputChannel" => (0xd15e_12be, 11),
        "RuntimeIPCChannel" => (0x9c99_d1fe, 12),
        "KpiReportChannel" => (0xeffd_07fe, 13),
        _ => return None,
    })
}

pub fn extract_invite(
    invite_message: &[u8],
) -> Result<(u32, String), &'static str> {
    let invite_records = unpack_tagged_records(invite_message)?;
    let invite_field_id = reflect_field_id("::xrs::XrsInvite", "invite");
    let invite_data = invite_records
        .iter()
        .find(|(field_id, _)| *field_id == invite_field_id)
        .map(|(_, value)| value.as_slice())
        .ok_or("InviteMessage has no invite field")?;
    let invite_fields = unpack_tagged_records(invite_data)?;
    let session_type = invite_fields
        .iter()
        .find(|(field_id, _)| {
            *field_id == reflect_field_id("::xrs::XrsSessionType", "sessionType")
        })
        .map(|(_, value)| value.as_slice())
        .ok_or("XrsInvite has no session type")?;
    if session_type.len() != 4 {
        return Err("XrsInvite session type is not a 32-bit value");
    }
    let session_type = u32::from_le_bytes(session_type.try_into().unwrap());

    let session_id = invite_fields
        .iter()
        .find(|(field_id, _)| {
            *field_id == reflect_field_id("std::string", "linkFlowSessionId")
        })
        .map(|(_, value)| value.as_slice())
        .ok_or("XrsInvite has no link flow session id")?;
    if session_id.len() < 4 {
        return Err("XrsInvite link flow session id is truncated");
    }
    let string_size = u32::from_le_bytes(session_id[..4].try_into().unwrap()) as usize;
    if string_size != session_id.len() - 4 {
        return Err("XrsInvite link flow session id has an invalid length");
    }
    let session_id = std::str::from_utf8(&session_id[4..])
        .map_err(|_| "XrsInvite link flow session id is not UTF-8")?
        .to_owned();
    Ok((session_type, session_id))
}

fn pack_compact_size(size: usize, output: &mut Vec<u8>) -> Result<(), &'static str> {
    let mut value = u32::try_from(size)
        .map_err(|_| "compact RIPC structure is too large")?
        .checked_mul(2)
        .ok_or("compact RIPC structure size overflow")?;
    while value >= 0x80 {
        output.push((value as u8 & 0x7f) | 0x80);
        value >>= 7;
    }
    output.push(value as u8);
    Ok(())
}

fn unpack_compact_size(data: &[u8], mut offset: usize) -> Result<(usize, usize), &'static str> {
    let mut value = 0u32;
    let mut shift = 0;
    loop {
        if shift > 28 {
            return Err("invalid compact RIPC size");
        }
        let byte = *data.get(offset).ok_or("truncated compact RIPC size")?;
        offset += 1;
        value |= u32::from(byte & 0x7f) << shift;
        if byte < 0x80 {
            break;
        }
        shift += 7;
    }
    if value & 1 != 0 {
        return Err("compact RIPC size is not doubled");
    }
    Ok(((value >> 1) as usize, offset))
}

pub fn pack_session_invite_accepted_event(
    request: RuntimeIpcHeader,
    link_flow_session_id: &str,
    session_type: u32,
) -> Result<Vec<u8>, &'static str> {
    if link_flow_session_id.len() != 36 || !link_flow_session_id.is_ascii() {
        return Err("captured link flow session id must be a UUID string");
    }
    let mut event_object = match session_type {
        1 => {
            let prefix = [
                0x01, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 9, b'T', b'C', b'P', b'_', b'D', b'I', b'S', b'C', b'O', 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 14, b'2', b'0', b'8',
                b'.', b'0', b'.', b'0', b'.', b'7', b'4', b'.', b'5', b'3', b'5', b'$',
            ];
            let mut invite = prefix.to_vec();
            invite.extend_from_slice(link_flow_session_id.as_bytes());
            let mut object = Vec::new();
            pack_compact_size(invite.len(), &mut object)?;
            object.extend_from_slice(&invite);
            object.extend_from_slice(&[
                0x16, 0x05, 0x02, 0, 4, 0, 3, 0, 5, 0, 9, 0, 2, 0,
            ]);
            let mut wrapped = Vec::new();
            pack_compact_size(object.len(), &mut wrapped)?;
            wrapped.extend_from_slice(&object);
            wrapped
        }
        3 => {
            let encoded = decode_hex(
                "c604dc0103000000000000000000000000004800000001000000010001000000\
                 095443505f444953434f02000100010000000100000100000000000000000e32\
                 30382e302e302e37342e35333524303030303030303030303030303030303030\
                 3030303030303030303030303030303030302208020004000300180008001300\
                 14001200be020d19000000000000d00700000000000100000000100000100000\
                 00020000000001000000000000030000000001000010000000040000003a0500\
                 0000000000050000001410000000000000060000000400000000000000070000\
                 00000010000000000008000000000100000000000009000000fc020000100000\
                 000a000000cf000000100000000b00000000000100000000000d000000180008\
                 0000000000",
            )?;
            let mut object = encoded;
            object[78..114].copy_from_slice(link_flow_session_id.as_bytes());
            object
        }
        _ => return Err("unsupported captured Air Link session type"),
    };

    let mut event_payload = Vec::with_capacity(10 + event_object.len());
    event_payload.extend_from_slice(&2u16.to_le_bytes());
    event_payload.extend_from_slice(&CLIENT_EVENT_SESSION_INVITE_ACCEPTED.to_le_bytes());
    event_payload.extend_from_slice(&REFLECT_INVITE_OK_MESSAGE_TYPE_HASH.to_le_bytes());
    event_payload.append(&mut event_object);
    // An event is not the answer to a call: Windows sends request and memory
    // ids of zero, not the ids of the invite it follows.
    let mut header = request.response_to(MESSAGE_TYPE_SESSION_EVENT, event_payload.len())?;
    header.request_id = 0;
    header.memory_id = 0;
    RuntimeIpcMessage::pack(header, &event_payload)
}

/// The streaming client sends a 4 kB record every 5 s (`commandGotDelivered`);
/// Windows answers with a bare success response of this type.
pub const CLIENT_EVENT_PERIODIC_REPORT: u32 = 0xbb61_cbe2;
pub const RPC_RESPONSE_TYPE_PERIODIC_REPORT: u32 = 0xc0f0_87f1;

const OAF_BROADCAST_HANDLER: u32 = 0x5ede_83cc;
const OAF_BROADCAST_TYPE: u32 = 0xd591_54e0;

fn encode_varint(mut value: u64, output: &mut Vec<u8>) {
    while value >= 0x80 {
        output.push((value as u8 & 0x7f) | 0x80);
        value >>= 7;
    }
    output.push(value as u8);
}

pub fn pack_oaf_broadcast_event(
    route: RuntimeIpcHeader,
    command: &str,
    payload_json: &str,
    timestamp_ms: u128,
) -> Result<Vec<u8>, &'static str> {
    let text = format!(
        "{{\n\t\"messageType\" : \"PUSH\",\n\t\"payload\" : \n\t{{\n{payload_json}\n\t}},\n\t\"payloadType\" : \"{command}\",\n\t\"sequenceId\" : \"null\",\n\t\"timestamp\" : \"{timestamp_ms}\"\n}}"
    );
    let name = command.as_bytes();
    let mut record = Vec::with_capacity(10 + name.len() + text.len());
    record.extend_from_slice(&0u32.to_le_bytes());
    record.extend_from_slice(&1u32.to_le_bytes());
    encode_varint(name.len() as u64, &mut record);
    record.extend_from_slice(name);
    encode_varint(text.len() as u64, &mut record);
    record.extend_from_slice(text.as_bytes());
    record.push(0);

    let mut event = Vec::with_capacity(10 + record.len() + 5);
    event.extend_from_slice(&2u16.to_le_bytes());
    event.extend_from_slice(&OAF_BROADCAST_HANDLER.to_le_bytes());
    event.extend_from_slice(&OAF_BROADCAST_TYPE.to_le_bytes());
    encode_varint((record.len() as u64) << 1, &mut event);
    event.extend_from_slice(&record);

    RuntimeIpcMessage::pack(
        RuntimeIpcHeader {
            message_type: MESSAGE_TYPE_SESSION_EVENT,
            message_size: u32::try_from(event.len()).map_err(|_| "OAF event too large")?,
            client_id: route.client_id,
            server_id: route.server_id,
            one_way: false,
            request_id: 0,
            memory_id: 0,
            message_direction: MESSAGE_DIRECTION_RESPONSE,
            rpc_failure_status: 0,
        },
        &event,
    )
}

fn decode_hex(data: &str) -> Result<Vec<u8>, &'static str> {
    let compact = data.bytes().filter(|byte| !byte.is_ascii_whitespace()).collect::<Vec<_>>();
    if compact.len() % 2 != 0 {
        return Err("invalid hexadecimal RIPC event template");
    }
    compact
        .chunks_exact(2)
        .map(|pair| {
            let high = (pair[0] as char)
                .to_digit(16)
                .ok_or("invalid hexadecimal RIPC event template")?;
            let low = (pair[1] as char)
                .to_digit(16)
                .ok_or("invalid hexadecimal RIPC event template")?;
            Ok(((high << 4) | low) as u8)
        })
        .collect()
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HandshakeMessage {
    pub reflect_version: u16,
    pub handshake_pass: u16,
    pub payload: Vec<u8>,
}

impl HandshakeMessage {
    pub fn pack(&self) -> Result<Vec<u8>, &'static str> {
        let payload_size = u64::try_from(self.payload.len())
            .map_err(|_| "RIPC handshake payload is too large")?;
        let mut data = Vec::with_capacity(16 + self.payload.len());
        data.extend_from_slice(&HANDSHAKE_HEADER_SIZE.to_le_bytes());
        data.extend_from_slice(&payload_size.to_le_bytes());
        data.extend_from_slice(&HANDSHAKE_TAG.to_le_bytes());
        data.extend_from_slice(&self.reflect_version.to_le_bytes());
        data.extend_from_slice(&self.handshake_pass.to_le_bytes());
        data.extend_from_slice(&self.payload);
        Ok(data)
    }

    pub fn unpack(data: &[u8]) -> Result<Self, &'static str> {
        if data.len() < 16 {
            return Err("RIPC handshake frame is truncated");
        }
        let header_size = u64::from_le_bytes(data[0..8].try_into().unwrap());
        let payload_size = u64::from_le_bytes(data[8..16].try_into().unwrap());
        if header_size != HANDSHAKE_HEADER_SIZE
            || payload_size + HANDSHAKE_HEADER_SIZE != (data.len() - 16) as u64
            || u32::from_le_bytes(data[16..20].try_into().unwrap()) != HANDSHAKE_TAG
        {
            return Err("invalid RIPC handshake frame");
        }
        Ok(Self {
            reflect_version: u16::from_le_bytes(data[20..22].try_into().unwrap()),
            handshake_pass: u16::from_le_bytes(data[22..24].try_into().unwrap()),
            payload: data[24..].to_vec(),
        })
    }
}

pub fn is_handshake(data: &[u8]) -> bool {
    data.len() >= 24
        && u64::from_le_bytes(data[0..8].try_into().unwrap()) == HANDSHAKE_HEADER_SIZE
        && u32::from_le_bytes(data[16..20].try_into().unwrap()) == HANDSHAKE_TAG
}

pub fn channel_buffer_handshake_ack(data: &[u8]) -> Option<&'static [u8; 28]> {
    (data == CHANNEL_BUFFER_HANDSHAKE).then_some(&CHANNEL_BUFFER_HANDSHAKE)
}

pub fn gpu_buffer_handshake_ack(data: &[u8]) -> Option<Vec<u8>> {
    if data.len() != GPU_BUFFER_HANDSHAKE_SIZE
        || u64::from_le_bytes(data[0..8].try_into().ok()?) != GPU_BUFFER_HANDSHAKE_HEADER_SIZE
        || u64::from_le_bytes(data[8..16].try_into().ok()?) != GPU_BUFFER_HANDSHAKE_PAYLOAD_SIZE
        || data[GPU_BUFFER_HANDSHAKE_ACK_OFFSET] != 0
    {
        return None;
    }
    let mut response = data.to_vec();
    response[GPU_BUFFER_HANDSHAKE_ACK_OFFSET] = 1;
    Some(response)
}

#[cfg(test)]
mod handshake_tests {
    use super::HandshakeMessage;

    #[test]
    fn unpack_accepts_what_pack_writes() {
        let message = HandshakeMessage {
            reflect_version: 2,
            handshake_pass: 1,
            payload: vec![0x34, 0x03, 0x01, 0x00],
        };
        let packed = message.pack().unwrap();
        // 16 bytes of sizes, 8 bytes of header, then the payload.
        assert_eq!(packed.len(), 16 + 8 + 4);
        let unpacked = HandshakeMessage::unpack(&packed).unwrap();
        assert_eq!(unpacked.reflect_version, 2);
        assert_eq!(unpacked.handshake_pass, 1);
        assert_eq!(unpacked.payload, message.payload);
    }
}

#[cfg(test)]
mod captured_layout_tests {
    use super::*;

    #[test]
    fn client_create_payload_is_complete() {
        let payload = decode_hex(RUNTIME_SERVICE_CLIENT_CREATE_PAYLOAD_HEX).unwrap();
        assert_eq!(payload.len(), 95);
        assert!(payload
            .windows(b"com.oculus.systemdriver".len())
            .any(|window| window == b"com.oculus.systemdriver"));
    }

    #[test]
    fn channel_handshake_matches_the_headset() {
        assert_eq!(
            CHANNEL_BUFFER_HANDSHAKE,
            [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 5, 0, 0, 0, 1, 1, 0, 0, 0]
        );
    }
}
