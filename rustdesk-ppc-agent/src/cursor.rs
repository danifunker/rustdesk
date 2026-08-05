//! The remote pointer, which the framebuffer does not contain.
//!
//! On this vintage the cursor is drawn by the hardware as an overlay, not
//! composited into the scanout buffer we read. Measured: a 100x100 patch of a
//! captured frame centred on the pointer contains exactly **one** colour. So a
//! peer sees no pointer at all unless the agent sends one, which makes pointing
//! at anything guesswork.
//!
//! Reading the *actual* cursor image is not available here. `NSCursor` only
//! knows about the calling application's own cursor, and the system-wide shape
//! lives behind private CoreGraphics calls (`CGSCurrentCursorSeed` and
//! friends). So this sends a standard arrow once, and then only tracks where it
//! is. The shape is wrong while the pointer is over a text field or a resize
//! edge; the position, which is what actually matters for aiming, is right.

/// Any id will do, since only one shape is ever sent. The client keys its
/// cached shapes on this.
pub const CURSOR_ID: u64 = 1;

/// The classic arrow, as a mask: `#` outline, `.` fill, space transparent.
///
/// Every row must be the same width -- a ragged row would shear the image, and
/// the test below is what stops that happening quietly.
const ARROW: [&str; 19] = [
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#....#####  ",
    "#..#..#     ",
    "#.# #..#    ",
    "##  #..#    ",
    "#    #..#   ",
    "     #..#   ",
    "      #..#  ",
    "      #..#  ",
    "       ##   ",
];

pub struct Cursor {
    pub width: i32,
    pub height: i32,
    /// Where the point of the arrow is, relative to the image.
    pub hotx: i32,
    pub hoty: i32,
    /// RGBA, row-major, straight (not premultiplied) alpha.
    pub rgba: Vec<u8>,
}

/// Build the arrow. Cheap, and only done once per session.
pub fn arrow() -> Cursor {
    let height = ARROW.len();
    let width = ARROW[0].len();
    let mut rgba = Vec::with_capacity(width * height * 4);
    for row in ARROW.iter() {
        let mut n = 0;
        for ch in row.chars() {
            match ch {
                '#' => rgba.extend_from_slice(&[0, 0, 0, 255]),
                '.' => rgba.extend_from_slice(&[255, 255, 255, 255]),
                _ => rgba.extend_from_slice(&[0, 0, 0, 0]),
            }
            n += 1;
        }
        // Pad a short row rather than emit a sheared image.
        for _ in n..width {
            rgba.extend_from_slice(&[0, 0, 0, 0]);
        }
    }
    Cursor { width: width as i32, height: height as i32, hotx: 0, hoty: 0, rgba }
}

/// Tracks the pointer so a position is only sent when it actually moves.
///
/// Worth the state: the poll runs every ~30 ms, and a client that is shown the
/// same coordinates thirty times a second is being sent traffic for nothing.
pub struct Tracker {
    last: Option<(i32, i32)>,
}

impl Tracker {
    pub fn new() -> Self {
        Self { last: None }
    }

    /// Forget the last position, so the next update reports even if the
    /// pointer has not moved. Used when a peer newly asks for the cursor: it
    /// needs a position now, not whenever the pointer next happens to move.
    pub fn reset(&mut self) {
        self.last = None;
    }

    /// Returns the position to send, or None if it has not moved.
    pub fn update(&mut self, x: i32, y: i32) -> Option<(i32, i32)> {
        if self.last == Some((x, y)) {
            None
        } else {
            self.last = Some((x, y));
            Some((x, y))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A ragged mask would shear every row after the short one.
    #[test]
    fn every_mask_row_is_the_same_width() {
        let w = ARROW[0].len();
        for (i, row) in ARROW.iter().enumerate() {
            assert_eq!(row.len(), w, "row {} is {} wide, expected {}", i, row.len(), w);
        }
    }

    #[test]
    fn the_image_is_rgba_and_the_right_size() {
        let c = arrow();
        assert_eq!(c.rgba.len(), (c.width * c.height * 4) as usize);
        assert_eq!((c.width, c.height), (12, 19));
    }

    /// The hotspot is the tip of the arrow, and the tip must be opaque -- a
    /// transparent pixel there means the pointer appears offset from where it
    /// actually is.
    #[test]
    fn the_hotspot_pixel_is_opaque() {
        let c = arrow();
        let i = ((c.hoty * c.width + c.hotx) * 4) as usize;
        assert_eq!(c.rgba[i + 3], 255, "the hotspot must not be transparent");
    }

    #[test]
    fn transparent_pixels_are_fully_transparent_and_the_fill_is_opaque() {
        let c = arrow();
        let alphas: Vec<u8> = c.rgba.chunks(4).map(|p| p[3]).collect();
        assert!(alphas.iter().any(|a| *a == 0), "nothing is transparent");
        assert!(alphas.iter().any(|a| *a == 255), "nothing is opaque");
        assert!(alphas.iter().all(|a| *a == 0 || *a == 255), "no partial alpha is intended");
    }

    #[test]
    fn a_reset_makes_the_next_position_report_again() {
        let mut t = Tracker::new();
        assert_eq!(t.update(5, 5), Some((5, 5)));
        assert_eq!(t.update(5, 5), None);
        t.reset();
        assert_eq!(t.update(5, 5), Some((5, 5)), "after a reset the position is news again");
    }

    #[test]
    fn a_position_is_only_reported_when_it_changes() {
        let mut t = Tracker::new();
        assert_eq!(t.update(10, 10), Some((10, 10)), "the first position is always news");
        assert_eq!(t.update(10, 10), None);
        assert_eq!(t.update(10, 11), Some((10, 11)));
        assert_eq!(t.update(10, 11), None);
        assert_eq!(t.update(0, 0), Some((0, 0)), "the origin is a real position");
    }
}
