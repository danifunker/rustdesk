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

#[cfg(target_os = "macos")]
extern "C" {
    fn rd_cursor_seed() -> std::os::raw::c_int;
    fn rd_cursor_image(
        out: *mut u8,
        out_len: std::os::raw::c_int,
        w: *mut std::os::raw::c_int,
        h: *mut std::os::raw::c_int,
        hotx: *mut std::os::raw::c_int,
        hoty: *mut std::os::raw::c_int,
    ) -> std::os::raw::c_int;
}

/// Largest cursor accepted, in pixels each way.
///
/// Real ones are 24x24 or 32x32; the cap exists so a nonsense size cannot make
/// the agent allocate for it. Anything bigger is skipped, keeping the shape
/// already sent.
const MAX_CURSOR_PX: usize = 128;

/// A number that changes whenever the pointer changes shape.
///
/// Cheap enough to poll every pass, which is the whole point: fetching the
/// image costs an allocation and a copy, and the shape changes a handful of
/// times a session.
#[cfg(target_os = "macos")]
pub fn seed() -> i32 {
    unsafe { rd_cursor_seed() }
}

#[cfg(not(target_os = "macos"))]
pub fn seed() -> i32 {
    0
}

/// The pointer as it looks right now, or `None` if it could not be read.
///
/// `None` is not a failure to report to the peer -- the caller keeps sending
/// whatever it had, or falls back to [`arrow`]. A pointer in the right place
/// with the wrong picture is much better than no pointer.
#[cfg(target_os = "macos")]
pub fn current() -> Option<Cursor> {
    let mut rgba = vec![0u8; MAX_CURSOR_PX * MAX_CURSOR_PX * 4];
    let (mut w, mut h, mut hotx, mut hoty) = (0, 0, 0, 0);
    let n = unsafe {
        rd_cursor_image(
            rgba.as_mut_ptr(),
            rgba.len() as std::os::raw::c_int,
            &mut w,
            &mut h,
            &mut hotx,
            &mut hoty,
        )
    };
    if n <= 0 || w <= 0 || h <= 0 {
        return None;
    }
    rgba.truncate(n as usize);
    Some(Cursor {
        width: w,
        height: h,
        hotx,
        hoty,
        rgba,
    })
}

#[cfg(not(target_os = "macos"))]
pub fn current() -> Option<Cursor> {
    None
}

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

/// How long after a peer's own input its cursor position stays suppressed.
///
/// Upstream's value, from `run_pos` in `src/server/input_service.rs`, which
/// excludes the connection whose input arrived within the last 300 ms.
pub const SUPPRESS_AFTER_INPUT_MS: u64 = 300;

/// Tracks the pointer so a position is only sent when it actually moves, and
/// not back at the peer that just moved it.
///
/// The suppression is not an optimisation. The client treats an incoming
/// position as the remote side taking over: `setCursorPosition` sets
/// `got_mouse_control = false`, after which it *discards* its own mouse events
/// until one moves more than 12 pixels at once. Echoing positions back at a
/// peer that is driving therefore stops its mouse working -- it moves for the
/// moment before the first position arrives, then stops.
pub struct Tracker {
    last: Option<(i32, i32)>,
    /// Framebuffer pixels per peer pixel. See [`Tracker::set_scale`].
    scale: i32,
}

impl Tracker {
    pub fn new() -> Self {
        Self { last: None, scale: 1 }
    }

    /// Tell the tracker the peer's coordinate space is *downscaled*, and by
    /// how much.
    ///
    /// The mirror of `input::Injector::set_display_scale`. The session tells
    /// the peer the display is the size of the frames it will receive, so a
    /// pointer sitting at (200, 200) on the framebuffer is at (100, 100) as far
    /// as the peer is concerned. Sending the framebuffer figure would put the
    /// client's pointer off the bottom-right of its own canvas.
    ///
    /// Comparison happens *after* scaling, deliberately: at 1/2 the pointer has
    /// to move two framebuffer pixels before the peer's position changes at
    /// all, and reporting a position identical to the last one is a message
    /// that says nothing and, worse, re-triggers the client's "the remote side
    /// is driving" suppression.
    pub fn set_scale(&mut self, scale: usize) {
        let s = scale.max(1) as i32;
        if s != self.scale {
            self.scale = s;
            self.last = None;
        }
    }

    /// Forget the last position, so the next update reports even if the
    /// pointer has not moved. Used when a peer newly asks for the cursor: it
    /// needs a position now, not whenever the pointer next happens to move.
    pub fn reset(&mut self) {
        self.last = None;
    }

    /// Returns the position to send, or None to stay quiet.
    ///
    /// `since_peer_input_ms` is how long ago this peer last sent input. The
    /// position is always recorded, as upstream records it, so that once the
    /// suppression window passes only a genuine new movement is reported.
    pub fn update(&mut self, x: i32, y: i32, since_peer_input_ms: u64) -> Option<(i32, i32)> {
        let (x, y) = (x / self.scale, y / self.scale);
        let changed = self.last != Some((x, y));
        self.last = Some((x, y));
        if !changed || since_peer_input_ms < SUPPRESS_AFTER_INPUT_MS {
            return None;
        }
        Some((x, y))
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

    /// Long enough ago that nothing is suppressed.
    const IDLE: u64 = 10_000;

    #[test]
    fn a_reset_makes_the_next_position_report_again() {
        let mut t = Tracker::new();
        assert_eq!(t.update(5, 5, IDLE), Some((5, 5)));
        assert_eq!(t.update(5, 5, IDLE), None);
        t.reset();
        assert_eq!(t.update(5, 5, IDLE), Some((5, 5)), "after a reset the position is news again");
    }

    #[test]
    fn a_position_is_only_reported_when_it_changes() {
        let mut t = Tracker::new();
        assert_eq!(t.update(10, 10, IDLE), Some((10, 10)), "the first position is always news");
        assert_eq!(t.update(10, 10, IDLE), None);
        assert_eq!(t.update(10, 11, IDLE), Some((10, 11)));
        assert_eq!(t.update(10, 11, IDLE), None);
        assert_eq!(t.update(0, 0, IDLE), Some((0, 0)), "the origin is a real position");
    }

    /// The peer that is driving must not be told where it just put the cursor:
    /// the client reads that as losing mouse control and starts dropping its
    /// own movements.
    #[test]
    fn a_peer_that_just_sent_input_is_not_told_the_position() {
        let mut t = Tracker::new();
        assert_eq!(t.update(100, 100, 0), None, "input this instant");
        assert_eq!(t.update(120, 120, 299), None, "still inside the window");
        assert_eq!(t.update(140, 140, 300), Some((140, 140)), "the window has passed");
    }

    /// Suppressed positions are still recorded, so when the window passes a
    /// stationary pointer stays quiet rather than emitting a stale position.
    #[test]
    fn a_suppressed_position_is_still_remembered() {
        let mut t = Tracker::new();
        assert_eq!(t.update(50, 50, 0), None);
        assert_eq!(t.update(50, 50, IDLE), None, "unchanged since the suppressed update");
        assert_eq!(t.update(51, 50, IDLE), Some((51, 50)));
    }

    /// The mirror of `input::Injector::set_display_scale`: the peer's canvas is
    /// the served size, so a pointer at (200, 160) on a 1/2 session is at
    /// (100, 80) as far as the peer is concerned. Sending the framebuffer
    /// figure would put the client's pointer off its own canvas.
    #[test]
    fn positions_are_reported_in_the_peers_coordinate_space() {
        let mut t = Tracker::new();
        t.set_scale(2);
        assert_eq!(t.update(200, 160, u64::MAX), Some((100, 80)));
    }

    /// Comparison happens after scaling, so a sub-scale wobble is not reported
    /// at all. It would be a message that says nothing, and worse: an incoming
    /// position is how the client decides the remote side has taken over, and
    /// it stops honouring its own mouse until something moves 12 pixels.
    #[test]
    fn a_move_too_small_to_show_is_not_reported() {
        let mut t = Tracker::new();
        t.set_scale(4);
        assert_eq!(t.update(400, 400, u64::MAX), Some((100, 100)));
        assert_eq!(t.update(402, 401, u64::MAX), None);
        assert_eq!(t.update(404, 400, u64::MAX), Some((101, 100)));
    }

    /// Changing scale mid-session invalidates the last position: it was
    /// recorded in the old space, and comparing across the two would either
    /// suppress the first real report or invent one.
    #[test]
    fn changing_scale_forgets_the_last_position() {
        let mut t = Tracker::new();
        assert_eq!(t.update(100, 100, u64::MAX), Some((100, 100)));
        t.set_scale(2);
        assert_eq!(t.update(200, 200, u64::MAX), Some((100, 100)));
    }
}
