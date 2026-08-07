// Capturing the default audio input with an AUHAL unit, on Mac OS X 10.4/10.5.
//
// # What this can and cannot hear
//
// **This captures the default *input* device, not what the machine is playing.**
// Mac OS X has no native way to capture system output -- ScreenCaptureKit, which
// is how modern RustDesk does it, is 12.3+. Upstream's macOS path has the same
// limitation and the same answer: it opens the default input device, and a user
// who wants system sound installs a loopback driver (Soundflower on hardware
// this old) which then *is* the default input. So the honest description is
// "whatever the machine is listening to", and that is all any Mac could offer
// until a decade after this one shipped.
//
// # Why the Component Manager and not AudioComponentFindNext
//
// `AudioComponentFindNext` / `AudioComponentInstanceNew` are 10.6. On 10.5 the
// only way to a HAL output unit is the Component Manager -- `FindNextComponent`,
// `OpenAComponent`, `CloseComponent`. Every example written after 2009 uses the
// newer pair, which is why this looks unfamiliar; it is not an older style, it
// is the only one that links here.
//
// # The ring
//
// CoreAudio calls back on its own real-time thread and must not be made to wait,
// so the callback only copies. Single producer, single consumer, `volatile`
// indices, and no lock: the consumer is the session loop, which reads whole
// 10 ms frames. On overrun the *consumer* resynchronises rather than the
// producer -- if both moved `read` there would be a genuine race, and the
// producer is the one that cannot afford to check.

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <string.h>

/// One second of stereo at 48 kHz. Generous on purpose: the session loop can be
/// away for a whole video frame, and dropping audio to save memory on a machine
/// with 4 GB would be a poor trade.
#define RD_RING_FRAMES 48000
#define RD_MAX_CHANNELS 2
/// Bigger than any `MaximumFramesPerSlice` we ask for, so the render never
/// overruns the scratch buffer.
#define RD_SCRATCH_FRAMES 8192

static AudioUnit g_unit = 0;
static int g_open = 0;
static int g_channels = 2;
static double g_rate = 48000.0;

static float g_ring[RD_RING_FRAMES * RD_MAX_CHANNELS];
static volatile unsigned long g_write; /* frames produced, monotonic */
static volatile unsigned long g_read;  /* frames consumed, monotonic */
static volatile unsigned long g_overruns;

/* Diagnostics. Without these "no audio" is one message for two very different
   faults -- a callback that never fires, and a render that fails every time --
   and they want opposite investigations. The first version of this shim
   swallowed the render error and produced exactly that ambiguity. */
static volatile unsigned long g_callbacks;
static volatile unsigned long g_render_errors;
static volatile int g_last_err;

static float g_scratch[RD_SCRATCH_FRAMES * RD_MAX_CHANNELS];

static OSStatus rd_input_cb(void *inRefCon,
                            AudioUnitRenderActionFlags *ioActionFlags,
                            const AudioTimeStamp *inTimeStamp,
                            UInt32 inBusNumber,
                            UInt32 inNumberFrames,
                            AudioBufferList *ioData) {
    AudioBufferList abl;
    OSStatus err;
    UInt32 i;
    unsigned long w;
    (void)inRefCon;
    (void)ioData;

    g_callbacks++;
    if (!g_unit || inNumberFrames == 0 || inNumberFrames > RD_SCRATCH_FRAMES) {
        return noErr;
    }

    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = (UInt32)g_channels;
    abl.mBuffers[0].mDataByteSize = inNumberFrames * g_channels * sizeof(float);
    abl.mBuffers[0].mData = g_scratch;

    err = AudioUnitRender(g_unit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, &abl);
    if (err != noErr) {
        g_render_errors++;
        g_last_err = (int)err;
        return noErr; /* a dropped slice is not worth failing the unit for */
    }

    w = g_write;
    for (i = 0; i < inNumberFrames; i++) {
        unsigned long slot = (w + i) % RD_RING_FRAMES;
        int c;
        for (c = 0; c < g_channels; c++) {
            g_ring[slot * RD_MAX_CHANNELS + c] = g_scratch[i * g_channels + c];
        }
    }
    g_write = w + inNumberFrames;
    return noErr;
}

static void rd_reset_ring(void) {
    g_write = 0;
    g_read = 0;
    g_overruns = 0;
    g_callbacks = 0;
    g_render_errors = 0;
    g_last_err = 0;
}

/// How the capture is actually behaving, as opposed to whether it opened.
void rd_audio_stats(int *callbacks, int *render_errors, int *last_err) {
    if (callbacks) *callbacks = (int)g_callbacks;
    if (render_errors) *render_errors = (int)g_render_errors;
    if (last_err) *last_err = g_last_err;
}

/// The format on the *hardware* side of the input element, which is not the one
/// we asked for and is the usual reason a render fails: the AUHAL will not
/// always convert, and the device's own rate wins.
int rd_audio_hw_format(int *rate, int *channels) {
    AudioStreamBasicDescription hw;
    UInt32 size = sizeof(hw);
    if (!g_unit) return -1;
    if (AudioUnitGetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1,
                             &hw, &size) != noErr) {
        return -1;
    }
    if (rate) *rate = (int)(hw.mSampleRate + 0.5);
    if (channels) *channels = (int)hw.mChannelsPerFrame;
    return 0;
}

/// Open the default input device.
///
/// Returns 0 on success, and writes the format actually negotiated into
/// `out_rate` / `out_channels` -- which may not be what was asked for, and the
/// caller has to care: Opus accepts only 8/12/16/24/48 kHz.
///
/// Negative returns are the stage that failed, so a log line says where rather
/// than just "no audio":
///   -1 no HAL output component      -4 could not set the client format
///   -2 could not open//configure it  -5 callback or initialise failed
///   -3 no default input device       -6 could not start
int rd_audio_open(int want_rate, int want_channels, int *out_rate, int *out_channels) {
    ComponentDescription desc;
    Component comp;
    AudioDeviceID dev = kAudioDeviceUnknown;
    AudioStreamBasicDescription asbd;
    AURenderCallbackStruct cb;
    UInt32 size, enable, disable, slice;
    OSStatus err;

    if (g_open) {
        return 0;
    }
    if (want_channels < 1 || want_channels > RD_MAX_CHANNELS) {
        want_channels = 2;
    }

    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;
    comp = FindNextComponent(NULL, &desc);
    if (!comp) {
        return -1;
    }
    if (OpenAComponent(comp, &g_unit) != noErr || !g_unit) {
        g_unit = 0;
        return -2;
    }

    /* Element 1 is input, element 0 is output. We want the first and not the
       second, and the unit will not open the device without being told. */
    enable = 1;
    disable = 0;
    if (AudioUnitSetProperty(g_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                             &enable, sizeof(enable)) != noErr ||
        AudioUnitSetProperty(g_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,
                             &disable, sizeof(disable)) != noErr) {
        rd_audio_close();
        return -2;
    }

    size = sizeof(dev);
    /* AudioHardwareGetProperty, not AudioObjectGetPropertyData: the latter's
       10.5 form works but this is the call that is documented for this OS. */
    if (AudioHardwareGetProperty(kAudioHardwarePropertyDefaultInputDevice, &size, &dev) != noErr ||
        dev == kAudioDeviceUnknown) {
        rd_audio_close();
        return -3;
    }
    if (AudioUnitSetProperty(g_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
                             0, &dev, sizeof(dev)) != noErr) {
        rd_audio_close();
        return -3;
    }

    /* Ask the device itself for 48 kHz before asking the unit for it.
       On 10.5 the AUHAL input element will do channel and sample-format
       conversion but is not reliable about *rate*, so a device sitting at
       44.1 kHz makes every AudioUnitRender fail rather than resampling. The
       PCM3052 in this machine offers 48 kHz natively, and a device-wide rate
       change is what every capture application of the era did.
       Best effort: if it refuses, the format negotiation below reports it. */
    {
        AudioStreamBasicDescription hw;
        UInt32 hwsize = sizeof(hw);
        if (AudioUnitGetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1,
                                 &hw, &hwsize) == noErr &&
            (int)(hw.mSampleRate + 0.5) != want_rate) {
            Float64 want = (Float64)want_rate;
            AudioDeviceSetProperty(dev, NULL, 0, 1 /* isInput */, kAudioDevicePropertyNominalSampleRate,
                                   sizeof(want), &want);
        }
    }

    /* The format we want the unit to hand *us*, on the output side of the input
       element. The AUHAL converts from the hardware format where it can, which
       is what lets us insist on a rate Opus will accept. */
    memset(&asbd, 0, sizeof(asbd));
    asbd.mSampleRate = (Float64)want_rate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mFramesPerPacket = 1;
    asbd.mChannelsPerFrame = (UInt32)want_channels;
    asbd.mBitsPerChannel = 32;
    asbd.mBytesPerFrame = (UInt32)want_channels * sizeof(float);
    asbd.mBytesPerPacket = asbd.mBytesPerFrame;
    err = AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
                               &asbd, sizeof(asbd));
    if (err != noErr) {
        rd_audio_close();
        return -4;
    }
    /* Read it back rather than trusting the set: the unit is entitled to have
       given us something else, and the caller has to know the truth to tell the
       peer and to configure the encoder. */
    size = sizeof(asbd);
    if (AudioUnitGetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
                             &asbd, &size) != noErr) {
        rd_audio_close();
        return -4;
    }
    g_rate = asbd.mSampleRate;
    g_channels = (int)asbd.mChannelsPerFrame;
    if (g_channels < 1 || g_channels > RD_MAX_CHANNELS) {
        rd_audio_close();
        return -4;
    }

    slice = RD_SCRATCH_FRAMES;
    AudioUnitSetProperty(g_unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                         0, &slice, sizeof(slice));

    cb.inputProc = rd_input_cb;
    cb.inputProcRefCon = 0;
    if (AudioUnitSetProperty(g_unit, kAudioOutputUnitProperty_SetInputCallback,
                             kAudioUnitScope_Global, 0, &cb, sizeof(cb)) != noErr) {
        rd_audio_close();
        return -5;
    }

    rd_reset_ring();
    if (AudioUnitInitialize(g_unit) != noErr) {
        rd_audio_close();
        return -5;
    }
    if (AudioOutputUnitStart(g_unit) != noErr) {
        rd_audio_close();
        return -6;
    }

    g_open = 1;
    if (out_rate) {
        *out_rate = (int)(g_rate + 0.5);
    }
    if (out_channels) {
        *out_channels = g_channels;
    }
    return 0;
}

/// Frames (per channel) actually written to `out`, which is interleaved and must
/// hold `frames * channels` floats. Short reads are normal: it returns what has
/// arrived rather than waiting.
int rd_audio_read(float *out, int frames) {
    unsigned long w, r, avail;
    int i, c;

    if (!g_open || !out || frames <= 0) {
        return 0;
    }
    w = g_write;
    r = g_read;
    if (w <= r) {
        return 0;
    }
    avail = w - r;
    /* Overrun: the producer has lapped us. Resynchronise here rather than in the
       callback -- see the header. Everything older than one ring is gone. */
    if (avail > RD_RING_FRAMES) {
        g_overruns++;
        r = w - RD_RING_FRAMES;
        avail = RD_RING_FRAMES;
    }
    if ((unsigned long)frames > avail) {
        frames = (int)avail;
    }
    for (i = 0; i < frames; i++) {
        unsigned long slot = (r + i) % RD_RING_FRAMES;
        for (c = 0; c < g_channels; c++) {
            out[i * g_channels + c] = g_ring[slot * RD_MAX_CHANNELS + c];
        }
    }
    g_read = r + frames;
    return frames;
}

/// How many times the reader fell a whole ring behind. Nonzero means the session
/// loop is not draining audio often enough.
int rd_audio_overruns(void) { return (int)g_overruns; }

void rd_audio_close(void) {
    if (g_unit) {
        AudioOutputUnitStop(g_unit);
        AudioUnitUninitialize(g_unit);
        CloseComponent(g_unit);
        g_unit = 0;
    }
    g_open = 0;
    rd_reset_ring();
}
