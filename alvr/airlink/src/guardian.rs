//! Guardian data of a Quest v209, read from its RuntimeService RPC responses.
//!
//! This is Air Link's own source of floor height and boundary. ALVR clients
//! report their play space through `PlayspaceSync`, which is left untouched.

const RESPONSE_PREFIX: [u8; 3] = [0x05, 0x00, 0x01];
const RESPONSE_TYPE_POLYGON: u32 = 0xde32_e6b1; // GetGuardianPolygon
const RESPONSE_TYPE_BOUNDS: u32 = 0x2745_13f0; // GetGuardianBounds
const POLYGON_POINTS_OFFSET: usize = 11;
const BOUNDS_OFFSET: usize = 8;

/// What the headset's Guardian knows, in the tracking space of its poses.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AirLinkGuardian {
    /// Height of the floor. Absent with a stationary boundary: the headset
    /// then returns an empty polygon.
    pub floor_y: Option<f32>,
    /// Boundary outline on the floor as x, z points.
    pub perimeter: Vec<[f32; 2]>,
    /// Width and depth of the play area in metres.
    pub play_area: Option<[f32; 2]>,
}

impl AirLinkGuardian {
    /// Take in one RPC response payload. True when it changed the Guardian.
    pub fn update(&mut self, payload: &[u8]) -> bool {
        if payload.len() < 8 || payload[..3] != RESPONSE_PREFIX {
            return false;
        }
        let float = |offset: usize| {
            f32::from_le_bytes(payload[offset..offset + 4].try_into().unwrap())
        };
        let before = self.clone();
        match u32::from_le_bytes(payload[3..7].try_into().unwrap()) {
            RESPONSE_TYPE_BOUNDS if payload.len() >= BOUNDS_OFFSET + 8 => {
                let area = [float(BOUNDS_OFFSET), float(BOUNDS_OFFSET + 4)];
                if area.iter().all(|side| side.is_finite() && *side > 0.0) {
                    self.play_area = Some(area);
                }
            }
            RESPONSE_TYPE_POLYGON => {
                let count = payload.len().saturating_sub(POLYGON_POINTS_OFFSET) / 12;
                let points: Vec<[f32; 3]> = (0..count)
                    .map(|index| {
                        let offset = POLYGON_POINTS_OFFSET + 12 * index;
                        [float(offset), float(offset + 4), float(offset + 8)]
                    })
                    .filter(|point| point.iter().all(|value| value.is_finite()))
                    .collect();
                if !points.is_empty() {
                    // Every point carries the floor's height: take the median.
                    let mut heights: Vec<f32> = points.iter().map(|point| point[1]).collect();
                    heights.sort_by(f32::total_cmp);
                    self.floor_y = Some(heights[heights.len() / 2]);
                    self.perimeter = points.iter().map(|point| [point[0], point[2]]).collect();
                }
            }
            _ => {}
        }
        *self != before
    }

    /// Play area size: the reported one, or the outline's bounding box.
    pub fn play_area_or_extent(&self) -> Option<[f32; 2]> {
        self.play_area.or_else(|| {
            let (first, rest) = self.perimeter.split_first()?;
            let (mut low, mut high) = (*first, *first);
            for point in rest {
                for axis in 0..2 {
                    low[axis] = low[axis].min(point[axis]);
                    high[axis] = high[axis].max(point[axis]);
                }
            }
            Some([high[0] - low[0], high[1] - low[1]])
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn response(type_hash: u32, padding: usize, values: &[f32]) -> Vec<u8> {
        let mut payload = RESPONSE_PREFIX.to_vec();
        payload.extend(type_hash.to_le_bytes());
        payload.resize(padding, 0);
        for value in values {
            payload.extend(value.to_le_bytes());
        }
        payload
    }

    #[test]
    fn polygon_gives_floor_and_outline() {
        let mut guardian = AirLinkGuardian::default();
        let payload = response(
            RESPONSE_TYPE_POLYGON,
            POLYGON_POINTS_OFFSET,
            &[-1.0, -1.5, -2.0, 1.0, -1.5, -2.0, 1.0, -1.5, 2.0, -1.0, -1.5, 2.0],
        );
        assert!(guardian.update(&payload));
        assert_eq!(guardian.floor_y, Some(-1.5));
        assert_eq!(guardian.perimeter, [[-1.0, -2.0], [1.0, -2.0], [1.0, 2.0], [-1.0, 2.0]]);
        assert_eq!(guardian.play_area_or_extent(), Some([2.0, 4.0]));
        assert!(!guardian.update(&payload));
    }

    #[test]
    fn empty_polygon_leaves_the_floor_unknown() {
        let mut guardian = AirLinkGuardian::default();
        assert!(!guardian.update(&response(RESPONSE_TYPE_POLYGON, POLYGON_POINTS_OFFSET, &[])));
        assert_eq!(guardian.floor_y, None);
    }

    #[test]
    fn bounds_give_the_play_area() {
        let mut guardian = AirLinkGuardian::default();
        assert!(guardian.update(&response(RESPONSE_TYPE_BOUNDS, BOUNDS_OFFSET, &[2.5, 3.0])));
        assert_eq!(guardian.play_area_or_extent(), Some([2.5, 3.0]));
    }
}
