//! Meta DISCO/commlib framing and basic control-message encoders.

use base64::{Engine, engine::general_purpose::STANDARD};
use ring::digest::{SHA256, digest};

/// The normal Meta pairing path writes each X.509 certificate to a memory
/// BIO as PEM, hashes that PEM followed by its certificate-exchange random,
/// then XORs the four little-endian words of both hashes. The order of the
/// two peers is immaterial. Keep the PEM encoding identical to OpenSSL's
/// PEM_write_bio_X509, including the final newline.
fn certificate_pem(der: &[u8]) -> Vec<u8> {
    let encoded = STANDARD.encode(der);
    let mut pem = b"-----BEGIN CERTIFICATE-----\n".to_vec();
    for line in encoded.as_bytes().chunks(64) {
        pem.extend_from_slice(line);
        pem.push(b'\n');
    }
    pem.extend_from_slice(b"-----END CERTIFICATE-----\n");
    pem
}

pub fn pairing_code(
    headset_cert: &[u8],
    headset_random: &[u8; 32],
    host_cert: &[u8],
    host_random: &[u8; 32],
) -> u64 {
    fn hash_pair(certificate: &[u8], random: &[u8; 32]) -> ring::digest::Digest {
        let mut input = certificate_pem(certificate);
        input.extend_from_slice(random);
        digest(&SHA256, &input)
    }

    let headset = hash_pair(headset_cert, headset_random);
    let host = hash_pair(host_cert, host_random);
    headset
        .as_ref()
        .chunks_exact(8)
        .zip(host.as_ref().chunks_exact(8))
        .fold(0, |code, (headset_word, host_word)| {
            code ^ u64::from_le_bytes(headset_word.try_into().unwrap())
                ^ u64::from_le_bytes(host_word.try_into().unwrap())
        })
}

#[cfg(test)]
mod pairing_code_tests {
    use super::pairing_code;

    #[test]
    fn matches_independent_sha256_vector_and_is_symmetric() {
        let headset_cert: Vec<_> = (1u8..=32).collect();
        let headset_random: [u8; 32] = std::array::from_fn(|index| index as u8);
        let host_cert: Vec<_> = (41u8..=72).collect();
        let host_random: [u8; 32] = std::array::from_fn(|index| index as u8 + 32);
        let expected = 0x2fe9_d71a_8430_c914;
        assert_eq!(
            pairing_code(&headset_cert, &headset_random, &host_cert, &host_random),
            expected
        );
        assert_eq!(
            pairing_code(&host_cert, &host_random, &headset_cert, &headset_random),
            expected
        );
        assert_eq!(expected % 1_000_000, 107_092);
        assert_eq!(0x3ccc_846a_ba27_f7ec_u64 % 1_000_000, 231_340);
    }
}

pub const CONTROL_TOPIC: u32 = 0xcd;
pub const PAIRING_TOPIC: u32 = 0xce;

pub const MSG_CONNECT_REQ: u32 = 0x10;
pub const MSG_CONNECT_ACK: u32 = 0x11;
pub const MSG_CONNECT_NACK: u32 = 0x12;
pub const MSG_DISCONNECT_REQ: u32 = 0x20;
pub const MSG_DISCONNECT_ACK: u32 = 0x21;
pub const MSG_CLOCK_SYNC_INIT: u32 = 0x30;
pub const MSG_CLOCK_SYNC_REPLY: u32 = 0x31;
pub const MSG_CLOCK_SYNC_FINISH: u32 = 0x32;
pub const MSG_TOPIC_CREATE_REQ: u32 = 0x50;
pub const MSG_TOPIC_CREATE_ACK: u32 = 0x51;
pub const MSG_TOPIC_CREATE_NACK: u32 = 0x52;

pub const MSG_PAIRING_REQ: u32 = 0x10;
pub const MSG_PAIRING_ACK: u32 = 0x11;
pub const MSG_COMMITMENT: u32 = 0x18;
pub const MSG_COMMITMENT_REPLY: u32 = 0x19;
pub const MSG_EXCHANGING_CERT_REQ: u32 = 0x20;
pub const MSG_EXCHANGING_CERT_ACK: u32 = 0x21;
pub const MSG_EXCHANGING_CERT_NACK: u32 = 0x22;
pub const MSG_AWAITING_USER_REQ: u32 = 0x30;
pub const MSG_AWAITING_USER_ACK: u32 = 0x31;
pub const MSG_AWAITING_USER_NACK: u32 = 0x32;
pub const MSG_ABORT_PAIRING: u32 = 0x40;

pub const ENDPOINT_TYPE: &str = "HighwindService";
pub const ENDPOINT_VERSION: (u32, u32, u32) = (1, 0, 0);
pub const PROTOCOL_VERSION_RANGE: (u32, u32) = (9, 9);

pub const CONNECT_REQ_SIZE: usize = 0xd0;
pub const CONNECT_ACK_SIZE: usize = 0xb0;
const NAME_SIZE: usize = 64;
const RANDOM_SIZE: usize = 32;

pub fn pack_frame(topic: u32, payload: &[u8]) -> Result<Vec<u8>, &'static str> {
    let length = u32::try_from(payload.len()).map_err(|_| "DISCO payload is too large")?;
    let mut frame = Vec::with_capacity(8 + payload.len());
    frame.extend_from_slice(&length.to_be_bytes());
    frame.extend_from_slice(&topic.to_be_bytes());
    frame.extend_from_slice(payload);
    Ok(frame)
}

pub fn unpack_frame_header(data: &[u8]) -> Result<(u32, u32), &'static str> {
    if data.len() != 8 {
        return Err("DISCO frame header must be exactly 8 bytes");
    }
    Ok((
        u32::from_be_bytes(data[..4].try_into().unwrap()),
        u32::from_be_bytes(data[4..8].try_into().unwrap()),
    ))
}

fn read_u32_le(data: &[u8], offset: usize) -> Result<u32, &'static str> {
    let bytes = data
        .get(offset..offset + 4)
        .ok_or("DISCO message is truncated")?;
    Ok(u32::from_le_bytes(bytes.try_into().unwrap()))
}

fn read_u64_le(data: &[u8], offset: usize) -> Result<u64, &'static str> {
    let bytes = data
        .get(offset..offset + 8)
        .ok_or("DISCO message is truncated")?;
    Ok(u64::from_le_bytes(bytes.try_into().unwrap()))
}

fn name_field(value: &str) -> Result<[u8; NAME_SIZE], &'static str> {
    let bytes = value.as_bytes();
    if bytes.len() > NAME_SIZE {
        return Err("DISCO name exceeds 64 bytes");
    }
    let mut field = [0; NAME_SIZE];
    field[..bytes.len()].copy_from_slice(bytes);
    Ok(field)
}

fn text_field(field: &[u8]) -> String {
    let end = field.iter().position(|byte| *byte == 0).unwrap_or(field.len());
    String::from_utf8_lossy(&field[..end]).into_owned()
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConnectRequest {
    pub group: u32,
    pub endpoint_uuid: [u8; 16],
    pub second_uuid: [u8; 16],
    pub channel_uuid: [u8; 16],
    pub endpoint_name: String,
    pub endpoint_type: String,
    pub endpoint_version: (u32, u32, u32),
    pub protocol_min: u32,
    pub protocol_max: u32,
}

impl ConnectRequest {
    pub fn unpack(payload: &[u8]) -> Result<Self, &'static str> {
        if payload.len() != CONNECT_REQ_SIZE {
            return Err("DISCO ConnectReq has an invalid size");
        }
        if read_u32_le(payload, 4)? != MSG_CONNECT_REQ {
            return Err("DISCO message is not ConnectReq");
        }
        let copy_uuid = |offset: usize| -> [u8; 16] {
            payload[offset..offset + 16].try_into().unwrap()
        };
        Ok(Self {
            group: read_u32_le(payload, 0)?,
            endpoint_uuid: copy_uuid(8),
            second_uuid: copy_uuid(0x18),
            channel_uuid: copy_uuid(0x28),
            endpoint_name: text_field(&payload[0x38..0x78]),
            endpoint_type: text_field(&payload[0x78..0xb8]),
            endpoint_version: (
                read_u32_le(payload, 0xb8)?,
                read_u32_le(payload, 0xbc)?,
                read_u32_le(payload, 0xc0)?,
            ),
            protocol_min: read_u32_le(payload, 0xc8)?,
            protocol_max: read_u32_le(payload, 0xcc)?,
        })
    }
}

pub fn negotiate_protocol(request: &ConnectRequest, supported: (u32, u32)) -> Option<u32> {
    let low = request.protocol_min.max(supported.0);
    let high = request.protocol_max.min(supported.1);
    (low <= high).then_some(high)
}

pub fn pack_connect_ack(
    protocol_version: u32,
    endpoint_uuid: [u8; 16],
    endpoint_name: &str,
    endpoint_type: &str,
    endpoint_version: (u32, u32, u32),
) -> Result<[u8; CONNECT_ACK_SIZE], &'static str> {
    let name = name_field(endpoint_name)?;
    let kind = name_field(endpoint_type)?;
    let mut payload = [0; CONNECT_ACK_SIZE];
    payload[0..4].copy_from_slice(&2u32.to_le_bytes());
    payload[4..8].copy_from_slice(&MSG_CONNECT_ACK.to_le_bytes());
    payload[8..12].copy_from_slice(&protocol_version.to_le_bytes());
    payload[0x10..0x20].copy_from_slice(&endpoint_uuid);
    payload[0x20..0x60].copy_from_slice(&name);
    payload[0x60..0xa0].copy_from_slice(&kind);
    payload[0xa0..0xa4].copy_from_slice(&endpoint_version.0.to_le_bytes());
    payload[0xa4..0xa8].copy_from_slice(&endpoint_version.1.to_le_bytes());
    payload[0xa8..0xac].copy_from_slice(&endpoint_version.2.to_le_bytes());
    Ok(payload)
}

pub fn message_header(payload: &[u8]) -> Result<(u32, u32), &'static str> {
    Ok((read_u32_le(payload, 0)?, read_u32_le(payload, 4)?))
}

pub fn pack_clock_sync_reply(
    client_time: u64,
    receive_time: u64,
    send_time: u64,
) -> [u8; 32] {
    let mut payload = [0; 32];
    payload[0..4].copy_from_slice(&1u32.to_le_bytes());
    payload[4..8].copy_from_slice(&MSG_CLOCK_SYNC_REPLY.to_le_bytes());
    payload[8..16].copy_from_slice(&client_time.to_le_bytes());
    payload[16..24].copy_from_slice(&receive_time.to_le_bytes());
    payload[24..32].copy_from_slice(&send_time.to_le_bytes());
    payload
}

pub fn unpack_clock_sync_init(payload: &[u8]) -> Result<u64, &'static str> {
    read_u64_le(payload, 8)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PairingRequest {
    pub version: u32,
    pub auto_confirm: (u32, u32),
    pub pairing_attempt: Option<[u8; 16]>,
}

impl PairingRequest {
    pub fn unpack(payload: &[u8]) -> Result<Self, &'static str> {
        let (version, message) = message_header(payload)?;
        if message != MSG_PAIRING_REQ {
            return Err("DISCO message is not PairingReq");
        }
        let auto_confirm = if payload.len() >= 16 {
            (read_u32_le(payload, 8)?, read_u32_le(payload, 12)?)
        } else {
            (0, 0)
        };
        let pairing_attempt = payload
            .get(0x20..0x30)
            .map(|bytes| bytes.try_into().unwrap());
        Ok(Self {
            version,
            auto_confirm,
            pairing_attempt,
        })
    }
}

pub fn pack_pairing_ack() -> [u8; 8] {
    let mut payload = [0; 8];
    payload[..4].copy_from_slice(&1u32.to_le_bytes());
    payload[4..].copy_from_slice(&MSG_PAIRING_ACK.to_le_bytes());
    payload
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CertExchange {
    pub metadata: Vec<u8>,
    pub signature: Vec<u8>,
    pub random: [u8; RANDOM_SIZE],
}

impl CertExchange {
    pub fn unpack(payload: &[u8]) -> Result<Self, &'static str> {
        if payload.len() < 16 {
            return Err("certificate exchange message is shorter than 16 bytes");
        }
        let metadata_size = read_u32_le(payload, 8)? as usize;
        let signature_size = read_u32_le(payload, 12)? as usize;
        let random_offset = 16usize
            .checked_add(metadata_size)
            .and_then(|offset| offset.checked_add(signature_size))
            .ok_or("certificate exchange length overflow")?;
        let random = payload
            .get(random_offset..random_offset + RANDOM_SIZE)
            .ok_or("certificate exchange message is truncated")?
            .try_into()
            .unwrap();
        Ok(Self {
            metadata: payload[16..16 + metadata_size].to_vec(),
            signature: payload[16 + metadata_size..random_offset].to_vec(),
            random,
        })
    }

    pub fn pack(&self, message: u32) -> Result<Vec<u8>, &'static str> {
        let metadata_size = u32::try_from(self.metadata.len())
            .map_err(|_| "certificate metadata is too large")?;
        let signature_size = u32::try_from(self.signature.len())
            .map_err(|_| "certificate signature is too large")?;
        let mut payload = Vec::with_capacity(
            16 + self.metadata.len() + self.signature.len() + RANDOM_SIZE,
        );
        payload.extend_from_slice(&1u32.to_le_bytes());
        payload.extend_from_slice(&message.to_le_bytes());
        payload.extend_from_slice(&metadata_size.to_le_bytes());
        payload.extend_from_slice(&signature_size.to_le_bytes());
        payload.extend_from_slice(&self.metadata);
        payload.extend_from_slice(&self.signature);
        payload.extend_from_slice(&self.random);
        Ok(payload)
    }
}

pub fn pack_awaiting_user_ack() -> [u8; 8] {
    let mut payload = [0; 8];
    payload[..4].copy_from_slice(&1u32.to_le_bytes());
    payload[4..].copy_from_slice(&MSG_AWAITING_USER_ACK.to_le_bytes());
    payload
}

/// The user refused the pairing on the PC.
pub fn pack_awaiting_user_nack() -> [u8; 16] {
    // The Windows host sends its default refusal result (14) after a local
    // refusal. A header-only Nack is malformed to the Quest's DISCO parser.
    let mut payload = [0; 16];
    payload[..4].copy_from_slice(&1u32.to_le_bytes());
    payload[4..8].copy_from_slice(&MSG_AWAITING_USER_NACK.to_le_bytes());
    payload[8..16].copy_from_slice(&14u64.to_le_bytes());
    payload
}

#[cfg(test)]
mod pairing_refusal_tests {
    use super::pack_awaiting_user_nack;

    #[test]
    fn sends_user_rejected_reason_in_full_nack() {
        assert_eq!(
            pack_awaiting_user_nack(),
            [1, 0, 0, 0, 0x32, 0, 0, 0, 14, 0, 0, 0, 0, 0, 0, 0]
        );
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TopicCreateRequest {
    pub token: [u8; 16],
    pub descriptor: [u8; 16],
    pub kind: u32,
    pub commlib_topic: u32,
    pub max_fragment_size: u32,
}

impl TopicCreateRequest {
    pub fn unpack(payload: &[u8]) -> Result<Self, &'static str> {
        if payload.len() < 0x38 {
            return Err("topic create request is shorter than 56 bytes");
        }
        Ok(Self {
            token: payload[8..0x18].try_into().unwrap(),
            descriptor: payload[0x18..0x28].try_into().unwrap(),
            kind: read_u32_le(payload, 0x28)?,
            commlib_topic: read_u32_le(payload, 0x2c)?,
            max_fragment_size: read_u32_le(payload, 0x30)?,
        })
    }

    pub fn pack_ack(&self, max_fragment_size: u32) -> Result<[u8; 0x30], &'static str> {
        if (1..7).contains(&max_fragment_size) {
            return Err("max fragment size must be 0 or at least 7");
        }
        let mut payload = [0; 0x30];
        payload[..4].copy_from_slice(&2u32.to_le_bytes());
        payload[4..8].copy_from_slice(&MSG_TOPIC_CREATE_ACK.to_le_bytes());
        payload[8..0x18].copy_from_slice(&self.token);
        payload[0x18..0x28].copy_from_slice(&self.descriptor);
        payload[0x28..0x2c].copy_from_slice(&max_fragment_size.to_le_bytes());
        Ok(payload)
    }
}

pub fn pack_disconnect_ack() -> [u8; 8] {
    let mut payload = [0; 8];
    payload[..4].copy_from_slice(&1u32.to_le_bytes());
    payload[4..].copy_from_slice(&MSG_DISCONNECT_ACK.to_le_bytes());
    payload
}
