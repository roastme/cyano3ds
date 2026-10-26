#!/usr/bin/env python3
# fix-cm6-audio.py - give CyanogenMod 6 (Froyo) real sound on the Nintendo 3DS.
#
# Froyo has no ALSA audio HAL (hardware/alsa_sound is a Gingerbread project),
# and the generic HAL (AudioHardwareGeneric) talks to a /dev/eac device that
# does not exist here, so AudioHardwareInterface::create() falls back to
# AudioHardwareStub and logs
#
#     AudioHardwareInterface: Using stubbed audio hardware. No sound will be produced.
#
# The port's kernel driver (ctr_snd.c) exposes one ordinary ALSA playback PCM,
# /dev/snd/pcmC0D0p (S16_LE stereo, CSND on the I2S2 line; the codec is set up
# by the kernel).  This patch turns AudioHardwareGeneric's output stream into a
# small ALSA client that drives that PCM through the raw kernel ioctls (the
# same ones port/userland/tests/pcmtest.c uses, so no alsa-lib is needed):
#
#   * initCheck() succeeds when /dev/snd/pcmC0D0p exists;
#   * the first write() opens the PCM, sets 44.1 kHz S16_LE stereo and writes
#     with SNDRV_PCM_IOCTL_WRITEI_FRAMES (underruns are re-prepared);
#   * standby() closes the PCM so the codec/I2S clock is not kept running;
#   * the input stream hands out timed silence (no mic path in Froyo yet).
#
# Build with BOARD_USES_GENERIC_AUDIO := true (device-cm6/ctr/BoardConfig.mk).
#
# Usage: python3 port/scripts/fix-cm6-audio.py [CM6_DIR]
# Idempotent: safe to run repeatedly.

import os
import pathlib
import sys

CM6_DIR = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                       else os.path.expanduser("~/p3ds/cm6/src"))
D = CM6_DIR / "frameworks" / "base" / "libs" / "audioflinger"
CPP = D / "AudioHardwareGeneric.cpp"
HDR = D / "AudioHardwareGeneric.h"
MARK = "[3ds]"

PCM_CODE = r'''
// ----------------------------------------------------------------------------
// %(mark)s ALSA playback through the raw kernel ioctls (no alsa-lib in Froyo).
// Same minimal uapi definitions as port/userland/tests/pcmtest.c; the layouts
// match the 32-bit ARM kernel's <sound/asound.h>.

struct ctr_snd_mask { uint32_t bits[8]; };
struct ctr_snd_interval {
    uint32_t min, max;
    unsigned int openmin:1, openmax:1, integer:1, empty:1;
};
struct ctr_snd_pcm_hw_params {
    uint32_t flags;
    struct ctr_snd_mask masks[3];
    struct ctr_snd_mask mres[5];
    struct ctr_snd_interval intervals[12];
    struct ctr_snd_interval ires[9];
    uint32_t rmask, cmask, info, msbits, rate_num, rate_den;
    unsigned long fifo_size;
    unsigned char reserved[64];
};
struct ctr_snd_pcm_sw_params {
    int tstamp_mode;
    unsigned int period_step;
    unsigned int sleep_min;
    unsigned long avail_min;
    unsigned long xfer_align;
    unsigned long start_threshold;
    unsigned long stop_threshold;
    unsigned long silence_threshold;
    unsigned long silence_size;
    unsigned long boundary;
    unsigned int proto;
    unsigned int tstamp_type;
    unsigned char reserved[56];
};
struct ctr_snd_xferi {
    long result;
    void *buf;
    unsigned long frames;
};

#define CTR_PCM_HW_PARAMS     _IOWR('A', 0x11, struct ctr_snd_pcm_hw_params)
#define CTR_PCM_SW_PARAMS     _IOWR('A', 0x13, struct ctr_snd_pcm_sw_params)
#define CTR_PCM_PREPARE       _IO('A', 0x40)
#define CTR_PCM_DRAIN         _IO('A', 0x44)
#define CTR_PCM_DROP          _IO('A', 0x43)
#define CTR_PCM_WRITEI_FRAMES _IOW('A', 0x50, struct ctr_snd_xferi)

// parameter indexes (SNDRV_PCM_HW_PARAM_*)
#define CTR_P_ACCESS       0
#define CTR_P_FORMAT       1
#define CTR_P_SUBFORMAT    2
#define CTR_P_FIRST_MASK   0
#define CTR_P_SAMPLE_BITS  8
#define CTR_P_FRAME_BITS   9
#define CTR_P_CHANNELS     10
#define CTR_P_RATE         11
#define CTR_P_PERIOD_SIZE  13
#define CTR_P_PERIODS      15
#define CTR_P_BUFFER_SIZE  17
#define CTR_P_FIRST_IVAL   8
#define CTR_ACCESS_RW_INTERLEAVED 3
#define CTR_FORMAT_S16_LE  2

static const char *kPcmPlayback = "/dev/snd/pcmC0D0p";
static const unsigned kPcmRate = 44100;
static const unsigned kPcmPeriod = 882;     // 20 ms, as pcmtest.c

static void ctrPcmInit(struct ctr_snd_pcm_hw_params *p)
{
    int n;
    memset(p, 0, sizeof(*p));
    for (n = 0; n < 3; n++) {
        p->masks[n].bits[0] = ~0u;
        p->masks[n].bits[1] = ~0u;
    }
    for (n = 0; n < 12; n++) {
        p->intervals[n].min = 0;
        p->intervals[n].max = ~0u;
    }
    p->rmask = ~0u;
    p->info = ~0u;
}

static void ctrPcmSetMask(struct ctr_snd_pcm_hw_params *p, int n, unsigned bit)
{
    struct ctr_snd_mask *m = &p->masks[n - CTR_P_FIRST_MASK];
    memset(m, 0, sizeof(*m));
    m->bits[bit >> 5] = 1u << (bit & 31);
}

static void ctrPcmSetInt(struct ctr_snd_pcm_hw_params *p, int n, unsigned v)
{
    struct ctr_snd_interval *i = &p->intervals[n - CTR_P_FIRST_IVAL];
    i->min = v;
    i->max = v;
    i->integer = 1;
}

// Open /dev/snd/pcmC0D0p non-blocking (ALSA's blocking open() waits for a busy
// substream), then program 44.1 kHz S16_LE stereo.  Returns the fd or -1.
static int ctrPcmOpen()
{
    struct ctr_snd_pcm_hw_params hw;
    struct ctr_snd_pcm_sw_params sw;
    unsigned period = kPcmPeriod, buffer;
    int fd = ::open(kPcmPlayback, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        LOGE("open(%%s): %%s", kPcmPlayback, strerror(errno));
        return -1;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);

    ctrPcmInit(&hw);
    ctrPcmSetMask(&hw, CTR_P_ACCESS, CTR_ACCESS_RW_INTERLEAVED);
    ctrPcmSetMask(&hw, CTR_P_FORMAT, CTR_FORMAT_S16_LE);
    ctrPcmSetMask(&hw, CTR_P_SUBFORMAT, 0);
    ctrPcmSetInt(&hw, CTR_P_SAMPLE_BITS, 16);
    ctrPcmSetInt(&hw, CTR_P_FRAME_BITS, 32);
    ctrPcmSetInt(&hw, CTR_P_CHANNELS, 2);
    ctrPcmSetInt(&hw, CTR_P_RATE, kPcmRate);
    ctrPcmSetInt(&hw, CTR_P_PERIOD_SIZE, period);
    ctrPcmSetInt(&hw, CTR_P_PERIODS, 4);
    if (::ioctl(fd, CTR_PCM_HW_PARAMS, &hw) < 0) {
        LOGE("PCM HW_PARAMS failed: %%s", strerror(errno));
        ::close(fd);
        return -1;
    }
    period = hw.intervals[CTR_P_PERIOD_SIZE - CTR_P_FIRST_IVAL].min;
    buffer = hw.intervals[CTR_P_BUFFER_SIZE - CTR_P_FIRST_IVAL].min;

    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = 0;
    sw.period_step = 1;
    sw.avail_min = period;
    sw.start_threshold = period;
    sw.stop_threshold = buffer;
    sw.boundary = buffer;
    while (sw.boundary * 2 <= 0x7fffffffu - buffer)
        sw.boundary *= 2;
    if (::ioctl(fd, CTR_PCM_SW_PARAMS, &sw) < 0 ||
        ::ioctl(fd, CTR_PCM_PREPARE) < 0) {
        LOGE("PCM SW_PARAMS/PREPARE failed: %%s", strerror(errno));
        ::close(fd);
        return -1;
    }
    LOGI("PCM open: %%u Hz, period %%u, buffer %%u frames", kPcmRate, period, buffer);
    return fd;
}
''' % {"mark": MARK}


def sub(s, old, new, what):
    if old not in s:
        print("cannot find anchor for %s in %s" % (what, CPP), file=sys.stderr)
        sys.exit(1)
    return s.replace(old, new, 1)


def main():
    if not CPP.is_file():
        print("no %s" % CPP, file=sys.stderr)
        return 1
    s = CPP.read_text()
    if MARK + " ALSA playback" in s:
        print("fix-cm6-audio: already applied")
        return 0

    # includes + the raw ALSA client, right after the existing includes
    s = sub(s, '#include "AudioHardwareGeneric.h"\n#include <media/AudioRecord.h>\n',
            '#include "AudioHardwareGeneric.h"\n#include <media/AudioRecord.h>\n'
            '#include <errno.h>\n#include <string.h>\n#include <sys/ioctl.h>\n'
            '#include <linux/ioctl.h>\n', "includes")
    s = sub(s, "// ----------------------------------------------------------------------------\n\nAudioHardwareGeneric::AudioHardwareGeneric()",
            PCM_CODE + "\n// ----------------------------------------------------------------------------\n\nAudioHardwareGeneric::AudioHardwareGeneric()",
            "constructor")

    # the hardware object does not open /dev/eac
    s = sub(s, "    mFd = ::open(kAudioDeviceName, O_RDWR);\n",
            "    mFd = -1;   // %s streams open the ALSA PCM themselves\n" % MARK, "ctor body")
    s = sub(s, """    if (mFd >= 0) {
        if (::access(kAudioDeviceName, O_RDWR) == NO_ERROR)
            return NO_ERROR;
    }
    return NO_INIT;""",
            """    // %s usable when the kernel's ALSA playback node exists
    if (::access(kPcmPlayback, R_OK | W_OK) == 0)
        return NO_ERROR;
    return NO_INIT;""" % MARK, "initCheck")

    # output stream: lazy open, write, standby
    s = sub(s, """    mAudioHardware = hw;
    mFd = fd;
    mDevice = devices;
    return NO_ERROR;
}

AudioStreamOutGeneric::~AudioStreamOutGeneric()
{
}

ssize_t AudioStreamOutGeneric::write(const void* buffer, size_t bytes)
{
    Mutex::Autolock _l(mLock);
    return ssize_t(::write(mFd, buffer, bytes));
}

status_t AudioStreamOutGeneric::standby()
{
    // Implement: audio hardware to standby mode
    return NO_ERROR;
}
""", """    mAudioHardware = hw;
    mFd = -1;   // %(m)s the PCM is opened on the first write()
    mDevice = devices;
    return NO_ERROR;
}

AudioStreamOutGeneric::~AudioStreamOutGeneric()
{
    standby();
}

ssize_t AudioStreamOutGeneric::write(const void* buffer, size_t bytes)
{
    Mutex::Autolock _l(mLock);
    if (mFd < 0) {
        mFd = ctrPcmOpen();
        if (mFd < 0) {
            // no usable PCM: behave like the stub (consume at the stream rate)
            usleep(((bytes / 4) * 1000000ULL) / sampleRate());
            return bytes;
        }
    }
    const char *p = (const char *)buffer;
    size_t left = bytes;
    while (left >= 4) {
        struct ctr_snd_xferi x;
        x.result = 0;
        x.buf = (void *)p;
        x.frames = left / 4;
        if (::ioctl(mFd, CTR_PCM_WRITEI_FRAMES, &x) < 0) {
            if (errno == EPIPE || errno == ESTRPIPE) {
                ::ioctl(mFd, CTR_PCM_PREPARE);      // underrun: restart
                continue;
            }
            if (errno == EINTR)
                continue;
            LOGE("PCM write failed: %%s", strerror(errno));
            ::close(mFd);
            mFd = -1;
            usleep(((left / 4) * 1000000ULL) / sampleRate());
            return bytes;
        }
        p += x.result * 4;
        left -= x.result * 4;
    }
    return bytes;
}

status_t AudioStreamOutGeneric::standby()
{
    Mutex::Autolock _l(mLock);
    if (mFd >= 0) {
        ::ioctl(mFd, CTR_PCM_DROP);
        ::close(mFd);
        mFd = -1;
    }
    return NO_ERROR;
}
""" % {"m": MARK}, "output stream")

    # input stream: timed silence instead of a read() on a closed fd
    s = sub(s, """    AutoMutex lock(mLock);
    if (mFd < 0) {
        LOGE("Attempt to read from unopened device");
        return NO_INIT;
    }
    return ::read(mFd, buffer, bytes);""",
            """    AutoMutex lock(mLock);
    // %s no microphone path in Froyo: deliver silence at the stream rate
    memset(buffer, 0, bytes);
    usleep(((bytes / 2) * 1000000ULL) / sampleRate());   // mono S16
    return bytes;""" % MARK, "input read")
    CPP.write_text(s)

    # header: the PCM keeps a few periods queued, so report more latency
    h = HDR.read_text()
    h = h.replace("virtual uint32_t    latency() const { return 20; }",
                  "virtual uint32_t    latency() const { return 100; }")
    HDR.write_text(h)
    print("fix-cm6-audio: patched %s" % CPP)
    return 0


sys.exit(main())
