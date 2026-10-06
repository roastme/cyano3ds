/*
 * libsoundpool-stub.c - minimal drop-in replacement for the archived flavor's
 *                       /system/lib/libsoundpool.so
 *
 * Why: the stock libsoundpool.so in the SDK image is prelinked (or otherwise
 * refuses to load), so Dalvik's SoundPool.<clinit> throws UnsatisfiedLinkError.
 * That kills AudioService, so AudioManager.mService is null and
 * StatusBarPolicy.installIcons() NPEs in getRingerMode() - which is what stops
 * the status bar layer from ever existing, so SurfaceFlinger never composites
 * the bottom screen.
 *
 * We do not need audio.  This stub exports the exact set of JNI natives that
 * android.media.SoundPool declares (names/signatures taken from the real
 * library), each a no-op, plus JNI_OnLoad.  Dalvik then loads the class, the
 * audio service initialises, and the status bar is created.
 *
 * Built freestanding (no libc) with:
 *   arm-linux-gnueabi-gcc -shared -fPIC -nostdlib -marm -march=armv5te \
 *       -fno-asynchronous-unwind-tables -Wl,-z,noexecstack \
 *       -o libsoundpool.so libsoundpool-stub.c
 */

typedef void *JNIEnv;
typedef void *jobject;

#define JNI_VERSION_1_4 0x00010004

int JNI_OnLoad(void *vm, void *reserved)
{
	(void)vm;
	(void)reserved;
	return JNI_VERSION_1_4;
}

int Java_android_media_SoundPool_native_1setup(JNIEnv *e, jobject o,
					       jobject weak, int a, int b, int c)
{
	(void)e; (void)o; (void)weak; (void)a; (void)b; (void)c;
	return 0;
}

int Java_android_media_SoundPool__1load(JNIEnv *e, jobject o,
					jobject src, int priority)
{
	(void)e; (void)o; (void)src; (void)priority;
	return 1;	/* a non-zero sample id */
}

int Java_android_media_SoundPool__1load__Ljava_lang_String_2I(
	JNIEnv *e, jobject o, jobject src, int priority)
{
	(void)e; (void)o; (void)src; (void)priority;
	return 1;
}

int Java_android_media_SoundPool__1load__Ljava_io_FileDescriptor_2JJI(
	JNIEnv *e, jobject o, jobject fd, long long off, long long len, int prio)
{
	(void)e; (void)o; (void)fd; (void)off; (void)len; (void)prio;
	return 1;
}

int Java_android_media_SoundPool_unload(JNIEnv *e, jobject o, int id)
{
	(void)e; (void)o; (void)id;
	return 1;
}

int Java_android_media_SoundPool_play(JNIEnv *e, jobject o, int id,
				      float lvol, float rvol, int prio,
				      int loop, float rate)
{
	(void)e; (void)o; (void)id; (void)lvol; (void)rvol;
	(void)prio; (void)loop; (void)rate;
	return 0;
}

int Java_android_media_SoundPool_pause(JNIEnv *e, jobject o, int id)
{
	(void)e; (void)o; (void)id;
	return 0;
}

int Java_android_media_SoundPool_resume(JNIEnv *e, jobject o, int id)
{
	(void)e; (void)o; (void)id;
	return 0;
}

int Java_android_media_SoundPool_stop(JNIEnv *e, jobject o, int id)
{
	(void)e; (void)o; (void)id;
	return 0;
}

int Java_android_media_SoundPool_setVolume(JNIEnv *e, jobject o, int id,
					   float lvol, float rvol)
{
	(void)e; (void)o; (void)id; (void)lvol; (void)rvol;
	return 0;
}

int Java_android_media_SoundPool_setPriority(JNIEnv *e, jobject o, int id,
					     int prio)
{
	(void)e; (void)o; (void)id; (void)prio;
	return 0;
}

int Java_android_media_SoundPool_setLoop(JNIEnv *e, jobject o, int id,
					 int loop)
{
	(void)e; (void)o; (void)id; (void)loop;
	return 0;
}

int Java_android_media_SoundPool_setRate(JNIEnv *e, jobject o, int id,
					 float rate)
{
	(void)e; (void)o; (void)id; (void)rate;
	return 0;
}

int Java_android_media_SoundPool_release(JNIEnv *e, jobject o)
{
	(void)e; (void)o;
	return 0;
}
