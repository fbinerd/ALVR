//! XRSP packet framing used by the Meta Link session protocol.

pub const HEADER_SIZE: usize = 8;
pub const BUILTIN_TOPIC: u8 = 1;
pub const BUILTIN_PREFIX_SIZE: usize = 8;

const FLAG_BIT: u16 = 1 << 2;
const PADDING_BIT: u16 = 1 << 3;
const INTERNAL_BIT: u16 = 1 << 4;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum MessageType {
    PairingAck = 0,
    Invite = 1,
    Ok = 2,
    Ack = 3,
    Error = 4,
    Bye = 5,
    Echo = 6,
    Pairing = 7,
    CodeGeneration = 9,
    CodeGenerationAck = 10,
    ProcessRecovered = 11,
    Reserved = 15,
}

impl MessageType {
    pub fn from_id(id: u8) -> Option<Self> {
        Some(match id {
            0 => Self::PairingAck,
            1 => Self::Invite,
            2 => Self::Ok,
            3 => Self::Ack,
            4 => Self::Error,
            5 => Self::Bye,
            6 => Self::Echo,
            7 => Self::Pairing,
            9 => Self::CodeGeneration,
            10 => Self::CodeGenerationAck,
            11 => Self::ProcessRecovered,
            15 => Self::Reserved,
            _ => return None,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PacketHeader {
    pub topic: u8,
    pub total_size: usize,
    pub sequence: u16,
    pub version: u8,
    pub internal: bool,
    pub has_padding: bool,
    pub flag: bool,
}

impl PacketHeader {
    pub fn body_size(self) -> Result<usize, &'static str> {
        self.total_size
            .checked_sub(HEADER_SIZE)
            .ok_or("XRSP packet is shorter than its header")
    }

    pub fn pack(self) -> Result<[u8; HEADER_SIZE], &'static str> {
        if self.total_size < HEADER_SIZE || self.total_size % 4 != 0 {
            return Err("XRSP packet size must be aligned and contain a header");
        }
        let length_words = self.total_size / 4 - 1;
        if length_words > u16::MAX as usize {
            return Err("XRSP packet is too large");
        }
        let word0 = ((self.topic as u16 & 0x3f) << 8)
            | ((self.version as u16 & 0x7) << 5)
            | (if self.internal { INTERNAL_BIT } else { 0 })
            | (if self.has_padding { PADDING_BIT } else { 0 })
            | (if self.flag { FLAG_BIT } else { 0 });
        let mut bytes = [0; HEADER_SIZE];
        bytes[0..2].copy_from_slice(&word0.to_le_bytes());
        bytes[2..4].copy_from_slice(&(length_words as u16).to_le_bytes());
        bytes[4..6].copy_from_slice(&self.sequence.to_le_bytes());
        Ok(bytes)
    }

    pub fn unpack(data: &[u8]) -> Result<Self, &'static str> {
        if data.len() < HEADER_SIZE {
            return Err("XRSP header is truncated");
        }
        let word0 = u16::from_le_bytes([data[0], data[1]]);
        let length_words = u16::from_le_bytes([data[2], data[3]]);
        if length_words == 0 {
            return Err("XRSP packet has zero length word");
        }
        Ok(Self {
            topic: ((word0 >> 8) & 0x3f) as u8,
            total_size: (length_words as usize + 1) * 4,
            sequence: u16::from_le_bytes([data[4], data[5]]),
            version: ((word0 >> 5) & 0x7) as u8,
            internal: word0 & INTERNAL_BIT != 0,
            has_padding: word0 & PADDING_BIT != 0,
            flag: word0 & FLAG_BIT != 0,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BuiltinPrefix {
    pub message_type: u8,
    pub result: u16,
    pub raw: u32,
}

impl BuiltinPrefix {
    pub fn unpack(payload: &[u8]) -> Result<Self, &'static str> {
        if payload.len() < 4 {
            return Err("built-in message is shorter than 4 bytes");
        }
        let raw = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
        Ok(Self {
            message_type: (raw & 0xf) as u8,
            result: ((raw >> 4) & 0x1ff) as u16,
            raw,
        })
    }

    pub fn pack(message_type: u8, result: u16) -> [u8; BUILTIN_PREFIX_SIZE] {
        let word = (message_type as u32 & 0xf) | ((result as u32 & 0x1ff) << 4) | 0x4000;
        let mut bytes = [0; BUILTIN_PREFIX_SIZE];
        bytes[..4].copy_from_slice(&word.to_le_bytes());
        bytes
    }
}
