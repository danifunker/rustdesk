//! Capturing sound and turning it into `AudioFrame`s.
//!
//! Mirrors the agent side of upstream's `src/server/audio_service.rs`: the
//! default input device at 48 kHz stereo, Opus in restricted-low-delay mode,
//! 10 ms frames.
//!
//! # What a peer will actually hear
//!
//! **The default input device, not what the machine is playing.** Mac OS X has
//! no native capture of system output -- ScreenCaptureKit is 12.3+, twelve
//! years after this hardware -- and upstream's macOS path has the same
//! limitation. Someone who wants to hear the G5's own sound installs a loopback
//! driver, which then *becomes* the default input and needs no change here. On
//! a machine with nothing plugged into line-in, the honest outcome is silence,
//! and the zero gate below means silence costs no bandwidth.
//!
//! # Why the numbers are not adjustable
//!
//! Opus accepts 8, 12, 16, 24 or 48 kHz and nothing else, so the capture side
//! has to deliver one of them whatever the hardware would prefer -- the AUHAL
//! is asked to convert, and `Capture::open` reports what it actually got rather
//! than what it asked for, because a mismatch has to fail loudly rather than
//! encode garbage. The 10 ms frame is Opus's shortest that is not
//! latency-hostile to encode, and is upstream's choice.

/// Opus will not take anything else that is also worth using here.
pub const SAMPLE_RATE: u32 = 48_000;
pub const CHANNELS: usize = 2;
/// 10 ms, which is upstream's frame and a legal Opus duration.
pub const FRAME_MS: usize = 10;
/// Samples **per channel** in one frame: what Opus calls `frame_size`.
pub const FRAME_SAMPLES: usize = SAMPLE_RATE as usize / 1000 * FRAME_MS;
/// Interleaved floats in one frame.
pub const FRAME_FLOATS: usize = FRAME_SAMPLES * CHANNELS;
/// Opus never needs this much for 10 ms, but the API wants a bound.
#[cfg(target_os = "macos")]
const MAX_PACKET: usize = 4000;

/// How many silent frames before we stop sending, and the reason for a number
/// this large: upstream's comment puts 800 frames at a gate attack time of 3-8
/// seconds, which is long enough that a pause in speech does not clip the tail
/// off a word.
const MAX_ZERO_FRAMES: u32 = 800;

/// The part that has nothing to do with the machine: turn a stream of
/// interleaved samples into whole frames, and decide which are worth sending.
///
/// Split out so it is testable on the host, where there is no CoreAudio and no
/// libopus. Everything below this is FFI.
pub struct Framer {
    pending: Vec<f32>,
    zeros: u32,
}

impl Default for Framer {
    fn default() -> Self {
        Self::new()
    }
}

impl Framer {
    pub fn new() -> Self {
        Framer {
            pending: Vec::with_capacity(FRAME_FLOATS * 2),
            zeros: 0,
        }
    }

    /// Add captured samples, and call `emit` for each whole frame worth sending.
    ///
    /// Silence is dropped once `MAX_ZERO_FRAMES` of it have gone by, and the
    /// counter resets the moment anything audible arrives -- so the gate opens
    /// on the first non-zero sample rather than after a delay.
    pub fn push<F: FnMut(&[f32])>(&mut self, samples: &[f32], mut emit: F) {
        self.pending.extend_from_slice(samples);
        while self.pending.len() >= FRAME_FLOATS {
            let audible = self.pending[..FRAME_FLOATS].iter().any(|s| *s != 0.0);
            if audible {
                self.zeros = 0;
            } else {
                self.zeros = self.zeros.saturating_add(1);
            }
            if audible || self.zeros <= MAX_ZERO_FRAMES {
                emit(&self.pending[..FRAME_FLOATS]);
            }
            self.pending.drain(..FRAME_FLOATS);
        }
    }

    /// Samples held back because they do not yet make a whole frame.
    pub fn pending(&self) -> usize {
        self.pending.len()
    }

    /// Consecutive silent frames seen. Only interesting to the probe.
    pub fn silent_frames(&self) -> u32 {
        self.zeros
    }
}

// --- the machine half ------------------------------------------------------

#[cfg(target_os = "macos")]
#[link(name = "CoreAudio", kind = "framework")]
#[link(name = "AudioUnit", kind = "framework")]
#[link(name = "AudioToolbox", kind = "framework")]
extern "C" {
    fn rd_audio_open(
        want_rate: std::os::raw::c_int,
        want_channels: std::os::raw::c_int,
        out_rate: *mut std::os::raw::c_int,
        out_channels: *mut std::os::raw::c_int,
    ) -> std::os::raw::c_int;
    fn rd_audio_read(out: *mut f32, frames: std::os::raw::c_int) -> std::os::raw::c_int;
    fn rd_audio_overruns() -> std::os::raw::c_int;
    fn rd_audio_stats(
        callbacks: *mut std::os::raw::c_int,
        render_errors: *mut std::os::raw::c_int,
        last_err: *mut std::os::raw::c_int,
    );
    fn rd_audio_hw_format(
        rate: *mut std::os::raw::c_int,
        channels: *mut std::os::raw::c_int,
    ) -> std::os::raw::c_int;
    fn rd_audio_close();

    fn rd_opus_new(rate: std::os::raw::c_int, channels: std::os::raw::c_int) -> *mut std::ffi::c_void;
    fn rd_opus_encode(
        enc: *mut std::ffi::c_void,
        pcm: *const f32,
        frame_size: std::os::raw::c_int,
        out: *mut u8,
        max_out: std::os::raw::c_int,
    ) -> std::os::raw::c_int;
    fn rd_opus_free(enc: *mut std::ffi::c_void);

    fn rd_opus_decoder_new(
        rate: std::os::raw::c_int,
        channels: std::os::raw::c_int,
    ) -> *mut std::ffi::c_void;
    fn rd_opus_decode(
        dec: *mut std::ffi::c_void,
        data: *const u8,
        len: std::os::raw::c_int,
        pcm: *mut f32,
        frame_size: std::os::raw::c_int,
    ) -> std::os::raw::c_int;
    fn rd_opus_decoder_free(dec: *mut std::ffi::c_void);
}

/// Why capture could not be opened, in the words of the stage that failed.
///
/// Worth the enum rather than a bare error: "no audio" is the same message for
/// a machine with nothing plugged in and a machine where the unit would not
/// take our format, and those want different responses from whoever reads the
/// log.
#[cfg(target_os = "macos")]
fn open_error(code: i32) -> String {
    match code {
        -1 => "no HAL output component -- this build cannot reach CoreAudio".to_owned(),
        -2 => "the audio unit would not open or configure".to_owned(),
        -3 => "no default input device; nothing is plugged in, or none is selected".to_owned(),
        -4 => "the unit would not give us 48 kHz stereo float".to_owned(),
        -5 => "the input callback or initialise failed".to_owned(),
        -6 => "the unit would not start".to_owned(),
        n => format!("unknown failure {}", n),
    }
}

#[cfg(target_os = "macos")]
pub struct Capture {
    pub rate: u32,
    pub channels: usize,
}

#[cfg(target_os = "macos")]
impl Capture {
    /// Open the default input device.
    ///
    /// Fails rather than adapting if the negotiated format is not one Opus
    /// takes: encoding at the wrong rate produces sound that is confidently
    /// the wrong pitch, which is worse than none.
    pub fn open() -> Result<Self, String> {
        let (mut rate, mut channels) = (0, 0);
        let rc = unsafe { rd_audio_open(SAMPLE_RATE as _, CHANNELS as _, &mut rate, &mut channels) };
        if rc != 0 {
            return Err(open_error(rc));
        }
        if rate as u32 != SAMPLE_RATE || channels as usize != CHANNELS {
            unsafe { rd_audio_close() };
            return Err(format!(
                "got {} Hz / {} channels, and only {} Hz / {} is usable here",
                rate, channels, SAMPLE_RATE, CHANNELS
            ));
        }
        Ok(Capture {
            rate: rate as u32,
            channels: channels as usize,
        })
    }

    /// Read whatever has arrived. Short reads are normal and mean "nothing yet".
    pub fn read(&mut self, out: &mut [f32]) -> usize {
        let frames = out.len() / self.channels;
        if frames == 0 {
            return 0;
        }
        let got = unsafe { rd_audio_read(out.as_mut_ptr(), frames as _) };
        (got.max(0) as usize) * self.channels
    }

    /// Times the reader fell a whole ring behind. Nonzero means the session
    /// loop is not draining often enough, which is a real fault rather than a
    /// statistic.
    pub fn overruns(&self) -> u32 {
        unsafe { rd_audio_overruns().max(0) as u32 }
    }

    /// Callbacks seen, renders that failed, and the last OSStatus.
    ///
    /// The distinction that matters: no callbacks at all means the unit is not
    /// running, whereas callbacks with failing renders means it is running and
    /// the format is wrong. Those want opposite investigations, and without
    /// this both look like "no audio".
    pub fn stats(&self) -> (u32, u32, i32) {
        let (mut cb, mut errs, mut last) = (0, 0, 0);
        unsafe { rd_audio_stats(&mut cb, &mut errs, &mut last) };
        (cb.max(0) as u32, errs.max(0) as u32, last)
    }

    /// The format on the hardware side of the input element, which is not
    /// necessarily the one we were given.
    pub fn hw_format(&self) -> Option<(u32, u32)> {
        let (mut rate, mut ch) = (0, 0);
        if unsafe { rd_audio_hw_format(&mut rate, &mut ch) } != 0 {
            return None;
        }
        Some((rate.max(0) as u32, ch.max(0) as u32))
    }
}

#[cfg(target_os = "macos")]
impl Drop for Capture {
    fn drop(&mut self) {
        unsafe { rd_audio_close() };
    }
}

#[cfg(target_os = "macos")]
pub struct Encoder {
    enc: *mut std::ffi::c_void,
    buf: Vec<u8>,
}

#[cfg(target_os = "macos")]
impl Encoder {
    pub fn new() -> Result<Self, String> {
        let enc = unsafe { rd_opus_new(SAMPLE_RATE as _, CHANNELS as _) };
        if enc.is_null() {
            return Err("opus_encoder_create failed".to_owned());
        }
        Ok(Encoder {
            enc,
            buf: vec![0u8; MAX_PACKET],
        })
    }

    /// Encode one frame. `pcm` must be exactly `FRAME_FLOATS` interleaved
    /// samples; Opus is told the per-channel count, which is half that.
    pub fn encode(&mut self, pcm: &[f32]) -> Result<&[u8], String> {
        if pcm.len() != FRAME_FLOATS {
            return Err(format!("frame is {} floats, want {}", pcm.len(), FRAME_FLOATS));
        }
        let n = unsafe {
            rd_opus_encode(
                self.enc,
                pcm.as_ptr(),
                FRAME_SAMPLES as _,
                self.buf.as_mut_ptr(),
                self.buf.len() as _,
            )
        };
        if n < 0 {
            return Err("opus_encode_float failed".to_owned());
        }
        Ok(&self.buf[..n as usize])
    }
}

#[cfg(target_os = "macos")]
impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe { rd_opus_free(self.enc) };
        self.enc = std::ptr::null_mut();
    }
}

/// Decoding exists only so the agent can check its own output on the machine
/// that will be sending it -- the peer is what decodes in a real session. Same
/// idea as `--probe-display` checking the converter against a reference.
#[cfg(target_os = "macos")]
pub struct Decoder {
    dec: *mut std::ffi::c_void,
}

#[cfg(target_os = "macos")]
impl Decoder {
    pub fn new() -> Result<Self, String> {
        let dec = unsafe { rd_opus_decoder_new(SAMPLE_RATE as _, CHANNELS as _) };
        if dec.is_null() {
            return Err("opus_decoder_create failed".to_owned());
        }
        Ok(Decoder { dec })
    }

    /// Samples per channel written into `pcm`, which must hold `FRAME_FLOATS`.
    pub fn decode(&mut self, packet: &[u8], pcm: &mut [f32]) -> Result<usize, String> {
        if pcm.len() < FRAME_FLOATS {
            return Err("decode buffer too small".to_owned());
        }
        let n = unsafe {
            rd_opus_decode(
                self.dec,
                packet.as_ptr(),
                packet.len() as _,
                pcm.as_mut_ptr(),
                FRAME_SAMPLES as _,
            )
        };
        if n < 0 {
            return Err("opus_decode_float failed".to_owned());
        }
        Ok(n as usize)
    }
}

#[cfg(target_os = "macos")]
impl Drop for Decoder {
    fn drop(&mut self) {
        unsafe { rd_opus_decoder_free(self.dec) };
        self.dec = std::ptr::null_mut();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_frame_arithmetic_is_what_opus_expects() {
        // 480 samples per channel is 10 ms at 48 kHz, and one of the durations
        // Opus will accept. 960 interleaved floats is upstream's buffer size.
        assert_eq!(FRAME_SAMPLES, 480);
        assert_eq!(FRAME_FLOATS, 960);
        assert_eq!(FRAME_SAMPLES * 1000 / SAMPLE_RATE as usize, FRAME_MS);
    }

    #[test]
    fn samples_are_gathered_into_whole_frames() {
        let mut f = Framer::new();
        let mut frames = 0;
        // Deliberately not a multiple of the frame: capture returns whatever
        // has arrived, which is never conveniently aligned.
        for _ in 0..10 {
            f.push(&vec![0.5f32; 700], |frame| {
                assert_eq!(frame.len(), FRAME_FLOATS);
                frames += 1;
            });
        }
        assert_eq!(frames, 7000 / FRAME_FLOATS);
        assert_eq!(f.pending(), 7000 % FRAME_FLOATS);
    }

    #[test]
    fn a_partial_frame_is_never_emitted() {
        let mut f = Framer::new();
        let mut called = false;
        f.push(&vec![1.0f32; FRAME_FLOATS - 1], |_| called = true);
        assert!(!called, "959 floats is not a frame");
        assert_eq!(f.pending(), FRAME_FLOATS - 1);
        f.push(&[1.0], |frame| {
            assert_eq!(frame.len(), FRAME_FLOATS);
            called = true;
        });
        assert!(called, "the 960th float completes it");
        assert_eq!(f.pending(), 0);
    }

    /// Silence should stop costing bandwidth, but only after long enough that a
    /// pause in speech is not clipped.
    #[test]
    fn the_gate_closes_on_sustained_silence_and_opens_at_once() {
        let mut f = Framer::new();
        let silence = vec![0.0f32; FRAME_FLOATS];

        let mut sent = 0;
        for _ in 0..MAX_ZERO_FRAMES {
            f.push(&silence, |_| sent += 1);
        }
        assert_eq!(sent, MAX_ZERO_FRAMES, "silence still flows below the gate");

        let mut after = 0;
        for _ in 0..50 {
            f.push(&silence, |_| after += 1);
        }
        assert_eq!(after, 0, "and stops once the gate closes");

        // One audible sample must reopen it immediately, not after a delay.
        let mut noise = vec![0.0f32; FRAME_FLOATS];
        noise[FRAME_FLOATS - 1] = 0.01;
        let mut reopened = 0;
        f.push(&noise, |_| reopened += 1);
        assert_eq!(reopened, 1, "the gate opens on the first audible frame");
        assert_eq!(f.silent_frames(), 0);
    }
}
