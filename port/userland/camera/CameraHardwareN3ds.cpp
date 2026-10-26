/*
 * CameraHardwareN3ds -- the camera HAL for the Nintendo 3DS port.
 *
 * N3DS_CAMERA_HAL
 *
 * Replaces CameraHardwareStub (AOSP's software camera) now that the kernel
 * has a real capture path.  The layering is unchanged: the stock Camera app
 * -> android.hardware.Camera -> libui camera client -> CameraService -> this.
 *
 * Source: /dev/ctr_cam (drivers/platform/nintendo3ds/ctr_cam.c).  ioctl START
 * {camera, width, height} programs one of the three MT9V113 sensors and
 * starts the CAM port; every read() then returns one complete YUV 4:2:2
 * frame, blocking for at most 2 s.  The driver's framing, P1 auto-slowdown
 * and diagnostics are all kernel-side and show up in dmesg.
 *
 * Preview is RGB565, not upstream's YCbCr_420_SP.  This build has no
 * copybit, so SurfaceFlinger's LayerBuffer draws preview buffers through
 * libagl's texture path, which takes YCbCr_420_SP as a luminance texture --
 * a grey picture.  RGB565 is native there.  CameraService registers the
 * preview and raw heaps with the format named by the preview-format
 * parameter (N3DS_CAMERA_RGB565 in CameraService.cpp).
 *
 * Stills: takePicture() restarts the stream at the picture size (640x480
 * or 320x240), lets auto-exposure settle, and JPEG-encodes the frame with
 * libjpeg straight from YCbCr -- no colour conversion.  If the sensor gives
 * nothing, the last preview frame is used instead, so the app always gets a
 * picture back rather than waiting forever in SNAPSHOT_IN_PROGRESS.
 *
 * Camera selection: Gingerbread has camera ids.  Id 0 is the outer right
 * sensor (back), id 1 the inner one (front).  The "n3ds-camera-id"
 * parameter still picks any of the three sensors (2 = outer left).
 *
 * Byte order: the driver programs the sensor for Y0 Cb Y1 Cr.  What the CAM
 * FIFO packs into its words is not documented, so the luma position is
 * checked on live frames (luma swings far from 128, chroma does not) and
 * locked after three agreeing frames.  Cb/Cr order cannot be told apart
 * that way; `setprop n3ds.cam.swapuv 1` swaps it, and `setprop n3ds.cam.order
 * yuyv|yvyu|uyvy|vyuy` overrides everything.  Both are read at startPreview.
 */

#undef LOG_TAG
#define LOG_TAG "CameraHardwareN3ds"
#include <utils/Log.h>

#include "CameraHardwareN3ds.h"
#include <cutils/properties.h>
#include <utils/threads.h>

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" {
#include <jpeglib.h>
}

namespace android {

/* ---- kernel ABI: a copy of the block at the top of ctr_cam.c ------------ */

#define CTR_CAM_OUTER_RIGHT     0
#define CTR_CAM_INNER           1
#define CTR_CAM_OUTER_LEFT      2
#define CTR_CAM_COUNT           3

struct ctr_cam_config {
    uint32_t camera;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
};

struct ctr_cam_stats {
    uint32_t frames;
    uint32_t short_frames;
    uint32_t overruns;
    uint32_t last_gap_bytes;
    uint32_t max_fifo;
    uint32_t p1;
    uint64_t words;
};

#define CTR_CAM_IOC_START   _IOW('c', 0x01, struct ctr_cam_config)
#define CTR_CAM_IOC_STOP    _IO('c', 0x02)
#define CTR_CAM_IOC_STATS   _IOR('c', 0x03, struct ctr_cam_stats)

static const char kDevice[] = "/dev/ctr_cam";

/* ---- parameters ------------------------------------------------------- */

static const char kKeyCameraId[]       = "n3ds-camera-id";
static const char kKeyCameraIdValues[] = "n3ds-camera-id-values";
static const char kPreviewFormat[]     = "rgb565";

static const int kMaxW = 640;
static const int kMaxH = 480;
static const int kMaxFrameBytes = kMaxW * kMaxH * 2;

/* How long a freshly (re)started sensor gets before a still is taken:
 * the full init list resets auto-exposure, which needs a dozen frames. */
static const int kSettleMs = 1200;
static const int kSettleFrames = 3;
static const int kCaptureBudgetMs = 7000;

static const char *cameraName(int id)
{
    switch (id) {
    case CTR_CAM_OUTER_RIGHT: return "outer right (back)";
    case CTR_CAM_INNER:       return "inner (front)";
    case CTR_CAM_OUTER_LEFT:  return "outer left";
    }
    return "?";
}

static inline int msSince(nsecs_t t0)
{
    return (int)ns2ms(systemTime() - t0);
}

/* ---- construction ------------------------------------------------------ */

CameraHardwareN3ds::CameraHardwareN3ds(int sensor)
    : mSensor(sensor),
      mPreviewStopping(false),
      mParameters(),
      mPreviewHeap(0),
      mRawHeap(0),
      mPreviewFrameSize(0),
      mRawW(0), mRawH(0),
      mNotifyCb(0),
      mDataCb(0),
      mDataCbTimestamp(0),
      mCallbackCookie(0),
      mMsgEnabled(0),
      mCurrentPreviewFrame(0),
      mFd(-1),
      mStreamOn(false),
      mStreamCam(-1), mStreamW(0), mStreamH(0),
      mStreamStart(0),
      mStreamSawFrame(false),
      mLastOpenTry(0),
      mLastStartFail(0),
      mLastErrno(0),
      mFill(0),
      mLastValid(false),
      mLastIdx(0), mLastW(0), mLastH(0),
      mOffY0(0), mOffY1(2), mOffU(1), mOffV(3),
      mOrderLocked(false),
      mOrderCand(-1), mOrderVotes(0),
      mSwapUV(false),
      mFramesOk(0), mTimeouts(0), mBadSize(0), mConsecTimeouts(0),
      mLastReport(0)
{
    mLastErrWhat[0] = 0;
    mYuyv[0] = (uint8_t *)malloc(kMaxFrameBytes);
    mYuyv[1] = (uint8_t *)malloc(kMaxFrameBytes);
    if (mYuyv[0] == 0 || mYuyv[1] == 0)
        LOGE("N3DS_CAMERA_HAL: cannot allocate frame buffers");
    initDefaultParameters();
    LOGI("N3DS_CAMERA_HAL: camera HAL up, source %s", kDevice);
}

CameraHardwareN3ds::~CameraHardwareN3ds()
{
    stopPreviewThread();
    {
        Mutex::Autolock d(mDevLock);
        stopStreamLocked();
        if (mFd >= 0) {
            close(mFd);
            mFd = -1;
        }
    }
    free(mYuyv[0]);
    free(mYuyv[1]);
    mYuyv[0] = mYuyv[1] = 0;
    singleton.clear();
}

void CameraHardwareN3ds::initDefaultParameters()
{
    CameraParameters p;

    p.setPreviewSize(320, 240);
    p.setPreviewFrameRate(15);
    p.setPreviewFormat(kPreviewFormat);
    p.setPictureSize(640, 480);
    p.setPictureFormat("jpeg");
    p.set("jpeg-quality", 90);
    p.set("focus-mode", "auto");
    p.set(kKeyCameraId, mSensor);

    if (setParameters(p) != NO_ERROR)
        LOGE("Failed to set default parameters?!");
}

/* The lists apps read to find out what is supported.  Re-asserted on every
 * setParameters() so a client that round-trips a trimmed set cannot lose them. */
static void setSupportedLists(CameraParameters &p)
{
    p.set("preview-size-values", "320x240,160x120");
    p.set("preview-format-values", kPreviewFormat);
    p.set("preview-frame-rate-values", "15");
    p.set("picture-size-values", "640x480,320x240");
    p.set("picture-format-values", "jpeg");
    p.set("focus-mode-values", "auto");
    p.set(kKeyCameraIdValues, "0,1,2");
}

void CameraHardwareN3ds::initHeapLocked()
{
    int pw, ph;
    mParameters.getPreviewSize(&pw, &ph);
    int size = pw * ph * 2;

    if (size != mPreviewFrameSize || mPreviewHeap == 0) {
        LOGD("initHeapLocked: preview %dx%d RGB565", pw, ph);
        mPreviewFrameSize = size;
        mPreviewHeap = new MemoryHeapBase(size * kBufferCount);
        for (int i = 0; i < kBufferCount; i++)
            mBuffers[i] = new MemoryBase(mPreviewHeap, i * size, size);
        mCurrentPreviewFrame = 0;
    }

    int w, h;
    mParameters.getPictureSize(&w, &h);
    if (w != mRawW || h != mRawH || mRawHeap == 0) {
        mRawW = w;
        mRawH = h;
        mRawHeap = new MemoryHeapBase(w * h * 2);
    }
}

/* ---- trivial interface ------------------------------------------------- */

sp<IMemoryHeap> CameraHardwareN3ds::getPreviewHeap() const
{
    return mPreviewHeap;
}

sp<IMemoryHeap> CameraHardwareN3ds::getRawHeap() const
{
    return mRawHeap;
}

void CameraHardwareN3ds::setCallbacks(notify_callback notify_cb,
                                      data_callback data_cb,
                                      data_callback_timestamp data_cb_timestamp,
                                      void* user)
{
    Mutex::Autolock lock(mLock);
    mNotifyCb = notify_cb;
    mDataCb = data_cb;
    mDataCbTimestamp = data_cb_timestamp;
    mCallbackCookie = user;
}

void CameraHardwareN3ds::enableMsgType(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    mMsgEnabled |= msgType;
}

void CameraHardwareN3ds::disableMsgType(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    mMsgEnabled &= ~msgType;
}

bool CameraHardwareN3ds::msgTypeEnabled(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    return (mMsgEnabled & msgType);
}

/* ---- the device -------------------------------------------------------- */

void CameraHardwareN3ds::setErrorLocked(const char *what, int err)
{
    strncpy(mLastErrWhat, what, sizeof(mLastErrWhat) - 1);
    mLastErrWhat[sizeof(mLastErrWhat) - 1] = 0;
    mLastErrno = err;
}

/*
 * Open the device if needed and make sure it streams camera `cam` at w x h.
 * Failures are rate-limited to one attempt per 2 s so a missing driver does
 * not turn the preview thread into an open()/ioctl() loop.
 */
bool CameraHardwareN3ds::ensureStreamLocked(int cam, int w, int h,
                                            bool *restarted)
{
    if (restarted)
        *restarted = false;

    if (mFd < 0) {
        nsecs_t now = systemTime();
        if (mLastOpenTry != 0 && now - mLastOpenTry < ms2ns(2000))
            return false;
        mLastOpenTry = now;
        mFd = open(kDevice, O_RDWR);
        if (mFd < 0) {
            int e = errno;
            if (mLastErrno != e || strcmp(mLastErrWhat, "OPEN"))
                LOGE("N3DS_CAMERA_HAL: open %s: %s", kDevice, strerror(e));
            setErrorLocked("OPEN", e);
            return false;
        }
        LOGI("opened %s", kDevice);
        mStreamOn = false;
    }

    if (mStreamOn && mStreamCam == cam && mStreamW == w && mStreamH == h)
        return true;

    if (mLastStartFail != 0 && systemTime() - mLastStartFail < ms2ns(2000))
        return false;

    struct ctr_cam_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.camera = cam;
    cfg.width = w;
    cfg.height = h;

    nsecs_t t0 = systemTime();
    if (ioctl(mFd, CTR_CAM_IOC_START, &cfg) < 0) {
        int e = errno;
        LOGE("N3DS_CAMERA_HAL: START camera %d %dx%d failed: %s",
             cam, w, h, strerror(e));
        setErrorLocked("START", e);
        mStreamOn = false;
        mLastStartFail = systemTime();
        return false;
    }
    mLastStartFail = 0;
    mLastErrWhat[0] = 0;
    mLastErrno = 0;

    LOGI("N3DS_CAMERA_HAL: streaming camera %d, %s, at %dx%d "
         "(sensor start took %d ms)", cam, cameraName(cam), w, h, msSince(t0));
    mStreamOn = true;
    mStreamCam = cam;
    mStreamW = w;
    mStreamH = h;
    mStreamStart = systemTime();
    mStreamSawFrame = false;
    if (restarted)
        *restarted = true;
    return true;
}

void CameraHardwareN3ds::stopStreamLocked()
{
    if (mFd >= 0 && mStreamOn) {
        if (ioctl(mFd, CTR_CAM_IOC_STOP) < 0)
            LOGW("STOP: %s", strerror(errno));
    }
    mStreamOn = false;
}

/* 0 on a frame of exactly w*h*2 bytes, otherwise -errno (-EMSGSIZE for a
 * frame of some other size, left over from before a restart). */
int CameraHardwareN3ds::readFrame(int fd, uint8_t *dst, int w, int h)
{
    if (fd < 0)
        return -ENODEV;
    ssize_t n = read(fd, dst, kMaxFrameBytes);
    if (n < 0)
        return -errno;
    if (n != (ssize_t)(w * h * 2))
        return -EMSGSIZE;
    return 0;
}

/* ---- pixels ------------------------------------------------------------ */

void CameraHardwareN3ds::applyOrder(bool lumaOdd, bool swapUV)
{
    int c0, c1;
    if (lumaOdd) {
        mOffY0 = 1; mOffY1 = 3; c0 = 0; c1 = 2;
    } else {
        mOffY0 = 0; mOffY1 = 2; c0 = 1; c1 = 3;
    }
    mOffU = swapUV ? c1 : c0;
    mOffV = swapUV ? c0 : c1;
}

void CameraHardwareN3ds::loadOrderOverride()
{
    char order[PROPERTY_VALUE_MAX];
    char swap[PROPERTY_VALUE_MAX];

    property_get("n3ds.cam.order", order, "auto");
    property_get("n3ds.cam.swapuv", swap, "0");
    mSwapUV = (swap[0] == '1');

    if (!strcmp(order, "yuyv"))      { applyOrder(false, false); mOrderLocked = true; }
    else if (!strcmp(order, "yvyu")) { applyOrder(false, true);  mOrderLocked = true; }
    else if (!strcmp(order, "uyvy")) { applyOrder(true, false);  mOrderLocked = true; }
    else if (!strcmp(order, "vyuy")) { applyOrder(true, true);   mOrderLocked = true; }
    else if (!mOrderLocked)          { applyOrder(mOrderCand == 1, mSwapUV); }

    LOGI("byte order: %s (Y0 %d, Y1 %d, U %d, V %d)%s", order,
         mOffY0, mOffY1, mOffU, mOffV, mOrderLocked ? ", locked" : "");
}

/*
 * Luma swings far from 128 on anything but a mid-grey wall; chroma sits
 * near it on anything but saturated colour.  So the mean squared distance
 * from 128 of the even bytes against the odd ones says which are luma.
 * Three agreeing, clear-cut frames lock the answer.
 */
void CameraHardwareN3ds::decideOrder(const uint8_t *f, int w, int h)
{
    if (mOrderLocked)
        return;

    uint64_t se = 0, so = 0;
    int n = 0;
    const int total = w * h * 2;
    for (int i = 0; i + 3 < total; i += 4 * 37) {
        int a = f[i] - 128, b = f[i + 1] - 128;
        int c = f[i + 2] - 128, d = f[i + 3] - 128;
        se += (uint64_t)(a * a + c * c);
        so += (uint64_t)(b * b + d * d);
        n++;
    }
    if (n < 64)
        return;

    const uint64_t floor = (uint64_t)n * 2 * 200;
    int vote;
    if (se > 2 * so && se > floor)
        vote = 0;
    else if (so > 2 * se && so > floor)
        vote = 1;
    else
        return;

    if (vote == mOrderCand) {
        mOrderVotes++;
    } else {
        mOrderCand = vote;
        mOrderVotes = 1;
    }
    if (mOrderVotes >= 3) {
        applyOrder(vote == 1, mSwapUV);
        mOrderLocked = true;
        LOGI("N3DS_CAMERA_HAL: luma is in the %s bytes (msd even %llu, odd %llu over %d samples); "
             "order locked, U at %d, V at %d", vote ? "odd" : "even",
             (unsigned long long)(se / n), (unsigned long long)(so / n), n,
             mOffU, mOffV);
    }
}

static inline int clamp8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static inline uint16_t rgb565(int y, int u, int v)
{
    /* JFIF full-range YCbCr, the same the JPEG encoder assumes, so the
     * preview and the saved picture agree. */
    int d = u - 128, e = v - 128;
    int r = clamp8(y + ((359 * e) >> 8));
    int g = clamp8(y - ((88 * d + 183 * e) >> 8));
    int b = clamp8(y + ((454 * d) >> 8));
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void CameraHardwareN3ds::toRgb565(const uint8_t *src, int sw, int sh,
                                  uint16_t *dst, int dw, int dh) const
{
    const int oy0 = mOffY0, oy1 = mOffY1, ou = mOffU, ov = mOffV;

    if (sw == dw && sh == dh) {
        const int pairs = sw * sh / 2;
        for (int i = 0; i < pairs; i++, src += 4, dst += 2) {
            int u = src[ou], v = src[ov];
            dst[0] = rgb565(src[oy0], u, v);
            dst[1] = rgb565(src[oy1], u, v);
        }
        return;
    }

    /* Nearest neighbour: only the raw postview of a fallback still needs it. */
    for (int y = 0; y < dh; y++) {
        const uint8_t *line = src + (y * sh / dh) * sw * 2;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            const uint8_t *m = line + (sx & ~1) * 2;
            *dst++ = rgb565(m[(sx & 1) ? oy1 : oy0], m[ou], m[ov]);
        }
    }
}

/* 5x7, one byte per column, bit 0 is the top row (same font as FakeCamera). */
static const int kFontW = 5;
static const int kFontH = 7;
static const unsigned char kFont[][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 },  /* space */
    { 0x3e, 0x51, 0x49, 0x45, 0x3e },  /* 0 */
    { 0x00, 0x42, 0x7f, 0x40, 0x00 },  /* 1 */
    { 0x42, 0x61, 0x51, 0x49, 0x46 },  /* 2 */
    { 0x21, 0x41, 0x45, 0x4b, 0x31 },  /* 3 */
    { 0x18, 0x14, 0x12, 0x7f, 0x10 },  /* 4 */
    { 0x27, 0x45, 0x45, 0x45, 0x39 },  /* 5 */
    { 0x3c, 0x4a, 0x49, 0x49, 0x30 },  /* 6 */
    { 0x01, 0x71, 0x09, 0x05, 0x03 },  /* 7 */
    { 0x36, 0x49, 0x49, 0x49, 0x36 },  /* 8 */
    { 0x06, 0x49, 0x49, 0x29, 0x1e },  /* 9 */
    { 0x7e, 0x09, 0x09, 0x09, 0x7e },  /* A */
    { 0x7f, 0x49, 0x49, 0x49, 0x36 },  /* B */
    { 0x3e, 0x41, 0x41, 0x41, 0x22 },  /* C */
    { 0x7f, 0x41, 0x41, 0x41, 0x3e },  /* D */
    { 0x7f, 0x49, 0x49, 0x49, 0x41 },  /* E */
    { 0x7f, 0x09, 0x09, 0x09, 0x01 },  /* F */
    { 0x3e, 0x41, 0x49, 0x49, 0x3a },  /* G */
    { 0x7f, 0x08, 0x08, 0x08, 0x7f },  /* H */
    { 0x41, 0x41, 0x7f, 0x41, 0x41 },  /* I */
    { 0x20, 0x40, 0x41, 0x3f, 0x01 },  /* J */
    { 0x7f, 0x08, 0x14, 0x22, 0x41 },  /* K */
    { 0x7f, 0x40, 0x40, 0x40, 0x40 },  /* L */
    { 0x7f, 0x02, 0x04, 0x02, 0x7f },  /* M */
    { 0x7f, 0x02, 0x04, 0x08, 0x7f },  /* N */
    { 0x3e, 0x41, 0x41, 0x41, 0x3e },  /* O */
    { 0x7f, 0x09, 0x09, 0x09, 0x06 },  /* P */
    { 0x3e, 0x41, 0x51, 0x21, 0x5e },  /* Q */
    { 0x7f, 0x09, 0x19, 0x29, 0x46 },  /* R */
    { 0x46, 0x49, 0x49, 0x49, 0x31 },  /* S */
    { 0x01, 0x01, 0x7f, 0x01, 0x01 },  /* T */
    { 0x3f, 0x40, 0x40, 0x40, 0x3f },  /* U */
    { 0x1f, 0x20, 0x40, 0x20, 0x1f },  /* V */
    { 0x7f, 0x20, 0x10, 0x20, 0x7f },  /* W */
    { 0x63, 0x14, 0x08, 0x14, 0x63 },  /* X */
    { 0x03, 0x04, 0x78, 0x04, 0x03 },  /* Y */
    { 0x61, 0x51, 0x49, 0x45, 0x43 },  /* Z */
    { 0x00, 0x60, 0x60, 0x00, 0x00 },  /* . */
    { 0x00, 0x36, 0x36, 0x00, 0x00 },  /* : */
    { 0x08, 0x08, 0x08, 0x08, 0x08 },  /* - */
    { 0x40, 0x30, 0x30, 0x00, 0x00 },  /* , */
    { 0x60, 0x10, 0x08, 0x04, 0x03 },  /* / */
};

static int glyphIndex(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    switch (c) {
    case '.': return 37;
    case ':': return 38;
    case '-': return 39;
    case ',': return 40;
    case '/': return 41;
    }
    return 0;
}

static void drawText(uint16_t *dst, int w, int h, int x, int y,
                     const char *s, int scale, uint16_t color)
{
    for (; *s; s++, x += (kFontW + 1) * scale) {
        const unsigned char *g = kFont[glyphIndex(*s)];
        for (int col = 0; col < kFontW; col++) {
            for (int row = 0; row < kFontH; row++) {
                if (!(g[col] & (1 << row)))
                    continue;
                for (int sy = 0; sy < scale; sy++) {
                    int py = y + row * scale + sy;
                    if (py < 0 || py >= h)
                        continue;
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        if (px >= 0 && px < w)
                            dst[py * w + px] = color;
                    }
                }
            }
        }
    }
}

/* What the preview says while there is no picture to show. */
void CameraHardwareN3ds::drawStatus(uint16_t *dst, int w, int h,
                                    const char *l1, const char *l2,
                                    const char *l3) const
{
    const char *lines[3] = { l1, l2, l3 };
    const uint16_t colors[3] = { 0x073f, 0xce9b, 0xfd80 };
    int widest = 1;

    for (int i = 0; i < w * h; i++)
        dst[i] = 0x10a3;
    for (int i = 0; i < 3; i++)
        if (lines[i] && (int)strlen(lines[i]) > widest)
            widest = (int)strlen(lines[i]);

    int scale = (w - 8) / (widest * (kFontW + 1));
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    int lineH = (kFontH + 4) * scale;
    int y = (h - 3 * lineH) / 2;
    for (int i = 0; i < 3; i++, y += lineH) {
        if (!lines[i] || !lines[i][0])
            continue;
        int tw = (int)strlen(lines[i]) * (kFontW + 1) * scale;
        drawText(dst, w, h, (w - tw) / 2, y, lines[i], scale, colors[i]);
    }
}

/* ---- JPEG -------------------------------------------------------------- */

struct N3dsJpegErr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void n3dsJpegErrorExit(j_common_ptr c)
{
    char msg[JMSG_LENGTH_MAX];
    (*c->err->format_message)(c, msg);
    LOGE("libjpeg: %s", msg);
    longjmp(((N3dsJpegErr *)c->err)->jb, 1);
}

static void n3dsJpegOutputMessage(j_common_ptr c)
{
    char msg[JMSG_LENGTH_MAX];
    (*c->err->format_message)(c, msg);
    LOGW("libjpeg: %s", msg);
}

struct N3dsJpegDest {
    struct jpeg_destination_mgr pub;
    JOCTET *buf;
    size_t cap;
    bool overflow;
};

static void n3dsDestInit(j_compress_ptr c)
{
    N3dsJpegDest *d = (N3dsJpegDest *)c->dest;
    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = d->cap;
}

static boolean n3dsDestEmpty(j_compress_ptr c)
{
    /* Sized so this never happens; if it does, the result is discarded. */
    N3dsJpegDest *d = (N3dsJpegDest *)c->dest;
    d->overflow = true;
    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = d->cap;
    return TRUE;
}

static void n3dsDestTerm(j_compress_ptr c)
{
    (void)c;
}

status_t CameraHardwareN3ds::encodeJpeg(const uint8_t *yuyv, int w, int h,
                                        int quality, uint8_t *out, size_t cap,
                                        size_t *len) const
{
    struct jpeg_compress_struct cinfo;
    N3dsJpegErr jerr;
    N3dsJpegDest dest;
    JSAMPLE *row = (JSAMPLE *)malloc(w * 3);

    if (row == 0)
        return NO_MEMORY;

    const int oy0 = mOffY0, oy1 = mOffY1, ou = mOffU, ov = mOffV;

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = n3dsJpegErrorExit;
    jerr.pub.output_message = n3dsJpegOutputMessage;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_compress(&cinfo);
        free(row);
        return UNKNOWN_ERROR;
    }

    jpeg_create_compress(&cinfo);
    memset(&dest, 0, sizeof(dest));
    dest.pub.init_destination = n3dsDestInit;
    dest.pub.empty_output_buffer = n3dsDestEmpty;
    dest.pub.term_destination = n3dsDestTerm;
    dest.buf = out;
    dest.cap = cap;
    dest.overflow = false;
    cinfo.dest = &dest.pub;

    /* The sensor already produces YCbCr, which is what JPEG stores. */
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.dct_method = JDCT_IFAST;
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t *s = yuyv + cinfo.next_scanline * w * 2;
        JSAMPLE *d = row;
        for (int x = 0; x < w; x += 2, s += 4, d += 6) {
            d[0] = s[oy0]; d[1] = s[ou]; d[2] = s[ov];
            d[3] = s[oy1]; d[4] = s[ou]; d[5] = s[ov];
        }
        JSAMPROW rp = row;
        jpeg_write_scanlines(&cinfo, &rp, 1);
    }
    jpeg_finish_compress(&cinfo);

    *len = cap - dest.pub.free_in_buffer;
    bool overflow = dest.overflow;
    jpeg_destroy_compress(&cinfo);
    free(row);
    return overflow ? NO_MEMORY : NO_ERROR;
}

/* ---- preview ----------------------------------------------------------- */

void CameraHardwareN3ds::postPreview(const sp<MemoryBase>& buffer, int slot)
{
    /* see takePicture(): no callbacks once a picture is on its way */
    if (mPreviewStopping)
        return;
    if ((mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) && mDataCb)
        mDataCb(CAMERA_MSG_PREVIEW_FRAME, buffer, mCallbackCookie);

    Mutex::Autolock lock(mLock);
    mCurrentPreviewFrame = (slot + 1) % kBufferCount;
}

int CameraHardwareN3ds::previewThread()
{
    int pw, ph, cam, frameSize, slot;
    sp<MemoryHeapBase> heap;
    sp<MemoryBase> buffer;

    if (mPreviewStopping) {
        usleep(10000);
        return NO_ERROR;
    }

    mLock.lock();
    mParameters.getPreviewSize(&pw, &ph);
    cam = mParameters.getInt(kKeyCameraId);
    heap = mPreviewHeap;
    slot = mCurrentPreviewFrame;
    buffer = mBuffers[slot];
    frameSize = mPreviewFrameSize;
    int fill = mFill;
    mLock.unlock();

    if (heap == 0 || buffer == 0 || pw <= 0 || ph <= 0 ||
        frameSize != pw * ph * 2 || mYuyv[fill] == 0) {
        usleep(50000);
        return NO_ERROR;
    }
    if (cam < 0 || cam >= CTR_CAM_COUNT)
        cam = CTR_CAM_OUTER_RIGHT;

    uint16_t *dst = (uint16_t *)((uint8_t *)heap->base() + slot * frameSize);
    char l2[48], l3[48];
    bool ok, needStart;
    int fd;

    {
        Mutex::Autolock d(mDevLock);
        needStart = !(mFd >= 0 && mStreamOn && mStreamCam == cam &&
                      mStreamW == pw && mStreamH == ph);
    }
    if (needStart) {
        /* The sensor init list takes a moment; say so rather than freeze. */
        snprintf(l2, sizeof(l2), "%s", cameraName(cam));
        drawStatus(dst, pw, ph, "STARTING CAMERA", l2, "");
        postPreview(buffer, slot);
        mLock.lock();
        slot = mCurrentPreviewFrame;
        buffer = mBuffers[slot];
        mLock.unlock();
        if (buffer == 0)
            return NO_ERROR;
        dst = (uint16_t *)((uint8_t *)heap->base() + slot * frameSize);
    }

    {
        Mutex::Autolock d(mDevLock);
        ok = ensureStreamLocked(cam, pw, ph, 0);
        fd = mFd;
        if (!ok) {
            snprintf(l2, sizeof(l2), "%s FAILED:", mLastErrWhat[0] ? mLastErrWhat : "DEVICE");
            snprintf(l3, sizeof(l3), "%s", mLastErrno ? strerror(mLastErrno) : "UNKNOWN");
        }
    }
    if (!ok) {
        drawStatus(dst, pw, ph, "CAMERA NOT AVAILABLE", l2, l3);
        postPreview(buffer, slot);
        usleep(500000);
        return NO_ERROR;
    }

    int r = readFrame(fd, mYuyv[fill], pw, ph);
    if (r == 0) {
        decideOrder(mYuyv[fill], pw, ph);
        toRgb565(mYuyv[fill], pw, ph, dst, pw, ph);
        {
            Mutex::Autolock lock(mLock);
            mLastIdx = fill;
            mLastW = pw;
            mLastH = ph;
            mLastValid = true;
            mFill = fill ^ 1;
        }
        {
            Mutex::Autolock d(mDevLock);
            if (!mStreamSawFrame) {
                mStreamSawFrame = true;
                LOGI("N3DS_CAMERA_HAL: first frame from camera %d %d ms after START",
                     cam, msSince(mStreamStart));
            }
        }
        mFramesOk++;
        mConsecTimeouts = 0;
        postPreview(buffer, slot);
    } else if (r == -ETIMEDOUT) {
        struct ctr_cam_stats st;
        memset(&st, 0, sizeof(st));
        ioctl(fd, CTR_CAM_IOC_STATS, &st);
        mTimeouts++;
        mConsecTimeouts++;
        LOGW("no frame from camera %d in 2 s (frames %u, short %u, overruns %u, last gap %u, P1 %u)",
             cam, st.frames, st.short_frames, st.overruns, st.last_gap_bytes, st.p1);
        /*
         * N3DS_CAMERA_KEEP_LAST_FRAME: one late frame (a dropped frame, an
         * overrun restart) must not wipe the picture for a status screen and
         * flash it back a second later. Keep the last picture on screen
         * until the stream has really stalled: 6 s, or no frame yet.
         */
        bool haveFrame;
        {
            Mutex::Autolock lock(mLock);
            haveFrame = mLastValid && mLastW == pw && mLastH == ph;
        }
        if (!haveFrame || mConsecTimeouts >= 3) {
            snprintf(l2, sizeof(l2), "FRAMES %u SHORT %u", st.frames, st.short_frames);
            snprintf(l3, sizeof(l3), "OVERRUNS %u P1 %u", st.overruns, st.p1);
            drawStatus(dst, pw, ph, "WAITING FOR FRAMES", l2, l3);
            postPreview(buffer, slot);
        }
    } else if (r == -EIO) {
        /* The driver stopped the stream under us; restart it next pass. */
        LOGW("camera %d: stream stopped by the driver, restarting", cam);
        Mutex::Autolock d(mDevLock);
        mStreamOn = false;
    } else if (r == -EMSGSIZE) {
        mBadSize++;
    } else if (r != -EAGAIN && r != -EINTR) {
        LOGE("read %s: %s", kDevice, strerror(-r));
        usleep(100000);
    }

    nsecs_t now = systemTime();
    if (mLastReport == 0)
        mLastReport = now;
    if (now - mLastReport >= seconds(10)) {
        mLastReport = now;
        LOGI("preview: camera %d %dx%d, %u frames, %u timeouts, %u wrong-size",
             cam, pw, ph, mFramesOk, mTimeouts, mBadSize);
    }
    return NO_ERROR;
}

status_t CameraHardwareN3ds::startPreview()
{
    Mutex::Autolock lock(mLock);
    if (mPreviewThread != 0)
        return INVALID_OPERATION;
    mPreviewStopping = false;
    loadOrderOverride();
    mPreviewThread = new PreviewThread(this);
    return NO_ERROR;
}

/* Join the preview thread but leave the sensor streaming: takePicture()
 * at the preview size can then use it without a restart. */
void CameraHardwareN3ds::stopPreviewThread()
{
    sp<PreviewThread> previewThread;
    {
        Mutex::Autolock lock(mLock);
        previewThread = mPreviewThread;
    }
    if (previewThread != 0)
        previewThread->requestExitAndWait();

    Mutex::Autolock lock(mLock);
    mPreviewThread.clear();
}

void CameraHardwareN3ds::stopPreview()
{
    stopPreviewThread();
    Mutex::Autolock d(mDevLock);
    stopStreamLocked();
}

bool CameraHardwareN3ds::previewEnabled()
{
    return mPreviewThread != 0;
}

status_t CameraHardwareN3ds::startRecording()
{
    /* No video encoder in this image. */
    return UNKNOWN_ERROR;
}

void CameraHardwareN3ds::stopRecording()
{
}

bool CameraHardwareN3ds::recordingEnabled()
{
    return false;
}

void CameraHardwareN3ds::releaseRecordingFrame(const sp<IMemory>& mem)
{
    (void)mem;
}

/* ---- focus: fixed-focus sensors, so focusing always succeeds ----------- */

int CameraHardwareN3ds::beginAutoFocusThread(void *cookie)
{
    return ((CameraHardwareN3ds *)cookie)->autoFocusThread();
}

int CameraHardwareN3ds::autoFocusThread()
{
    if ((mMsgEnabled & CAMERA_MSG_FOCUS) && mNotifyCb)
        mNotifyCb(CAMERA_MSG_FOCUS, true, 0, mCallbackCookie);
    return NO_ERROR;
}

status_t CameraHardwareN3ds::autoFocus()
{
    Mutex::Autolock lock(mLock);
    if (createThread(beginAutoFocusThread, this) == false)
        return UNKNOWN_ERROR;
    return NO_ERROR;
}

status_t CameraHardwareN3ds::cancelAutoFocus()
{
    return NO_ERROR;
}

/* ---- stills ------------------------------------------------------------ */

/*
 * Get one good w x h frame of camera `cam` into mYuyv[*idx].  A stream that
 * had to be (re)started gets kSettleMs for auto-exposure first; one that
 * was already running just skips a frame that may predate the request.
 */
bool CameraHardwareN3ds::captureStill(int cam, int w, int h, int *idx)
{
    Mutex::Autolock d(mDevLock);
    bool restarted = false;

    /* A failed START a moment ago must not veto the still. */
    mLastStartFail = 0;
    mLastOpenTry = 0;
    if (!ensureStreamLocked(cam, w, h, &restarted))
        return false;

    int fill;
    {
        Mutex::Autolock lock(mLock);
        fill = mFill;
    }
    if (mYuyv[fill] == 0)
        return false;

    nsecs_t t0 = systemTime();
    int got = 0;
    for (;;) {
        int r = readFrame(mFd, mYuyv[fill], w, h);
        int el = msSince(t0);
        if (r == 0) {
            got++;
            decideOrder(mYuyv[fill], w, h);
            bool settled = restarted ? (el >= kSettleMs && got >= kSettleFrames)
                                     : (got >= 2);
            if (settled || got >= 30) {
                LOGI("N3DS_CAMERA_HAL: still from camera %d at %dx%d after %d frames, %d ms",
                     cam, w, h, got, el);
                Mutex::Autolock lock(mLock);
                mLastIdx = fill;
                mLastW = w;
                mLastH = h;
                mLastValid = true;
                mFill = fill ^ 1;
                *idx = fill;
                return true;
            }
        } else if (r == -EIO) {
            mStreamOn = false;
            mLastStartFail = 0;
            if (!ensureStreamLocked(cam, w, h, &restarted))
                return false;
        } else if (r != -EAGAIN && r != -EINTR && r != -EMSGSIZE && r != -ETIMEDOUT) {
            LOGE("still: read %s: %s", kDevice, strerror(-r));
            return false;
        }
        if (el > kCaptureBudgetMs) {
            LOGE("still: no settled frame from camera %d in %d ms (%d frames)",
                 cam, el, got);
            return false;
        }
    }
}

int CameraHardwareN3ds::beginPictureThread(void *cookie)
{
    return ((CameraHardwareN3ds *)cookie)->pictureThread();
}

int CameraHardwareN3ds::pictureThread()
{
    int pw, ph, cam, quality;
    sp<MemoryHeapBase> rawHeap;

    stopPreviewThread();	/* see takePicture() */
    {
        Mutex::Autolock lock(mLock);
        mParameters.getPictureSize(&pw, &ph);
        cam = mParameters.getInt(kKeyCameraId);
        quality = mParameters.getInt("jpeg-quality");
        rawHeap = mRawHeap;
    }
    if (cam < 0 || cam >= CTR_CAM_COUNT)
        cam = CTR_CAM_OUTER_RIGHT;
    if (quality < 30 || quality > 100)
        quality = 90;

    /* 1. the frame: fresh at picture size, else the last preview frame,
     *    else black -- the app must always get its JPEG callback */
    const uint8_t *frame = 0;
    int fw = 0, fh = 0, idx = 0;
    uint8_t *black = 0;

    if (captureStill(cam, pw, ph, &idx)) {
        frame = mYuyv[idx];
        fw = pw;
        fh = ph;
    } else {
        Mutex::Autolock lock(mLock);
        if (mLastValid) {
            frame = mYuyv[mLastIdx];
            fw = mLastW;
            fh = mLastH;
            LOGW("N3DS_CAMERA_HAL: still capture failed, saving the last preview frame (%dx%d)",
                 fw, fh);
        }
    }
    if (frame == 0) {
        fw = pw;
        fh = ph;
        black = (uint8_t *)malloc(fw * fh * 2);
        if (black) {
            for (int i = 0; i < fw * fh * 2; i += 4) {
                black[i + mOffY0] = 16;
                black[i + mOffY1] = 16;
                black[i + mOffU] = 128;
                black[i + mOffV] = 128;
            }
        }
        frame = black;
        LOGE("N3DS_CAMERA_HAL: no camera frame at all; returning a black picture");
    }

    {
        /* Back to idle; the next startPreview() restarts at preview size. */
        Mutex::Autolock d(mDevLock);
        stopStreamLocked();
    }

    /* 2. shutter: CameraService plays the click and swaps the surface
     *    over to the raw heap */
    if ((mMsgEnabled & CAMERA_MSG_SHUTTER) && mNotifyCb)
        mNotifyCb(CAMERA_MSG_SHUTTER, 0, 0, mCallbackCookie);

    /* 3. raw: the postview, RGB565 at picture size */
    if ((mMsgEnabled & CAMERA_MSG_RAW_IMAGE) && mDataCb && rawHeap != 0 && frame) {
        toRgb565(frame, fw, fh, (uint16_t *)rawHeap->base(), pw, ph);
        sp<MemoryBase> mem = new MemoryBase(rawHeap, 0, pw * ph * 2);
        mDataCb(CAMERA_MSG_RAW_IMAGE, mem, mCallbackCookie);
    }

    /* 4. the JPEG */
    if ((mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE) && mDataCb && frame) {
        size_t cap = (size_t)fw * fh * 3 + 65536;
        uint8_t *out = (uint8_t *)malloc(cap);
        size_t len = 0;
        nsecs_t t0 = systemTime();
        status_t err = out ? encodeJpeg(frame, fw, fh, quality, out, cap, &len)
                           : NO_MEMORY;
        if (err == NO_ERROR && len > 0) {
            LOGI("N3DS_CAMERA_HAL: JPEG %dx%d q%d, %u bytes in %d ms",
                 fw, fh, quality, (unsigned)len, msSince(t0));
            sp<MemoryHeapBase> heap = new MemoryHeapBase(len);
            memcpy(heap->base(), out, len);
            sp<MemoryBase> mem = new MemoryBase(heap, 0, len);
            mDataCb(CAMERA_MSG_COMPRESSED_IMAGE, mem, mCallbackCookie);
        } else {
            LOGE("N3DS_CAMERA_HAL: JPEG encode failed (%d)", err);
            if (mNotifyCb)
                mNotifyCb(CAMERA_MSG_ERROR, UNKNOWN_ERROR, 0, mCallbackCookie);
        }
        free(out);
    }

    free(black);
    return NO_ERROR;
}

/*
 * Gingerbread's CameraService calls this with its client lock held and, unlike
 * stopPreview(), without disabling preview callbacks first.  The preview
 * thread may be about to post a frame through a callback that waits for that
 * same lock, so joining it here deadlocks (seen in Cyano3DS testing: the
 * shutter never returned).  So only ask it to stop and to post nothing more;
 * pictureThread() joins it once this has returned and the lock is free.
 */
status_t CameraHardwareN3ds::takePicture()
{
    {
        Mutex::Autolock lock(mLock);
        mPreviewStopping = true;
        if (mPreviewThread != 0)
            mPreviewThread->requestExit();
    }
    if (createThread(beginPictureThread, this) == false)
        return -1;
    return NO_ERROR;
}

status_t CameraHardwareN3ds::cancelPicture()
{
    return NO_ERROR;
}

/* ---- parameters -------------------------------------------------------- */

/*
 * Clamp to what the hardware does instead of failing: the Java side turns a
 * setParameters() error into a RuntimeException, which takes the Camera
 * app down over a preference it cannot know is unsupported.
 */
status_t CameraHardwareN3ds::setParameters(const CameraParameters& params)
{
    Mutex::Autolock lock(mLock);
    CameraParameters p = params;
    int w, h;

    p.getPreviewSize(&w, &h);
    if (!((w == 320 && h == 240) || (w == 160 && h == 120))) {
        LOGW("preview size %dx%d unsupported, using 320x240", w, h);
        p.setPreviewSize(320, 240);
    }

    const char *fmt = p.getPreviewFormat();
    if (fmt == 0 || strcmp(fmt, kPreviewFormat) != 0) {
        if (fmt != 0)
            LOGW("preview format %s unsupported, using %s", fmt, kPreviewFormat);
        p.setPreviewFormat(kPreviewFormat);
    }

    p.getPictureSize(&w, &h);
    if (!((w == 640 && h == 480) || (w == 320 && h == 240))) {
        bool big = (w <= 0 || h <= 0 || w * h > 320 * 240);
        LOGW("picture size %dx%d unsupported, using %s", w, h,
             big ? "640x480" : "320x240");
        p.setPictureSize(big ? 640 : 320, big ? 480 : 240);
    }

    fmt = p.getPictureFormat();
    if (fmt == 0 || strcmp(fmt, "jpeg") != 0)
        p.setPictureFormat("jpeg");

    if (p.getPreviewFrameRate() != 15)
        p.setPreviewFrameRate(15);

    int id = p.getInt(kKeyCameraId);
    if (id < 0 || id >= CTR_CAM_COUNT) {
        int old = mParameters.getInt(kKeyCameraId);
        p.set(kKeyCameraId, (old >= 0 && old < CTR_CAM_COUNT) ? old : 0);
    } else if (id != mParameters.getInt(kKeyCameraId)) {
        LOGI("N3DS_CAMERA_HAL: camera selection -> %d, %s", id, cameraName(id));
    }

    int q = p.getInt("jpeg-quality");
    if (q < 30 || q > 100)
        p.set("jpeg-quality", 90);

    setSupportedLists(p);
    mParameters = p;
    initHeapLocked();
    return NO_ERROR;
}

CameraParameters CameraHardwareN3ds::getParameters() const
{
    Mutex::Autolock lock(mLock);
    return mParameters;
}

status_t CameraHardwareN3ds::sendCommand(int32_t command, int32_t arg1,
                                         int32_t arg2)
{
    (void)command; (void)arg1; (void)arg2;
    return BAD_VALUE;
}

void CameraHardwareN3ds::release()
{
    stopPreviewThread();
    Mutex::Autolock d(mDevLock);
    stopStreamLocked();
    if (mFd >= 0) {
        close(mFd);
        mFd = -1;
    }
}

status_t CameraHardwareN3ds::dump(int fd, const Vector<String16>& args) const
{
    char buffer[256];
    String8 result;
    AutoMutex lock(&mLock);

    snprintf(buffer, sizeof(buffer),
             "CameraHardwareN3ds: frames %u, timeouts %u, wrong-size %u, "
             "order Y0 %d Y1 %d U %d V %d%s\n",
             mFramesOk, mTimeouts, mBadSize, mOffY0, mOffY1, mOffU, mOffV,
             mOrderLocked ? " (locked)" : "");
    result.append(buffer);
    write(fd, result.string(), result.size());
    mParameters.dump(fd, args);
    return NO_ERROR;
}

wp<CameraHardwareInterface> CameraHardwareN3ds::singleton;
int CameraHardwareN3ds::sSingletonSensor = -1;

/*
 * There is one /dev/ctr_cam and it streams one sensor at a time, so there
 * is at most one instance.  Opening the camera that is already open returns
 * it; opening another one while it is still alive fails (CameraService then
 * reports the camera as busy, and the app retries after it closed the other).
 */
sp<CameraHardwareInterface> CameraHardwareN3ds::createInstance(int sensor)
{
    if (singleton != 0) {
        sp<CameraHardwareInterface> hardware = singleton.promote();
        if (hardware != 0) {
            if (sSingletonSensor == sensor)
                return hardware;
            LOGE("N3DS_CAMERA_HAL: sensor %d requested while sensor %d is open",
                 sensor, sSingletonSensor);
            return NULL;
        }
    }
    sp<CameraHardwareInterface> hardware(new CameraHardwareN3ds(sensor));
    singleton = hardware;
    sSingletonSensor = sensor;
    return hardware;
}

/* ---- Gingerbread HAL entry points (CameraService.cpp) ------------------ */

/*
 * Orientation: CM7 on this port has a portrait-native display (the Camera
 * app runs at display rotation 90), so like on a phone the back camera says
 * 90 and the front one 270; with 0 the preview showed up turned sideways.
 */
static const CameraInfo sN3dsCameraInfo[] = {
    { CAMERA_FACING_BACK,  90 },    /* id 0: outer right sensor */
    { CAMERA_FACING_FRONT, 270 },   /* id 1: inner sensor */
};

/* No driver, no cameras: the Camera app then says so instead of hanging. */
extern "C" int HAL_getNumberOfCameras()
{
    if (access(kDevice, R_OK | W_OK) != 0) {
        LOGW("N3DS_CAMERA_HAL: %s missing (%s): no cameras", kDevice,
             strerror(errno));
        return 0;
    }
    return sizeof(sN3dsCameraInfo) / sizeof(sN3dsCameraInfo[0]);
}

extern "C" void HAL_getCameraInfo(int cameraId, struct CameraInfo* cameraInfo)
{
    if (cameraId < 0 || cameraId >= HAL_getNumberOfCameras())
        cameraId = 0;
    memcpy(cameraInfo, &sN3dsCameraInfo[cameraId], sizeof(CameraInfo));
}

extern "C" sp<CameraHardwareInterface> HAL_openCameraHardware(int cameraId)
{
    LOGI("N3DS_CAMERA_HAL: open camera id %d", cameraId);
    return CameraHardwareN3ds::createInstance(
        cameraId == 1 ? CTR_CAM_INNER : CTR_CAM_OUTER_RIGHT);
}

}; // namespace android
