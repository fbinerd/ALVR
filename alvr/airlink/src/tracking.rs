//! Quest v209 Air Link controller tracking and input packet decoding.

#[derive(Clone, Copy, Debug)]
pub struct AirLinkPose {
    pub orientation_xyzw: [f32; 4],
    pub position_xyz: [f32; 3],
}

#[derive(Clone, Copy, Debug)]
pub struct AirLinkTrackingSample {
    pub head: AirLinkPose,
    pub left_hand: AirLinkPose,
    pub right_hand: AirLinkPose,
}

#[derive(Clone, Copy, Debug)]
pub struct AirLinkHandInput {
    pub buttons: u32,
    pub touches: u32,
    pub trigger: f32,
    pub grip: f32,
    pub thumbstick_xy: [f32; 2],
}

#[derive(Clone, Copy, Debug)]
pub struct AirLinkControllerInput {
    pub left: AirLinkHandInput,
    pub right: AirLinkHandInput,
    /// Battery charge in percent: left controller, right controller, headset.
    /// One byte each at 36, 97 and 223; in the captures they only fall, each
    /// on its own, and the headset's matches its own log.
    pub battery_percent: [u8; 3],
}

fn read_f32(data: &[u8], offset: usize) -> Option<f32> {
    Some(f32::from_le_bytes(data.get(offset..offset + 4)?.try_into().ok()?))
}

fn read_pose(data: &[u8], offset: usize) -> Option<AirLinkPose> {
    let orientation_xyzw = [
        read_f32(data, offset)?,
        read_f32(data, offset + 4)?,
        read_f32(data, offset + 8)?,
        read_f32(data, offset + 12)?,
    ];
    let position_xyz = [
        read_f32(data, offset + 17)?,
        read_f32(data, offset + 21)?,
        read_f32(data, offset + 25)?,
    ];
    let norm = orientation_xyzw.iter().map(|v| v * v).sum::<f32>();
    (norm.is_finite()
        && norm > 0.5
        && orientation_xyzw.iter().all(|v| v.is_finite())
        && position_xyz.iter().all(|v| v.is_finite()))
    .then_some(AirLinkPose {
        orientation_xyzw,
        position_xyz,
    })
}

pub fn unpack_tracking(data: &[u8]) -> Option<AirLinkTrackingSample> {
    if data.len() != 582 {
        return None;
    }
    Some(AirLinkTrackingSample {
        left_hand: read_pose(data, 82)?,
        right_hand: read_pose(data, 184)?,
        head: read_pose(data, 388)?,
    })
}

fn read_u32(data: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(offset..offset + 4)?.try_into().ok()?))
}

fn read_hand_input(data: &[u8], offset: usize) -> Option<AirLinkHandInput> {
    let released_trigger = read_f32(data, offset + 8)?;
    let released_grip = read_f32(data, offset + 12)?;
    let clean = |v: f32| if v.is_finite() { v.clamp(0.0, 1.0) } else { 0.0 };
    let axis = |offset| {
        let value = read_f32(data, offset)?;
        Some(if value.is_finite() {
            value.clamp(-1.0, 1.0)
        } else {
            0.0
        })
    };
    Some(AirLinkHandInput {
        buttons: read_u32(data, offset)?,
        touches: read_u32(data, offset + 4)?,
        trigger: 1.0 - clean(released_trigger),
        grip: 1.0 - clean(released_grip),
        thumbstick_xy: [axis(offset + 17)?, axis(offset + 21)?],
    })
}

pub fn unpack_controller_input(data: &[u8]) -> Option<AirLinkControllerInput> {
    if data.len() != 227 {
        return None;
    }
    Some(AirLinkControllerInput {
        left: read_hand_input(data, 37)?,
        right: read_hand_input(data, 98)?,
        battery_percent: [data[36], data[97], data[223]],
    })
}
