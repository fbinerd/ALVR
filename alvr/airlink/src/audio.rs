//! Air Link's AudioChannel, both ways. Plain PCM in the framing checked
//! against Meta Horizon Link 208 (MetaHorizonLinux
//! `analysis/windows-capture-20261012.md`): a 20-byte zero prefix, the length
//! of the rest as a varint shifted left by one, the byte length of the
//! samples as a varint, the samples, the same byte length as a u64 and the
//! sender's monotonic clock in nanoseconds as a u64.
//!
//! PC to headset: 480 stereo frames of f32 at 48 kHz, 10 ms, 3880 bytes.
//! Headset to PC: 480 mono samples of s16 at 48 kHz, 1000 bytes.

use std::sync::Arc;

pub const SAMPLE_RATE: u32 = 48_000;
/// Bytes of one PC to headset packet's samples: 480 frames, two f32 each.
pub const SPEAKER_CHUNK_BYTES: usize = 480 * 2 * 4;

const PREFIX_BYTES: usize = 20;
const TRAILER_BYTES: usize = 16;

/// What the host needs to carry sound: game audio comes in through the
/// receiver in chunks of `SPEAKER_CHUNK_BYTES` (f32 little endian, left and
/// right interleaved), the headset's microphone goes out through the
/// callback as s16 little endian mono.
pub struct AirLinkAudio {
    pub speaker_receiver: tokio::sync::mpsc::Receiver<Vec<u8>>,
    pub on_microphone: Arc<dyn Fn(&[u8]) + Send + Sync>,
}

fn encode_varint(mut value: u64, output: &mut Vec<u8>) {
    while value >= 0x80 {
        output.push((value as u8 & 0x7f) | 0x80);
        value >>= 7;
    }
    output.push(value as u8);
}

fn decode_varint(input: &[u8]) -> Option<(u64, usize)> {
    let mut value = 0u64;
    for (index, byte) in input.iter().enumerate().take(9) {
        value |= u64::from(byte & 0x7f) << (7 * index);
        if byte & 0x80 == 0 {
            return Some((value, index + 1));
        }
    }
    None
}

pub fn pack_speaker_packet(samples: &[u8], clock_ns: u64) -> Vec<u8> {
    let mut body = Vec::with_capacity(samples.len() + 4 + TRAILER_BYTES);
    encode_varint(samples.len() as u64, &mut body);
    body.extend_from_slice(samples);
    body.extend_from_slice(&(samples.len() as u64).to_le_bytes());
    body.extend_from_slice(&clock_ns.to_le_bytes());

    let mut packet = vec![0; PREFIX_BYTES];
    encode_varint((body.len() as u64) << 1, &mut packet);
    packet.extend_from_slice(&body);
    packet
}

/// The samples of a headset to PC audio packet.
pub fn unpack_microphone_packet(packet: &[u8]) -> Option<&[u8]> {
    let rest = packet.get(PREFIX_BYTES..)?;
    let (body_length, used) = decode_varint(rest)?;
    let body = rest.get(used..)?;
    if body.len() as u64 != body_length >> 1 {
        return None;
    }
    let (sample_bytes, used) = decode_varint(body)?;
    let samples = body.get(used..used + usize::try_from(sample_bytes).ok()?)?;
    (body.len() == used + samples.len() + TRAILER_BYTES).then_some(samples)
}

#[cfg(test)]
mod tests {
    use super::{SPEAKER_CHUNK_BYTES, pack_speaker_packet, unpack_microphone_packet};

    #[test]
    fn speaker_packet_matches_the_captured_layout() {
        let packet = pack_speaker_packet(&[0; SPEAKER_CHUNK_BYTES], 5_883_493_149_800);
        assert_eq!(packet.len(), 3880);
        assert_eq!(&packet[..20], &[0; 20]);
        assert_eq!(&packet[20..24], &[0xa4, 0x3c, 0x80, 0x1e]);
        assert_eq!(
            &packet[3864..],
            &[
                0, 0x0f, 0, 0, 0, 0, 0, 0, 0x68, 0xec, 0x84, 0xdb, 0x59, 0x05, 0, 0
            ]
        );
    }

    #[test]
    fn microphone_packet_gives_its_samples() {
        let mut packet = vec![0; 20];
        packet.extend_from_slice(&[0xa4, 0x0f, 0xc0, 0x07]);
        packet.extend((0..960).map(|index| index as u8));
        packet.extend_from_slice(&960u64.to_le_bytes());
        packet.extend_from_slice(&70_945_691_087_290u64.to_le_bytes());
        assert_eq!(packet.len(), 1000);
        let samples = unpack_microphone_packet(&packet).unwrap();
        assert_eq!(samples.len(), 960);
        assert_eq!(samples[5], 5);
        assert!(unpack_microphone_packet(&packet[..999]).is_none());
    }
}
