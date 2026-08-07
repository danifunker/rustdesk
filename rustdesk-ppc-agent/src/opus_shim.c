// Opus encoding for the audio path, and decoding for the self-check.
//
// In C rather than through a crate for the reason the whole dependency list is
// short: `magnum-opus` would be the idiomatic choice, but it carries a bindgen
// build step, and every build-time crate has to be transpiled for the host by
// mrustc before a line of the agent compiles. The API actually used here is
// four functions.
//
// The headers are vendored under opus-include/ rather than referenced on the
// Mac, for the same reason vpx's are: ppc-cc-remote.py mirrors a local -I
// directory to the target, but passes paths under a *system* prefix through
// untouched, and ~/ppc-libs is neither.
//
// Everything is void* across the boundary so the Rust side needs no layout
// knowledge of OpusEncoder, which is opaque in the C API anyway.

#include <opus/opus.h>
#include <stdlib.h>
#include <string.h>

// Upstream's choice, and it is the one that matters for a remote desktop:
// RESTRICTED_LOWDELAY drops the look-ahead that VOIP and AUDIO modes use, which
// is latency the interactive path cannot spend.
#define RD_OPUS_APPLICATION OPUS_APPLICATION_RESTRICTED_LOWDELAY

/// NULL on failure. `sample_rate` must be one of 8000/12000/16000/24000/48000 --
/// Opus accepts nothing else, which is why the capture side has to deliver one
/// of them rather than whatever the hardware prefers.
void *rd_opus_new(int sample_rate, int channels) {
    int err = OPUS_OK;
    OpusEncoder *enc = opus_encoder_create(sample_rate, channels, RD_OPUS_APPLICATION, &err);
    if (err != OPUS_OK) {
        return 0;
    }
    return (void *)enc;
}

/// Bytes written to `out`, or -1. `frame_size` is samples **per channel**, so a
/// 10 ms stereo frame at 48 kHz is 480 here and 960 floats in `pcm`.
int rd_opus_encode(void *enc, const float *pcm, int frame_size, unsigned char *out, int max_out) {
    int n;
    if (!enc || !pcm || !out) {
        return -1;
    }
    n = opus_encode_float((OpusEncoder *)enc, pcm, frame_size, out, max_out);
    return n < 0 ? -1 : n;
}

void rd_opus_free(void *enc) {
    if (enc) {
        opus_encoder_destroy((OpusEncoder *)enc);
    }
}

// --- the decoder half, which exists only so the agent can check itself -------
//
// Nothing in the session path decodes: the peer does that. This is here so
// `--probe-audio` can put a known signal through the encoder and satisfy itself
// that what came out is a real Opus packet of the right length, on the machine
// that will actually be sending them. The same shape as `--probe-display`
// checking the converter against the Rust reference.

void *rd_opus_decoder_new(int sample_rate, int channels) {
    int err = OPUS_OK;
    OpusDecoder *dec = opus_decoder_create(sample_rate, channels, &err);
    if (err != OPUS_OK) {
        return 0;
    }
    return (void *)dec;
}

/// Samples **per channel** written to `pcm`, or -1.
int rd_opus_decode(void *dec, const unsigned char *data, int len, float *pcm, int frame_size) {
    int n;
    if (!dec || !data || !pcm) {
        return -1;
    }
    n = opus_decode_float((OpusDecoder *)dec, data, len, pcm, frame_size, 0);
    return n < 0 ? -1 : n;
}

void rd_opus_decoder_free(void *dec) {
    if (dec) {
        opus_decoder_destroy((OpusDecoder *)dec);
    }
}
