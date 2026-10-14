/*
 * CameraHardwareN3ds -- the camera HAL for the Nintendo 3DS port.
 *
 * From Android3DS/Octoblimp (github.com/Octoblimp/Android-Eclair-3DS,
 * patches/frameworks_base.diff, Apache-2.0), moved from Eclair's
 * ui/CameraHardwareInterface.h to Gingerbread's camera/ one for Cyano3DS
 * (CM7.2): one instance per Android camera id, and the HAL_* entry points
 * Gingerbread's CameraService calls.
 *
 * N3DS_CAMERA_HAL: backed by /dev/ctr_cam (drivers/platform/nintendo3ds/
 * ctr_cam.c), which hands out one complete YUV 4:2:2 frame per read().  See
 * CameraHardwareN3ds.cpp for the pipeline; this header only mirrors the
 * CameraHardwareInterface that CameraHardwareStub implements.
 */

#ifndef ANDROID_HARDWARE_CAMERA_HARDWARE_N3DS_H
#define ANDROID_HARDWARE_CAMERA_HARDWARE_N3DS_H

#include <stdint.h>
#include <utils/threads.h>
#include <utils/Timers.h>
#include <camera/CameraHardwareInterface.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>

namespace android {

class CameraHardwareN3ds : public CameraHardwareInterface {
public:
    virtual sp<IMemoryHeap> getPreviewHeap() const;
    virtual sp<IMemoryHeap> getRawHeap() const;

    virtual void        setCallbacks(notify_callback notify_cb,
                                     data_callback data_cb,
                                     data_callback_timestamp data_cb_timestamp,
                                     void* user);

    virtual void        enableMsgType(int32_t msgType);
    virtual void        disableMsgType(int32_t msgType);
    virtual bool        msgTypeEnabled(int32_t msgType);

    virtual status_t    startPreview();
    virtual void        stopPreview();
    virtual bool        previewEnabled();

    virtual status_t    startRecording();
    virtual void        stopRecording();
    virtual bool        recordingEnabled();
    virtual void        releaseRecordingFrame(const sp<IMemory>& mem);

    virtual status_t    autoFocus();
    virtual status_t    cancelAutoFocus();
    virtual status_t    takePicture();
    virtual status_t    cancelPicture();
    virtual status_t    dump(int fd, const Vector<String16>& args) const;
    virtual status_t    setParameters(const CameraParameters& params);
    virtual CameraParameters  getParameters() const;
    virtual status_t    sendCommand(int32_t command, int32_t arg1,
                                    int32_t arg2);
    virtual void release();

    /* sensor: CTR_CAM_OUTER_RIGHT, CTR_CAM_INNER or CTR_CAM_OUTER_LEFT */
    static sp<CameraHardwareInterface> createInstance(int sensor);

private:
                        CameraHardwareN3ds(int sensor);
    virtual             ~CameraHardwareN3ds();

    static wp<CameraHardwareInterface> singleton;

    static const int kBufferCount = 4;

    class PreviewThread : public Thread {
        CameraHardwareN3ds* mHardware;
    public:
        PreviewThread(CameraHardwareN3ds* hw) :
#ifdef SINGLE_PROCESS
            Thread(true),
#else
            Thread(false),
#endif
              mHardware(hw) { }
        virtual void onFirstRef() {
            run("CameraPreviewThread", PRIORITY_URGENT_DISPLAY);
        }
        virtual bool threadLoop() {
            mHardware->previewThread();
            return true;
        }
    };

    void initDefaultParameters();

    int                 mSensor;        /* default sensor of this instance */
    volatile bool       mPreviewStopping; /* takePicture() asked the preview thread to stop */
    static int          sSingletonSensor;
    void initHeapLocked();
    void stopPreviewThread();

    int previewThread();
    void postPreview(const sp<MemoryBase>& buffer, int slot);

    static int beginAutoFocusThread(void *cookie);
    int autoFocusThread();

    static int beginPictureThread(void *cookie);
    int pictureThread();
    bool captureStill(int cam, int w, int h, int *idx);

    /* /dev/ctr_cam, all under mDevLock */
    bool ensureStreamLocked(int cam, int w, int h, bool *restarted);
    void stopStreamLocked();
    void setErrorLocked(const char *what, int err);
    static int readFrame(int fd, uint8_t *dst, int w, int h);

    /* pixels */
    void loadOrderOverride();
    void decideOrder(const uint8_t *yuyv, int w, int h);
    void applyOrder(bool lumaOdd, bool swapUV);
    void toRgb565(const uint8_t *yuyv, int sw, int sh,
                  uint16_t *dst, int dw, int dh) const;
    void drawStatus(uint16_t *dst, int w, int h,
                    const char *l1, const char *l2, const char *l3) const;
    status_t encodeJpeg(const uint8_t *yuyv, int w, int h, int quality,
                        uint8_t *out, size_t cap, size_t *len) const;

    mutable Mutex       mLock;

    CameraParameters    mParameters;

    sp<MemoryHeapBase>  mPreviewHeap;
    sp<MemoryHeapBase>  mRawHeap;
    sp<MemoryBase>      mBuffers[kBufferCount];
    int                 mPreviewFrameSize;
    int                 mRawW, mRawH;

    sp<PreviewThread>   mPreviewThread;

    notify_callback     mNotifyCb;
    data_callback       mDataCb;
    data_callback_timestamp mDataCbTimestamp;
    void               *mCallbackCookie;

    int32_t             mMsgEnabled;
    int                 mCurrentPreviewFrame;

    /* device state, protected by mDevLock */
    Mutex               mDevLock;
    int                 mFd;
    bool                mStreamOn;
    int                 mStreamCam, mStreamW, mStreamH;
    nsecs_t             mStreamStart;
    bool                mStreamSawFrame;
    nsecs_t             mLastOpenTry;
    nsecs_t             mLastStartFail;
    int                 mLastErrno;
    char                mLastErrWhat[16];

    /* two max-size YUYV frames; mLast* names the newest complete one
     * (protected by mLock) */
    uint8_t            *mYuyv[2];
    int                 mFill;
    bool                mLastValid;
    int                 mLastIdx, mLastW, mLastH;

    /* byte offsets inside one 4-byte macropixel */
    int                 mOffY0, mOffY1, mOffU, mOffV;
    bool                mOrderLocked;
    int                 mOrderCand, mOrderVotes;
    bool                mSwapUV;

    /* counters, preview thread only */
    unsigned            mFramesOk, mTimeouts, mBadSize;
    unsigned            mConsecTimeouts;
    nsecs_t             mLastReport;
};

}; // namespace android

#endif
