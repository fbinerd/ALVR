//! Air Link video packet envelope for encoded ALVR frames.

#[derive(Clone, Debug)]
pub struct AirLinkVideoFrame {
    pub is_idr: bool,
    pub nal: Vec<u8>,
    /// Position xyz followed by quaternion xyzw in the frame's tracking space.
    pub head_pose: [f32; 7],
}

fn encode_varint(mut value: u64, output: &mut Vec<u8>) {
    while value >= 0x80 {
        output.push((value as u8 & 0x7f) | 0x80);
        value >>= 7;
    }
    output.push(value as u8);
}

impl AirLinkVideoFrame {
    /// Build the observed Quest v209 packet header and 205-byte frame metadata.
    pub fn pack(&self, frame_id: u32, host_now_ns: u64, headset_offset_ns: i64) -> Vec<u8> {
        let headset_now_s = (host_now_ns as i128 + i128::from(headset_offset_ns)) as f64
            / 1_000_000_000.0;
        let timestamp_us = ((host_now_ns as i128 / 1_000)
            + 2 * i128::from(headset_offset_ns) / 1_000
            + 43_000)
            .max(0) as u64;

        let mut metadata = [0u8; 205];
        metadata[..6].copy_from_slice(&[0, 0, 0, 1, 3, 12]);
        for (offset, lead) in [
            (84, 0.054),
            (148, -0.004),
            (156, 0.040),
            (164, 0.033),
            (172, 0.011),
        ] {
            metadata[offset..offset + 8]
                .copy_from_slice(&(headset_now_s + lead).to_le_bytes());
        }
        metadata[52..80].copy_from_slice(bytemuck_f32x7(self.head_pose).as_slice());
        metadata[124..128].copy_from_slice(&frame_id.to_le_bytes());
        metadata[180..188]
            .copy_from_slice(&(headset_offset_ns as f64 / 1_000_000_000.0).to_le_bytes());

        let (flags, reference_id, layer) = if self.is_idr {
            (477, 0, 1)
        } else {
            let reference_id = if frame_id == 4 {
                4
            } else {
                frame_id.saturating_sub(1) * 2
            };
            (223, reference_id, 0)
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
            layer,
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
