//! Air Link's HapticChannel, PC to headset. Layout read from Meta Horizon
//! Link 208 captures (MetaHorizonLinux `analysis/windows-capture-20261012.md`,
//! sessions 12:43 and 14:09): the AudioChannel framing (a 20-byte zero
//! prefix, then the body length shifted left by one as a varint) around a
//! 65-byte body:
//!
//! | body offset | value |
//! |---|---|
//! | 0 | `42 01 00 00 00`, constant (`4a 02` marks a sample-buffer kind not used here) |
//! | 5 | f64, the PC's monotonic clock in seconds |
//! | 13 | u32 hand: 0 left, 1 right (2, both, once at stream start) |
//! | 17 | `0d 00 00 00 8a 62 49 3d 10`, constant |
//! | 26 | f32 frequency, always 1.0 from Meta's runtime |
//! | 30 | f32 amplitude, 0 to 1; 0 stops the vibration |
//! | 34 | `3c 19` and zeros, constant |
//!
//! The headset vibrates until told to stop: Meta's runtime sends a packet
//! with amplitude 0 when the vibration's duration is over.

use std::sync::Mutex;
use std::time::Duration;
use tokio::sync::mpsc;

const PREFIX_BYTES: usize = 20;
const BODY_BYTES: usize = 65;
const KIND: [u8; 5] = [0x42, 0x01, 0, 0, 0];
const MIDDLE: [u8; 9] = [0x0d, 0, 0, 0, 0x8a, 0x62, 0x49, 0x3d, 0x10];
const TAIL_MARK: [u8; 2] = [0x3c, 0x19];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AirLinkHand {
    Left = 0,
    Right = 1,
}

/// One vibration asked for by the game.
#[derive(Clone, Copy, Debug)]
pub struct AirLinkHaptic {
    pub hand: AirLinkHand,
    /// 0 to 1.
    pub amplitude: f32,
    pub duration: Duration,
}

pub fn pack_haptic_packet(hand: u32, amplitude: f32, pc_clock_s: f64) -> Vec<u8> {
    let mut packet = Vec::with_capacity(PREFIX_BYTES + 2 + BODY_BYTES);
    packet.resize(PREFIX_BYTES, 0);
    // (65 << 1) as a varint.
    packet.extend_from_slice(&[0x82, 0x01]);
    packet.extend_from_slice(&KIND);
    packet.extend_from_slice(&pc_clock_s.to_le_bytes());
    packet.extend_from_slice(&hand.to_le_bytes());
    packet.extend_from_slice(&MIDDLE);
    let frequency = if amplitude > 0.0 { 1.0f32 } else { 0.0 };
    packet.extend_from_slice(&frequency.to_le_bytes());
    packet.extend_from_slice(&amplitude.clamp(0.0, 1.0).to_le_bytes());
    packet.extend_from_slice(&TAIL_MARK);
    packet.resize(PREFIX_BYTES + 2 + BODY_BYTES, 0);
    packet
}

// The host of the current connection takes vibrations from here; with no
// headset streaming they are dropped.
static QUEUE: Mutex<Option<mpsc::UnboundedSender<AirLinkHaptic>>> = Mutex::new(None);

/// Hands a vibration to the Air Link headset, if one is streaming.
pub fn send_haptic(haptic: AirLinkHaptic) {
    if let Some(sender) = &*QUEUE.lock().unwrap() {
        sender.send(haptic).ok();
    }
}

/// Opens the queue for a new connection; the previous one's is closed.
pub(crate) fn open_queue() -> mpsc::UnboundedReceiver<AirLinkHaptic> {
    let (sender, receiver) = mpsc::unbounded_channel();
    *QUEUE.lock().unwrap() = Some(sender);
    receiver
}

pub(crate) fn close_queue() {
    *QUEUE.lock().unwrap() = None;
}

#[cfg(test)]
mod tests {
    use super::pack_haptic_packet;

    fn hex(text: &str) -> Vec<u8> {
        (0..text.len())
            .step_by(2)
            .map(|index| u8::from_str_radix(&text[index..index + 2], 16).unwrap())
            .collect()
    }

    #[test]
    fn matches_captured_packets() {
        // Left hand on (VRChat, capture of 2026-10-12 14:09), and an off packet.
        let time = f64::from_le_bytes([0x7d, 0x84, 0xde, 0x8e, 0x6c, 0x4e, 0xf2, 0x40]);
        let on = format!(
            "{}82014201000000{}000000000d0000008a62493d100000803f9a99193e3c19{}",
            "00".repeat(20),
            "7d84de8e6c4ef240",
            "00".repeat(29)
        );
        assert_eq!(pack_haptic_packet(0, 0.15, time), hex(&on));
        let off = format!(
            "{}82014201000000{}010000000d0000008a62493d1000000000000000003c19{}",
            "00".repeat(20),
            "7d84de8e6c4ef240",
            "00".repeat(29)
        );
        assert_eq!(pack_haptic_packet(1, 0.0, time), hex(&off));
        assert_eq!(pack_haptic_packet(1, 0.0, time).len(), 87);
    }
}
