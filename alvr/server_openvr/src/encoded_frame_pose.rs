//! Exact frame geometry, indexed by the existing wire stamp. No clock relabel.
use std::collections::VecDeque;

pub struct FramePoses<T> {
    entries: VecDeque<(u64, T)>,
}

impl<T: Copy> FramePoses<T> {
    pub const fn new() -> Self { Self { entries: VecDeque::new() } }

    pub fn insert(&mut self, stamp: u64, pose: T) {
        // Multiple NALs may query the same frame; replace rather than consume.
        if let Some((_, value)) = self.entries.iter_mut().find(|(s, _)| *s == stamp) {
            *value = pose;
            return;
        }
        self.entries.push_back((stamp, pose));
        while self.entries.len() > 360 { self.entries.pop_front(); }
    }

    pub fn get(&self, stamp: u64) -> Option<T> {
        self.entries.iter().rev().find_map(|(s, p)| (*s == stamp).then_some(*p))
    }

    pub fn clear(&mut self) { self.entries.clear(); }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_multi_nal_bounded_and_epoch_reset() {
        let mut poses = FramePoses::new();
        assert_eq!(poses.get(42), None);
        poses.insert(42, 7);
        assert_eq!(poses.get(42), Some(7));
        assert_eq!(poses.get(42), Some(7));
        assert_eq!(poses.get(43), None); // Never use nearest geometry.
        poses.insert(42, 8);
        assert_eq!(poses.get(42), Some(8));
        for s in 43..403 { poses.insert(s, s); }
        assert_eq!(poses.get(42), None);
        assert_eq!(poses.entries.len(), 360);
        poses.clear();
        assert_eq!(poses.get(402), None);
        poses.insert(42, 9);
        assert_eq!(poses.get(42), Some(9));
    }
}
