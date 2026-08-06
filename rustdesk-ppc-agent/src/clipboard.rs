//! Clipboard text, in both directions.
//!
//! **Text only, deliberately.** A modern client tags every entry with a
//! `ClipboardFormat` and sends several at once -- copying from a browser
//! produces text, HTML and RTF together -- and this picks the text one and
//! drops the rest. Images would mean converting RGBA or PNG into pasteboard
//! flavours and back; RTF and HTML would paste into TextEdit and confuse
//! everything else. Text is what a remote session is actually for.
//!
//! Two things about talking to a modern client had to be established before any
//! of this could work, and neither is visible from the 1.1.8 proto:
//!
//! * **The message moved.** At 1.3.0 and above the client puts its clipboard in
//!   `MultiClipboards` (field 28), not `Clipboard` (16), for every peer that is
//!   not iOS. This agent reports 1.4.5, so field 16 never arrives from a desktop
//!   peer at all. Both are handled; only one is ever seen.
//! * **The content is really zstd.** `Clipboard.compress` is set whenever
//!   compressing helped, and then `content` is a genuine zstd stream -- not the
//!   raw-block frames [`crate::zstd_frame`] writes, which exist because the
//!   *cursor* path only ever needs to produce them. Reading a peer's clipboard
//!   needs a real decompressor, so this links libzstd (1.5.7 on the G5, in
//!   `/opt/local` alongside the libraries the binary already depends on).

use libc::{c_int, c_uint, c_ulonglong, size_t};

use crate::message_proto::*;

/// Largest clipboard accepted from a peer, before and after decompression.
///
/// A remote clipboard arrives over a session that is already tight on memory
/// and time, and a decompression bomb is a 100 KB message that asks for a
/// gigabyte. Upstream has no such limit because it is not running on a G5.
const MAX_CLIPBOARD: usize = 4 << 20; // 4 MiB of text

#[link(name = "zstd")]
extern "C" {
    fn ZSTD_decompress(
        dst: *mut u8,
        dst_cap: size_t,
        src: *const u8,
        src_size: size_t,
    ) -> size_t;
    fn ZSTD_getFrameContentSize(src: *const u8, src_size: size_t) -> c_ulonglong;
    fn ZSTD_compress(
        dst: *mut u8,
        dst_cap: size_t,
        src: *const u8,
        src_size: size_t,
        level: c_int,
    ) -> size_t;
    fn ZSTD_compressBound(src_size: size_t) -> size_t;
    fn ZSTD_isError(code: size_t) -> c_uint;
}

/// `ZSTD_getFrameContentSize` returns these rather than a size.
const ZSTD_CONTENTSIZE_UNKNOWN: c_ulonglong = 0u64.wrapping_sub(1);
const ZSTD_CONTENTSIZE_ERROR: c_ulonglong = 0u64.wrapping_sub(2);

/// Decompress a peer's clipboard, refusing anything implausible.
///
/// The declared size is checked *before* allocating, which is the point: a
/// frame header is free to claim a gigabyte, and finding that out by asking for
/// one is not an option here.
pub fn decompress(src: &[u8]) -> Option<Vec<u8>> {
    if src.is_empty() {
        return None;
    }
    let declared = unsafe { ZSTD_getFrameContentSize(src.as_ptr(), src.len() as size_t) };
    if declared == ZSTD_CONTENTSIZE_ERROR {
        return None;
    }
    // A stream without a declared size is legal but nothing this agent talks to
    // produces one, and honouring it would mean growing a buffer against an
    // unknown bound.
    if declared == ZSTD_CONTENTSIZE_UNKNOWN || declared as u64 > MAX_CLIPBOARD as u64 {
        return None;
    }
    let mut out = vec![0u8; declared as usize];
    let n = unsafe {
        ZSTD_decompress(
            out.as_mut_ptr(),
            out.len() as size_t,
            src.as_ptr(),
            src.len() as size_t,
        )
    };
    if unsafe { ZSTD_isError(n) } != 0 || n as u64 != declared {
        return None;
    }
    out.truncate(n as usize);
    Some(out)
}

/// Compress, or return None when it did not help.
///
/// The caller sends the original in that case and leaves `compress` unset,
/// which is exactly what the client does and what it expects.
pub fn compress(src: &[u8]) -> Option<Vec<u8>> {
    let cap = unsafe { ZSTD_compressBound(src.len() as size_t) };
    let mut out = vec![0u8; cap as usize];
    let n = unsafe {
        ZSTD_compress(
            out.as_mut_ptr(),
            cap,
            src.as_ptr(),
            src.len() as size_t,
            // Upstream's default. The clipboard is small and occasional, so
            // this is not a dial worth turning.
            3,
        )
    };
    if unsafe { ZSTD_isError(n) } != 0 || n as usize >= src.len() {
        return None;
    }
    out.truncate(n as usize);
    Some(out)
}

/// The text out of one `Clipboard`, or None if it is not text we can use.
///
/// `format` defaults to `Text` when absent, which is what a 1.1.8-era peer
/// sends -- it had no format field, and text was all it could mean.
pub fn text_of(c: &Clipboard) -> Option<String> {
    if c.format.enum_value_or_default() != ClipboardFormat::Text {
        return None;
    }
    if c.content.len() > MAX_CLIPBOARD {
        return None;
    }
    let bytes = if c.compress {
        decompress(&c.content)?
    } else {
        c.content.to_vec()
    };
    // Invalid UTF-8 is dropped rather than repaired: the pasteboard flavour we
    // write is `public.utf8-plain-text`, and lossy conversion would silently
    // corrupt the paste instead of failing it.
    String::from_utf8(bytes).ok()
}

/// The text a peer sent, from whichever message carried it.
///
/// A `MultiClipboards` holds one entry per flavour of a single copy, so the
/// first text one is the copy -- there is never a second, competing text entry.
pub fn incoming_text(msg: &message::Union) -> Option<String> {
    match msg {
        message::Union::clipboard(c) => text_of(c),
        message::Union::multi_clipboards(m) => m.clipboards.iter().find_map(text_of),
        _ => None,
    }
}

/// Does this peer understand `MultiClipboards`?
///
/// Upstream's `is_support_multi_clipboard`, reimplemented rather than guessed,
/// because the guess would be wrong in the interesting direction. **The version
/// gate runs both ways**: the client picks what to send from what we claim, and
/// we have to pick from what it claims, which arrives in `LoginRequest.version`
/// (field 11, backported). Sending field 28 to a 1.2 client would be a clipboard
/// that vanishes, and it would look exactly like the clipboard not working.
///
/// An empty version is an old or unknown peer and gets the old field. So does an
/// empty platform, which is upstream's own rule.
pub fn peer_takes_multi(version: &str, platform: &str) -> bool {
    if crate::session::version_number(version) < crate::session::version_number("1.3.0") {
        return false;
    }
    if platform.is_empty() || platform == "iOS" {
        return false;
    }
    if platform == "Android"
        && crate::session::version_number(version) < crate::session::version_number("1.3.3")
    {
        return false;
    }
    true
}

/// One `Clipboard` holding `text`, compressed if that helps.
fn entry(text: &str) -> Clipboard {
    let mut c = Clipboard::new();
    c.format = ClipboardFormat::Text.into();
    match compress(text.as_bytes()) {
        Some(z) => {
            c.compress = true;
            c.content = z.into();
        }
        None => {
            c.compress = false;
            c.content = text.as_bytes().to_vec().into();
        }
    }
    c
}

/// The message to send this peer for `text`, in the carrier it understands.
pub fn outgoing(text: &str, version: &str, platform: &str) -> Message {
    let mut msg = Message::new();
    if peer_takes_multi(version, platform) {
        let mut m = MultiClipboards::new();
        m.clipboards.push(entry(text));
        msg.set_multi_clipboards(m);
    } else {
        msg.set_clipboard(entry(text));
    }
    msg
}

// -- the Mac side ------------------------------------------------------------

#[cfg(target_os = "macos")]
extern "C" {
    fn rd_clip_ok() -> c_int;
    fn rd_clip_changed() -> c_int;
    fn rd_clip_get(out: *mut u8, cap: c_int) -> c_int;
    fn rd_clip_set(text: *const u8, len: c_int) -> c_int;
}

/// Is the clipboard reachable from this process at all?
///
/// **Usually not**, and that is not a bug in the code below. `PasteboardCreate`
/// fails from an ssh login and from the detached `screen` the agent normally
/// runs under -- measured, along with `pbcopy` and `pbpaste` failing the same
/// way, so it is the session rather than the API. The agent has to be started
/// from `deploy/com.rustdesk.ppc-agent.plist` for the clipboard to work.
/// Capture is no guide: that works over ssh, which is exactly why this had to be
/// established separately.
#[cfg(target_os = "macos")]
pub fn available() -> bool {
    unsafe { rd_clip_ok() != 0 }
}

#[cfg(not(target_os = "macos"))]
pub fn available() -> bool {
    false
}

/// Has the clipboard changed since the last check? None if it is unreachable.
#[cfg(target_os = "macos")]
pub fn changed() -> Option<bool> {
    match unsafe { rd_clip_changed() } {
        -1 => None,
        n => Some(n == 1),
    }
}

#[cfg(not(target_os = "macos"))]
pub fn changed() -> Option<bool> {
    None
}

/// The clipboard's text, or None if there is none, it is unreachable, or it is
/// larger than this agent will carry.
#[cfg(target_os = "macos")]
pub fn get() -> Option<String> {
    let mut buf = vec![0u8; MAX_CLIPBOARD];
    let n = unsafe { rd_clip_get(buf.as_mut_ptr(), buf.len() as c_int) };
    if n <= 0 {
        if n == -2 {
            log::debug!("clipboard is larger than {} bytes; not sending it", MAX_CLIPBOARD);
        }
        return None;
    }
    buf.truncate(n as usize);
    String::from_utf8(buf).ok()
}

#[cfg(not(target_os = "macos"))]
pub fn get() -> Option<String> {
    None
}

/// Put text on the clipboard. False if it could not be done.
#[cfg(target_os = "macos")]
pub fn set(text: &str) -> bool {
    unsafe { rd_clip_set(text.as_ptr(), text.len() as c_int) == 0 }
}

#[cfg(not(target_os = "macos"))]
pub fn set(_text: &str) -> bool {
    false
}

/// What the session keeps between passes, so the clipboard is not sent back and
/// forth for ever.
///
/// The loop is the whole difficulty. Writing a peer's clipboard onto the Mac
/// marks the pasteboard modified, so the next poll would read it back and send
/// it to the peer, whose own clipboard sync would apply it and send it back.
/// Comparing the *text* against the last thing that crossed in either direction
/// is what breaks it, and it breaks it in both directions with one rule.
#[derive(Default)]
pub struct Sync {
    last: Option<String>,
}

impl Sync {
    pub fn new() -> Self {
        Self::default()
    }

    /// The peer sent us this text: record it, and say whether it should be
    /// written to the Mac clipboard.
    pub fn accept(&mut self, text: String) -> bool {
        if self.last.as_deref() == Some(text.as_str()) {
            return false;
        }
        self.last = Some(text);
        true
    }

    /// The Mac clipboard now holds this: say whether the peer needs telling.
    pub fn offer(&mut self, text: String) -> Option<String> {
        if self.last.as_deref() == Some(text.as_str()) {
            return None;
        }
        self.last = Some(text.clone());
        Some(text)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn clip(text: &str, compressed: bool) -> Clipboard {
        let mut c = Clipboard::new();
        c.format = ClipboardFormat::Text.into();
        c.compress = compressed;
        c.content = if compressed {
            compress(text.as_bytes()).expect("test text must compress")
        } else {
            text.as_bytes().to_vec()
        }
        .into();
        c
    }

    /// Long enough that zstd actually helps, so the compressed path is real.
    fn long_text() -> String {
        "the quick brown fox jumps over the lazy dog. ".repeat(64)
    }

    #[test]
    fn zstd_round_trips_through_the_real_library() {
        let t = long_text();
        let z = compress(t.as_bytes()).expect("should compress");
        assert!(z.len() < t.len(), "compression did not help on repetitive text");
        assert_eq!(decompress(&z).unwrap(), t.as_bytes());
    }

    /// Short strings do not compress, and saying they did would make the peer
    /// try to inflate a plain string.
    /// A modern desktop peer, which is what almost every session is.
    fn modern(text: &str) -> Message {
        outgoing(text, "1.4.5", "Mac OS")
    }

    #[test]
    fn text_too_short_to_help_is_not_marked_compressed() {
        assert!(compress(b"hi").is_none());
        match modern("hi").union {
            Some(message::Union::multi_clipboards(m)) => {
                assert_eq!(m.clipboards.len(), 1);
                assert!(!m.clipboards[0].compress);
                assert_eq!(&m.clipboards[0].content[..], b"hi");
            }
            _ => panic!("expected multi_clipboards"),
        }
    }

    #[test]
    fn what_we_send_is_what_we_would_read_back() {
        for t in ["hi", &long_text(), "unicode: ✂ 📋 é"] {
            for (v, p) in [("1.4.5", "Mac OS"), ("1.2.0", "Windows"), ("", "")] {
                let msg = outgoing(t, v, p);
                assert_eq!(
                    incoming_text(msg.union.as_ref().unwrap()).as_deref(),
                    Some(t),
                    "round trip failed for a {:?}/{:?} peer",
                    v,
                    p
                );
            }
        }
    }

    // -- what the peer can actually decode ------------------------------------

    /// The version gate runs in both directions, and this is the direction that
    /// is easy to forget: sending `MultiClipboards` to a peer older than 1.3.0
    /// is a clipboard that silently vanishes, and it looks exactly like the
    /// clipboard not working.
    #[test]
    fn an_older_peer_gets_the_field_it_understands() {
        let old = outgoing("hello", "1.2.0", "Windows");
        assert!(
            matches!(old.union, Some(message::Union::clipboard(_))),
            "a 1.2.0 peer must be sent field 16"
        );
        let new = outgoing("hello", "1.3.0", "Windows");
        assert!(
            matches!(new.union, Some(message::Union::multi_clipboards(_))),
            "a 1.3.0 peer takes field 28"
        );
    }

    /// A peer that sends neither field is old enough not to have them, so it is
    /// assumed to be the oldest thing we support rather than the newest.
    #[test]
    fn a_peer_that_says_nothing_is_assumed_old() {
        assert!(!peer_takes_multi("", ""));
        assert!(matches!(
            outgoing("hello", "", "").union,
            Some(message::Union::clipboard(_))
        ));
        // A version without a platform is still not enough: upstream excludes
        // an empty platform explicitly.
        assert!(!peer_takes_multi("1.4.5", ""));
    }

    /// Upstream's two exclusions, mirrored rather than reasoned about.
    #[test]
    fn the_platform_exclusions_match_upstream() {
        assert!(!peer_takes_multi("1.4.5", "iOS"), "iOS never takes multi");
        assert!(!peer_takes_multi("1.3.0", "Android"), "Android needs 1.3.3");
        assert!(peer_takes_multi("1.3.3", "Android"));
        assert!(peer_takes_multi("1.4.5", "Linux"));
    }

    /// Both carriers, because the agent claims a version where only one of them
    /// is ever used and the other must still work for an older peer.
    #[test]
    fn text_is_found_in_either_carrier() {
        let mut single = Message::new();
        single.set_clipboard(clip("from field 16", false));
        assert_eq!(
            incoming_text(single.union.as_ref().unwrap()).as_deref(),
            Some("from field 16")
        );

        // Compressed, which needs text long enough for zstd to help -- a short
        // string legitimately comes back uncompressed and would test nothing.
        let long = long_text();
        let mut multi = MultiClipboards::new();
        multi.clipboards.push(clip(&long, true));
        let mut m = Message::new();
        m.set_multi_clipboards(multi);
        assert_eq!(incoming_text(m.union.as_ref().unwrap()).as_deref(), Some(&long[..]));
    }

    /// One copy from a browser arrives as three entries. The text one has to be
    /// picked out rather than the first one taken, or HTML markup gets pasted.
    #[test]
    fn the_text_entry_is_picked_out_of_a_browser_copy() {
        let mut m = MultiClipboards::new();
        let mut html = clip("<b>hello</b>", false);
        html.format = ClipboardFormat::Html.into();
        let mut rtf = clip("{\\rtf1 hello}", false);
        rtf.format = ClipboardFormat::Rtf.into();
        m.clipboards.push(html);
        m.clipboards.push(rtf);
        m.clipboards.push(clip("hello", false));

        let mut msg = Message::new();
        msg.set_multi_clipboards(m);
        assert_eq!(incoming_text(msg.union.as_ref().unwrap()).as_deref(), Some("hello"));
    }

    /// An entry with no format field set is text: that is what proto3 gives for
    /// an absent enum, and what a peer predating the field meant.
    #[test]
    fn an_absent_format_means_text() {
        let mut c = Clipboard::new();
        c.content = b"plain".to_vec().into();
        assert_eq!(text_of(&c).as_deref(), Some("plain"));
    }

    /// A copy with no text in it at all yields nothing, rather than yielding the
    /// image bytes as though they were a string.
    #[test]
    fn an_image_only_copy_produces_no_text() {
        let mut m = MultiClipboards::new();
        let mut img = clip("not really a png", false);
        img.format = ClipboardFormat::ImagePng.into();
        m.clipboards.push(img);
        let mut msg = Message::new();
        msg.set_multi_clipboards(m);
        assert_eq!(incoming_text(msg.union.as_ref().unwrap()), None);
    }

    // -- what a hostile or broken peer sends ---------------------------------

    /// The reason the declared size is checked before allocating: this frame is
    /// a few bytes and says it holds far more than the limit.
    #[test]
    fn a_decompression_bomb_is_refused_without_allocating_for_it() {
        let bomb = compress(&vec![0u8; MAX_CLIPBOARD + 1]).expect("should compress");
        assert!(bomb.len() < 4096, "the point is that the frame is small");
        assert_eq!(decompress(&bomb), None);
        // And through the real entry point, with the flag set.
        let mut c = Clipboard::new();
        c.compress = true;
        c.content = bomb.into();
        assert_eq!(text_of(&c), None);
    }

    #[test]
    fn rubbish_marked_as_compressed_is_dropped_not_pasted() {
        let mut c = Clipboard::new();
        c.compress = true;
        c.content = b"this is not a zstd frame at all".to_vec().into();
        assert_eq!(text_of(&c), None);
        assert_eq!(decompress(b"not zstd"), None);
        assert_eq!(decompress(b""), None);
    }

    /// The pasteboard flavour written is `public.utf8-plain-text`, so bytes that
    /// are not UTF-8 have to fail rather than be repaired into mojibake.
    #[test]
    fn invalid_utf8_is_dropped_rather_than_mangled() {
        let mut c = Clipboard::new();
        c.content = vec![0xff, 0xfe, 0x00, 0x80].into();
        assert_eq!(text_of(&c), None);
    }

    #[test]
    fn an_uncompressed_payload_over_the_limit_is_refused() {
        let mut c = Clipboard::new();
        c.content = vec![b'a'; MAX_CLIPBOARD + 1].into();
        assert_eq!(text_of(&c), None);
    }

    // -- the loop, which is the whole difficulty -----------------------------

    /// Writing a peer's clipboard onto the Mac marks the pasteboard modified,
    /// so the next poll reads it back. Without this rule that goes to the peer,
    /// whose own sync applies it and sends it back, for ever.
    #[test]
    fn a_peers_clipboard_is_not_immediately_sent_back_to_it() {
        let mut s = Sync::new();
        assert!(s.accept("hello".into()), "a new clipboard should be written");
        // The poll that follows our own write sees exactly what we just wrote.
        assert_eq!(s.offer("hello".into()), None, "that is the echo, not a new copy");
    }

    /// And the same in the other direction: what we sent must not come back.
    #[test]
    fn our_clipboard_is_not_applied_when_the_peer_echoes_it() {
        let mut s = Sync::new();
        assert_eq!(s.offer("from the mac".into()), Some("from the mac".to_owned()));
        assert!(!s.accept("from the mac".into()), "the peer is echoing us");
    }

    /// A genuine new copy still gets through, including one that restores an
    /// earlier value -- the rule is "same as the last thing", not a history.
    #[test]
    fn a_real_change_still_crosses_in_both_directions() {
        let mut s = Sync::new();
        assert!(s.accept("one".into()));
        assert_eq!(s.offer("two".into()), Some("two".to_owned()));
        assert!(s.accept("one".into()), "copying an earlier value again is a change");
        assert_eq!(s.offer("one".into()), None);
    }
}
