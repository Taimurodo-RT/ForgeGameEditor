//! What the player sees, as `Camera2D` in engine/render.

use forge_world::Rect;

/// The centre of the screen in tiles and pixels per tile.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Camera2D {
    pub x: f64,
    pub y: f64,
    pub zoom: f32,
}

impl Default for Camera2D {
    fn default() -> Self {
        Camera2D { x: 0.0, y: 0.0, zoom: 16.0 }
    }
}

impl Camera2D {
    /// Tiles covering a screen of width × height pixels.
    pub fn visible_tiles(&self, width: u32, height: u32) -> Rect {
        let zoom = self.zoom as f64;
        let half_w = width as f64 / (2.0 * zoom);
        let half_h = height as f64 / (2.0 * zoom);
        Rect {
            x0: (self.x - half_w).floor() as i32,
            y0: (self.y - half_h).floor() as i32,
            x1: (self.x + half_w).ceil() as i32 + 1,
            y1: (self.y + half_h).ceil() as i32 + 1,
        }
    }

    pub fn screen_to_tile(&self, sx: f32, sy: f32, width: u32, height: u32) -> (f64, f64) {
        let zoom = self.zoom as f64;
        (self.x + (sx as f64 - width as f64 * 0.5) / zoom, self.y + (sy as f64 - height as f64 * 0.5) / zoom)
    }

    /// The centre moved to a whole screen pixel, so the picture does not shimmer while scrolling. Every renderer
    /// draws around this point.
    pub fn snapped_x(&self) -> f64 {
        (self.x * self.zoom as f64).round() / self.zoom as f64
    }

    pub fn snapped_y(&self) -> f64 {
        (self.y * self.zoom as f64).round() / self.zoom as f64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn visible_tiles_and_screen_points() {
        let c = Camera2D { x: 10.0, y: -3.5, zoom: 16.0 };
        assert_eq!(c.visible_tiles(320, 160), Rect { x0: 0, y0: -9, x1: 21, y1: 3 });
        assert_eq!(c.screen_to_tile(160.0, 80.0, 320, 160), (10.0, -3.5));
        assert_eq!(c.screen_to_tile(0.0, 0.0, 320, 160), (0.0, -8.5));
        let s = Camera2D { x: 1.0 / 32.0 + 0.001, y: 0.0, zoom: 16.0 };
        assert_eq!(s.snapped_x(), 1.0 / 16.0);
    }
}
