//! Air Link video packet envelope for encoded ALVR frames.

use std::sync::atomic::{AtomicU32, Ordering};

// Size of the encoded picture, both eyes side by side. Meta Horizon Link
// negotiates it per session (3680x1920 by default for a Quest 2, 2880x1536
// seen with a narrower encode width) and names it in three places: the GPU
// stream configuration event, the GPU buffer channel response and the rectify
// mesh. They have to agree with what the encoder produces.
static STREAM_WIDTH: AtomicU32 = AtomicU32::new(3680);
static STREAM_HEIGHT: AtomicU32 = AtomicU32::new(1920);

pub fn set_stream_size(width: u32, height: u32) {
    STREAM_WIDTH.store(width, Ordering::Relaxed);
    STREAM_HEIGHT.store(height, Ordering::Relaxed);
}

/// A Quest 2 set to 72 Hz refreshes every 13.9223 ms, 71.827 times a second:
/// the step of the display times in Meta Horizon Link's video frames, the
/// same in two captures of 2026-10-12. Assumed to scale to the other rates.
pub const REFRESH_RATE_FACTOR: f64 = 71.8272 / 72.0;

static FRAME_PERIOD_NS: std::sync::atomic::AtomicU64 =
    std::sync::atomic::AtomicU64::new(13_922_300);

/// Sets the headset's real frame period from its nominal refresh rate.
pub fn set_refresh_rate(nominal_hz: f32) {
    let period_ns = 1e9 / (f64::from(nominal_hz.max(1.0)) * REFRESH_RATE_FACTOR);
    FRAME_PERIOD_NS.store(period_ns as u64, Ordering::Relaxed);
}

/// The headset's real frame period, in seconds.
pub fn frame_period_s() -> f64 {
    FRAME_PERIOD_NS.load(Ordering::Relaxed) as f64 / 1e9
}

static SHARPENING_LEVEL: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(2);
static HEAD_PREDICTION_CAP_MS: std::sync::atomic::AtomicU64 =
    std::sync::atomic::AtomicU64::new(100);

/// The headset's sharpening for the next stream: 1 off, 2 normal, 3 quality.
pub fn set_sharpening_level(level: u8) {
    SHARPENING_LEVEL.store(level.clamp(1, 3), Ordering::Relaxed);
}

pub fn sharpening_level() -> u8 {
    SHARPENING_LEVEL.load(Ordering::Relaxed)
}

/// Upper limit of the head pose prediction, in milliseconds.
pub fn set_head_prediction_cap_ms(cap_ms: u64) {
    HEAD_PREDICTION_CAP_MS.store(cap_ms, Ordering::Relaxed);
}

pub fn head_prediction_cap_ms() -> u64 {
    HEAD_PREDICTION_CAP_MS.load(Ordering::Relaxed)
}

pub fn stream_size() -> (u32, u32) {
    (STREAM_WIDTH.load(Ordering::Relaxed), STREAM_HEIGHT.load(Ordering::Relaxed))
}

#[derive(Clone, Debug)]
pub struct AirLinkVideoFrame {
    pub is_idr: bool,
    pub nal: Vec<u8>,
    /// Orientation quaternion x, y, z, w followed by position x, y, z, in the
    /// headset's tracking space: the order of the frame metadata at offset 52.
    pub head_pose: [f32; 7],
}

// Frame metadata as sent by Meta Horizon Link 208 to a Quest 2 (v209), with
// the per-frame values zeroed: pose at 52, display time at 84, frame index at
// 124, four times at 148 and the clock offset at 180. What remains is constant
// in the capture: a 46-byte prefix, and for frames after the keyframe the
// field-of-view tangents at 92 (up, down, left, right per eye) and the rectify
// mesh id 1000 at 140.
const KEYFRAME_METADATA: [u8; 205] = [
    0x00, 0x00, 0x00, 0x01, 0x03, 0x0c, 0xbc, 0x6e, 0x84, 0x1d, 0xec, 0x7e,
    0x82, 0x02, 0x25, 0x98, 0x01, 0x02, 0x04, 0x04, 0x0a, 0x04, 0x11, 0x05,
    0x9c, 0xe7, 0xbd, 0x10, 0x28, 0x11, 0xb5, 0xb5, 0x02, 0x00, 0x01, 0x10,
    0x11, 0xb1, 0xb5, 0x02, 0x00, 0x01, 0x0c, 0x04, 0x02, 0x11, 0xb5, 0xb5,
    0x02, 0x00, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf8, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xba, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
];

const FRAME_METADATA: [u8; 205] = [
    0x00, 0x00, 0x00, 0x01, 0x03, 0x0c, 0xbc, 0x6e, 0x84, 0x1d, 0xec, 0x7e,
    0x82, 0x02, 0x25, 0x98, 0x01, 0x02, 0x04, 0x04, 0x0a, 0x04, 0x11, 0x05,
    0x9c, 0xe7, 0xbd, 0x10, 0x28, 0x11, 0xb5, 0xb5, 0x02, 0x00, 0x01, 0x10,
    0x11, 0xb1, 0xb5, 0x02, 0x00, 0x01, 0x0c, 0x04, 0x02, 0x11, 0xb5, 0xb5,
    0x02, 0x00, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8d, 0x28, 0x8e, 0x3f,
    0x62, 0x8b, 0x98, 0x3f, 0x20, 0xd5, 0xa3, 0x3f, 0x00, 0x00, 0x80, 0x3f,
    0x8d, 0x28, 0x8e, 0x3f, 0x62, 0x8b, 0x98, 0x3f, 0x00, 0x00, 0x80, 0x3f,
    0x20, 0xd5, 0xa3, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00,
    0xf8, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xba, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
];

fn encode_varint(mut value: u64, output: &mut Vec<u8>) {
    while value >= 0x80 {
        output.push((value as u8 & 0x7f) | 0x80);
        value >>= 7;
    }
    output.push(value as u8);
}

/// Annex-B units as the headset's decoder takes them: four-byte start codes,
/// no access unit delimiters (type 35) and no SEI (types 39 and 40).
pub fn without_delimiters_and_sei(data: &[u8]) -> Vec<u8> {
    let mut starts = Vec::new();
    let mut i = 0;
    while i + 3 < data.len() {
        if data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 {
            starts.push(i);
            i += 3;
        } else {
            i += 1;
        }
    }
    let mut out = Vec::with_capacity(data.len() + 4);
    for (index, &start) in starts.iter().enumerate() {
        let mut end = starts.get(index + 1).copied().unwrap_or(data.len());
        // A four-byte start code belongs to the unit that follows it.
        if index + 1 < starts.len() && end > 0 && data[end - 1] == 0 {
            end -= 1;
        }
        let nal_type = (data[start + 3] >> 1) & 0x3f;
        if !matches!(nal_type, 35 | 39 | 40) {
            out.push(0);
            out.extend_from_slice(&data[start..end]);
        }
    }
    out
}

impl AirLinkVideoFrame {
    /// Build the observed Quest v209 packet header and 205-byte frame metadata.
    ///
    /// `display_base_s` is this frame's place on the headset's refresh grid:
    /// Horizon's display times advance by exactly one frame period per
    /// frame, while its send times jitter by milliseconds.
    pub fn pack(
        &self,
        frame_id: u32,
        host_now_ns: u64,
        headset_offset_ns: i64,
        display_base_s: f64,
    ) -> Vec<u8> {
        let headset_now_s = (host_now_ns as i128 + i128::from(headset_offset_ns)) as f64
            / 1_000_000_000.0;
        // Horizon's header timestamps are as regular as its display times:
        // 13922.5 us a frame with a few microseconds of spread. Taken from
        // the frame's slot on the grid and not from the send time, which
        // jitters by milliseconds. (Headset clock plus the clock offset plus
        // a lead, see MetaHorizonLinux `analysis/windows-session-capture.md`
        // section 6.)
        let timestamp_us = ((display_base_s * 1e6) as i128
            + i128::from(headset_offset_ns) / 1_000
            + 43_000)
            .max(0) as u64;

        let mut metadata = if self.is_idr {
            KEYFRAME_METADATA
        } else {
            FRAME_METADATA
        };
        for (offset, base, lead) in [
            (84, display_base_s, 0.054),
            (148, headset_now_s, -0.004),
            (156, display_base_s, 0.040),
            (164, display_base_s, 0.033),
            (172, headset_now_s, 0.011),
        ] {
            metadata[offset..offset + 8].copy_from_slice(&(base + lead).to_le_bytes());
        }
        metadata[52..80].copy_from_slice(bytemuck_f32x7(self.head_pose).as_slice());
        metadata[124..128].copy_from_slice(&frame_id.to_le_bytes());
        metadata[180..188]
            .copy_from_slice(&(headset_offset_ns as f64 / 1_000_000_000.0).to_le_bytes());

        let (flags, reference_id) = if self.is_idr {
            (477, 0)
        } else {
            let reference_id = if frame_id == 4 {
                4
            } else {
                frame_id.saturating_sub(1) * 2
            };
            (223, reference_id)
        };
        let fields = [
            flags as u64,
            frame_id as u64,
            0,
            timestamp_us,
            0,
            205,
            reference_id as u64,
            if self.is_idr { 60 } else { 0 },
            if self.is_idr { 1 } else { 60 },
            1,
            0,
        ];
        let mut header = Vec::with_capacity(24);
        for field in fields {
            encode_varint(field, &mut header);
        }

        let mut packet = Vec::with_capacity(1 + header.len() + metadata.len() + self.nal.len());
        packet.push(header.len() as u8);
        packet.extend_from_slice(&header);
        packet.extend_from_slice(&metadata);
        packet.extend_from_slice(&self.nal);
        packet
    }
}

fn bytemuck_f32x7(values: [f32; 7]) -> [u8; 28] {
    let mut bytes = [0u8; 28];
    for (index, value) in values.into_iter().enumerate() {
        bytes[index * 4..index * 4 + 4].copy_from_slice(&value.to_le_bytes());
    }
    bytes
}

/// The numbers of the report the headset sends back for every video frame:
/// a header like the frame's own (flags, timestamp, frame id, body length)
/// and a body of a u32 and a few varints. Returned are the frame id and the
/// body's varints. **Not decoded**: in a Horizon capture they look like
/// times in microseconds (71, 5756, 33, 33, 5788, 7326 for one frame).
pub fn unpack_frame_report(packet: &[u8]) -> Option<(u64, Vec<u64>)> {
    fn varints(mut data: &[u8]) -> Vec<u64> {
        let mut values = Vec::new();
        while !data.is_empty() {
            let mut value = 0u64;
            let mut used = 0;
            for (index, byte) in data.iter().enumerate().take(10) {
                value |= u64::from(byte & 0x7f) << (7 * index);
                used = index + 1;
                if byte & 0x80 == 0 {
                    break;
                }
            }
            values.push(value);
            data = &data[used..];
        }
        values
    }

    let header_length = usize::from(*packet.first()?);
    let header = varints(packet.get(1..1 + header_length)?);
    let body = packet.get(1 + header_length..)?;
    Some((*header.get(2)?, varints(body.get(4..)?)))
}
