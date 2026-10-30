//! Deterministic head pitch, independent of the GUI and the VR stack.
use std::time::Duration;

pub fn world_angular_velocity(yaw: f32, yaw_velocity: f32, pitch_velocity: f32) -> [f32; 3] {
    [pitch_velocity*yaw.cos(), yaw_velocity, -pitch_velocity*yaw.sin()]
}

#[derive(Clone, Copy)]
pub struct PitchSweep {
    amplitude: f32,
    period: f32,
}

impl PitchSweep {
    pub fn parse(value: &str) -> Option<Self> {
        let (amplitude, period) = value.split_once(',')?;
        let amplitude: f32 = amplitude.trim().parse().ok()?;
        let period: f32 = period.trim().parse().ok()?;
        (amplitude.is_finite() && amplitude.abs() <= 80.0 && period.is_finite() && period > 0.0)
            .then_some(Self { amplitude: amplitude.to_radians(), period })
    }

    pub fn from_env() -> Option<Self> {
        Self::parse(&std::env::var("MOCK_PITCH").ok()?)
    }

    pub fn at(self, t: Duration) -> (f32, f32) {
        let omega = std::f32::consts::TAU / self.period;
        let phase = omega * t.as_secs_f32();
        (self.amplitude * phase.sin(), self.amplitude * omega * phase.cos())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn analytic_pitch_and_derivative() {
        let sweep = PitchSweep::parse("15,3").unwrap();
        let near = |a: f32, b: f32| assert!((a-b).abs()<1e-4, "{a} != {b}");
        near(sweep.at(Duration::ZERO).0, 0.0);
        near(sweep.at(Duration::from_secs_f32(0.75)).0, 15_f32.to_radians());
        near(sweep.at(Duration::from_secs_f32(0.75)).1, 0.0);
        for n in 1..300 {
            let t = n as f32*0.01;
            let value = |t| sweep.at(Duration::from_secs_f32(t)).0;
            near((value(t+0.001)-value(t-0.001))/0.002, sweep.at(Duration::from_secs_f32(t)).1);
        }
        for invalid in ["NaN,3","inf,3","15,NaN","15,0","15,-3","90,3","1,2,3",""] {
            assert!(PitchSweep::parse(invalid).is_none(), "{invalid}");
        }
    }

    #[test]
    fn yaw_pitch_velocity_is_world_space() {
        let forward = |t: f32| {
            let yaw = 0.7+0.3*t;
            let pitch = 0.2*(2.0*t).sin();
            [-yaw.sin()*pitch.cos(), pitch.sin(), -yaw.cos()*pitch.cos()]
        };
        for n in 1..200 {
            let t = n as f32*0.01;
            let v = forward(t);
            let w = world_angular_velocity(0.7+0.3*t, 0.3, 0.4*(2.0*t).cos());
            let derivative = [w[1]*v[2]-w[2]*v[1], w[2]*v[0]-w[0]*v[2], w[0]*v[1]-w[1]*v[0]];
            let a = forward(t-0.001);
            let b = forward(t+0.001);
            for i in 0..3 { assert!(((b[i]-a[i])/0.002-derivative[i]).abs()<2e-4); }
        }
        assert_eq!(world_angular_velocity(0.0, 0.3, 0.0), [0.0,0.3,-0.0]);
    }
}
