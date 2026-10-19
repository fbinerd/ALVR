//! Opt-in frame starvation for Linux Air Link diagnostics.

use std::{
    path::Path,
    time::{Duration, Instant},
};

#[derive(Default)]
pub(crate) struct FrameGate {
    last_poll: Option<Instant>,
    divisor: u32,
    remaining: u32,
}

impl FrameGate {
    pub(crate) fn should_encode(&mut self, active: bool, path: &Path) -> bool {
        if !active {
            self.last_poll = None;
            self.remaining = 0;
            alvr_airlink::video::set_test_frame_divisor(1);
            return true;
        }

        let now = Instant::now();
        if self
            .last_poll
            .is_none_or(|last| now.duration_since(last) >= Duration::from_secs(1))
        {
            self.last_poll = Some(now);
            let divisor = std::fs::read_to_string(path)
                .ok()
                .and_then(|text| text.trim().parse::<u32>().ok())
                .unwrap_or(1)
                .max(1);
            let previous = self.divisor.max(1);
            if previous != divisor {
                alvr_common::info!(
                    "Air Link test frame divisor: {previous} -> {divisor}; \
                     admit 1/{divisor} frames before encoding, display refresh unchanged"
                );
            }
            if self.divisor != divisor {
                self.remaining = 0;
            }
            self.divisor = divisor;
            alvr_airlink::video::set_test_frame_divisor(divisor);
        }

        if self.remaining == 0 {
            self.remaining = self.divisor.max(1) - 1;
            true
        } else {
            self.remaining -= 1;
            false
        }
    }
}
