//! Quest v209 Air Link controller tracking and input packet decoding.

#[derive(Clone, Copy, Debug)]
pub struct AirLinkPose {
    pub orientation_xyzw: [f32; 4],
    pub position_xyz: [f32; 3],
}

#[derive(Clone, Copy, Debug)]
pub struct AirLinkTrackingSample {
    pub head: AirLinkPose,
    /// When the head pose was sampled, on the headset's clock, in seconds
    /// (f64 at 422, 34 bytes into the pose).
    pub head_time_s: f64,
    /// Absent while the controller is asleep or out of sight: its pose is
    /// then not a rotation. The head pose of the packet is still good.
    pub left_hand: Option<AirLinkPose>,
    pub right_hand: Option<AirLinkPose>,
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
    /// Height of the tracking space's origin above the floor, f32 at 25. With
    /// the head's y it gives 1.1 to 1.2 m in captures of the user seated and
    /// 1.5 m playing upright; it changes when the user recenters (to 1.1) or
    /// sets the Guardian up again.
    pub origin_height: Option<f32>,
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
        left_hand: read_pose(data, 82),
        right_hand: read_pose(data, 184),
        head: read_pose(data, 388)?,
        head_time_s: read_f64(data, 422)?,
    })
}

fn read_f64(data: &[u8], offset: usize) -> Option<f64> {
    Some(f64::from_le_bytes(data.get(offset..offset + 8)?.try_into().ok()?))
}

/// Extrapolates the head pose to where it will be when the frame rendered
/// for it is shown. Meta Horizon Link does the same: the pose in each of its
/// video frames matches the headset's tracking samples of about 40 ms after
/// the frame was sent. Without it the headset has to reproject every frame
/// over the whole pipeline latency, and the picture shakes on fast moves.
pub struct HeadPredictor {
    // (time, pose) of the last tens of milliseconds, oldest first.
    history: std::collections::VecDeque<(f64, AirLinkPose)>,
    lead_s: f64,
}

impl HeadPredictor {
    /// Velocity is measured against the sample this much older.
    const BASELINE_S: f64 = 0.016;

    pub fn new(lead_ms: f64) -> Self {
        Self {
            history: std::collections::VecDeque::new(),
            lead_s: lead_ms.clamp(0.0, 100.0) / 1000.0,
        }
    }

    pub fn set_lead_ms(&mut self, lead_ms: f64) {
        self.lead_s = lead_ms.clamp(0.0, 100.0) / 1000.0;
    }

    pub fn predict(&mut self, pose: AirLinkPose, time_s: f64) -> AirLinkPose {
        // A clock that went back is a new session.
        if self.history.back().is_some_and(|(last, _)| time_s <= *last) {
            self.history.clear();
        }
        self.history.push_back((time_s, pose));
        while self
            .history
            .get(1)
            .is_some_and(|(next, _)| time_s - *next >= Self::BASELINE_S)
        {
            self.history.pop_front();
        }
        let (old_time, old) = self.history[0];
        let dt = time_s - old_time;
        if self.lead_s == 0.0 || dt < Self::BASELINE_S / 2.0 || dt > 0.1 {
            return pose;
        }
        let scale = (self.lead_s / dt) as f32;

        // Rotation since the old sample, in the tracking space: q * old^-1.
        let [x0, y0, z0, w0] = old.orientation_xyzw;
        let delta = quat_mul(pose.orientation_xyzw, [-x0, -y0, -z0, w0]);
        let delta = if delta[3] < 0.0 {
            delta.map(|component| -component)
        } else {
            delta
        };
        // The same rotation again, `scale` times, on top of the pose.
        let sine = (delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]).sqrt();
        let orientation_xyzw = if sine < 1e-6 {
            pose.orientation_xyzw
        } else {
            let half_angle = sine.atan2(delta[3]) * scale;
            let factor = half_angle.sin() / sine;
            quat_mul(
                [delta[0] * factor, delta[1] * factor, delta[2] * factor, half_angle.cos()],
                pose.orientation_xyzw,
            )
        };
        let mut position_xyz = pose.position_xyz;
        for axis in 0..3 {
            position_xyz[axis] += (pose.position_xyz[axis] - old.position_xyz[axis]) * scale;
        }
        AirLinkPose {
            orientation_xyzw,
            position_xyz,
        }
    }
}

fn quat_mul(a: [f32; 4], b: [f32; 4]) -> [f32; 4] {
    let [ax, ay, az, aw] = a;
    let [bx, by, bz, bw] = b;
    [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ]
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
        origin_height: read_f32(data, 25).filter(|height| (0.1..3.0).contains(height)),
    })
}
