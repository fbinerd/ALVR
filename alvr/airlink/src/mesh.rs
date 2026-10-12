//! Procedural Quest 2 rectification mesh for Air Link's MeshChannel.
//!
//! Layout checked against the 250479-byte packet Meta Horizon Link 208 sends
//! (MetaHorizonLinux `analysis/windows-session-capture.md`, section 15): a
//! 20-byte zero prefix, the object length as a varint shifted left by one,
//! the vertex count, 18-byte vertices (0x10, position x, y, 0x10, texture
//! u, v), nine bytes, the index count, u16 triangle indices, nine bytes, the
//! input and output resolutions, the mesh id and a five-byte trailer.
//! Windows' texture coordinates are S-shaped because its encoder squeezes
//! the periphery; ALVR's picture is rectilinear, so they are linear here.

const GRID_SIDE: u32 = 65;
const VERTICES_PER_EYE: u32 = GRID_SIDE * GRID_SIDE;
const MESH_ID: u32 = 1000;
const OUTPUT_WIDTH: u32 = 4160;
const OUTPUT_HEIGHT: u32 = 2096;

fn encode_varint(mut value: u64, output: &mut Vec<u8>) {
    while value >= 0x80 {
        output.push((value as u8 & 0x7f) | 0x80);
        value >>= 7;
    }
    output.push(value as u8);
}

fn encode_array<const N: usize>(value: [u8; N], output: &mut Vec<u8>) {
    output.push(N as u8);
    output.extend_from_slice(&value);
}

fn encode_vec2(x: f32, y: f32, output: &mut Vec<u8>) {
    output.push(0x10);
    output.extend_from_slice(&x.to_le_bytes());
    output.extend_from_slice(&y.to_le_bytes());
}

/// A 65x65 grid for each eye with linear UVs, matching ALVR's rectilinear video.
/// `input_resolution` is the side-by-side encoded video size.
pub fn pack(input_resolution: (u32, u32)) -> Vec<u8> {
    let vertex_count = VERTICES_PER_EYE * 2;
    let index_count = (GRID_SIDE - 1) * (GRID_SIDE - 1) * 6 * 2;
    let mut object = Vec::with_capacity(250_456);

    encode_varint(u64::from(vertex_count), &mut object);
    // The two-byte prefix is present in the captured MeshChannel vertex buffer.
    object.extend_from_slice(&[0x25, 0x00]);

    for eye in 0..2 {
        let eye_left = if eye == 0 { -1.0_f32 } else { 0.0 };
        for y in 0..GRID_SIDE {
            let v = y as f32 / (GRID_SIDE - 1) as f32;
            let position_y = 1.0 - 2.0 * v;
            for x in 0..GRID_SIDE {
                let eye_u = x as f32 / (GRID_SIDE - 1) as f32;
                let position_x = eye_left + eye_u;
                let u = (position_x + 1.0) * 0.5;
                encode_vec2(position_x, position_y, &mut object);
                encode_vec2(u, v, &mut object);
            }
        }
    }

    encode_array([0; 8], &mut object);
    encode_varint(u64::from(index_count), &mut object);
    for eye in 0..2 {
        let eye_base = eye * VERTICES_PER_EYE;
        for x in 0..GRID_SIDE - 1 {
            for y in 0..GRID_SIDE - 1 {
                let top_left = eye_base + y * GRID_SIDE + x;
                let top_right = top_left + 1;
                let bottom_left = top_left + GRID_SIDE;
                let bottom_right = bottom_left + 1;
                for index in [
                    top_left,
                    top_right,
                    bottom_left,
                    top_right,
                    bottom_right,
                    bottom_left,
                ] {
                    object.extend_from_slice(&(index as u16).to_le_bytes());
                }
            }
        }
    }

    encode_array([0; 8], &mut object);
    // Resolutions are pairs of u32 behind the same 8-byte marker as a vec2.
    for (width, height) in [input_resolution, (OUTPUT_WIDTH, OUTPUT_HEIGHT)] {
        object.push(0x10);
        object.extend_from_slice(&width.to_le_bytes());
        object.extend_from_slice(&height.to_le_bytes());
    }
    object.extend_from_slice(&MESH_ID.to_le_bytes());
    encode_array([0xf8, 0x7f, 0, 0], &mut object);

    let mut packet = vec![0; 20];
    encode_varint((object.len() as u64) << 1, &mut packet);
    packet.extend_from_slice(&object);
    packet
}

#[cfg(test)]
mod tests {
    use super::pack;

    #[test]
    fn matches_the_captured_layout() {
        let packet = pack((3680, 1920));
        assert_eq!(packet.len(), 250_479);
        assert_eq!(&packet[20..27], &[0xb0, 0xc9, 0x1e, 0x82, 0x42, 0x25, 0x00]);
        // First vertex: position (-1, 1), texture (0, 0).
        assert_eq!(packet[27], 0x10);
        assert_eq!(&packet[28..36], &[0, 0, 0x80, 0xbf, 0, 0, 0x80, 0x3f]);
        assert_eq!(packet[36], 0x10);
        assert_eq!(&packet[37..45], &[0; 8]);
        let indices = 27 + 18 * 8450;
        assert_eq!(
            &packet[indices..indices + 12],
            &[0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x80, 0x03]
        );
        let first: Vec<u16> = packet[indices + 12..indices + 36]
            .chunks(2)
            .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
            .collect();
        assert_eq!(first, [0, 1, 65, 1, 66, 65, 65, 66, 130, 66, 131, 130]);
        let tail: [u8; 36] = [
            0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0x60, 0x0e, 0, 0, 0x80, 0x07, 0, 0, 0x10, 0x40,
            0x10, 0, 0, 0x30, 0x08, 0, 0, 0xe8, 0x03, 0, 0, 0x04, 0xf8, 0x7f, 0, 0,
        ];
        assert_eq!(&packet[packet.len() - 36..], &tail);
    }
}
