// SPDX-License-Identifier: GPL-2.0
/*
 * pcmtest.c - drive the port's ALSA PCM through the raw kernel ioctls
 *
 * Usage: pcmtest capture [SECONDS] [RATE] [CHANNELS]
 *        pcmtest play    [SECONDS] [RATE]
 *
 * capture records from /dev/snd/pcmC0D0c (ctr_snd.c's microphone stream) and
 * prints, per half second, how many frames arrived and their peak and mean
 * absolute level.  play sends a 440 Hz tone to /dev/snd/pcmC0D0p (fails with
 * EBUSY while AudioFlinger holds the playback stream).  Exit status 0 means
 * the stream ran for the whole time and delivered about the expected number
 * of frames, so a hang or a dead stream in the driver shows up as a failure,
 * not as a stuck shell.
 *
 * Static glibc binary built with the host cross compiler; it needs no
 * alsa-lib, so it also runs from the initramfs before Android is up.  The
 * hw_params handling follows tinyalsa: every mask and interval starts fully
 * open and only the values that matter are pinned.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

static void param_init(struct snd_pcm_hw_params *p)
{
	int n;

	memset(p, 0, sizeof(*p));
	for (n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
		struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];

		m->bits[0] = ~0u;
		m->bits[1] = ~0u;
	}
	for (n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
		struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

		i->min = 0;
		i->max = ~0u;
	}
	p->rmask = ~0u;
	p->info = ~0u;
}

static void param_set_mask(struct snd_pcm_hw_params *p, int n, unsigned int bit)
{
	struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(m, 0, sizeof(*m));
	m->bits[bit >> 5] = 1u << (bit & 31);
}

static void param_set_int(struct snd_pcm_hw_params *p, int n, unsigned int v)
{
	struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	i->min = v;
	i->max = v;
	i->integer = 1;
}

static unsigned int param_get_int(struct snd_pcm_hw_params *p, int n)
{
	return p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
}

/*
 * ALSA's blocking open() waits for a busy substream instead of failing (it
 * hung on hardware and in 3Beans while AudioFlinger held the playback
 * stream), so open non-blocking and switch to blocking I/O afterwards.
 */
static int pcm_open(const char *path)
{
	int fd = open(path, O_RDWR | O_NONBLOCK);

	if (fd >= 0)
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	return fd;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int setup(int fd, unsigned int rate, unsigned int ch,
		 unsigned int *period, unsigned int *buffer)
{
	struct snd_pcm_hw_params hw;
	struct snd_pcm_sw_params sw;

	param_init(&hw);
	param_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	param_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	param_set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_FRAME_BITS, 16 * ch);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, ch);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_RATE, rate);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, *period);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_PERIODS, 4);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
		printf("pcmtest: HW_PARAMS(%u Hz, %u ch, %u-frame periods) failed: %s\n",
		       rate, ch, *period, strerror(errno));
		return -1;
	}
	*period = param_get_int(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	*buffer = param_get_int(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);

	memset(&sw, 0, sizeof(sw));
	sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sw.period_step = 1;
	sw.avail_min = *period;
	sw.start_threshold = *period;
	sw.stop_threshold = *buffer;
	sw.boundary = *buffer;
	while (sw.boundary * 2 <= 0x7fffffffu - *buffer)
		sw.boundary *= 2;
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
		printf("pcmtest: SW_PARAMS failed: %s\n", strerror(errno));
		return -1;
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		printf("pcmtest: PREPARE failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int capture(double secs, unsigned int rate, unsigned int ch)
{
	unsigned int period = rate / 50, buffer = 0;
	long total = 0, slot_frames = 0, expect;
	double t0, slot_t;
	int peak = 0, fd;
	long long sum = 0;
	short *buf;

	fd = pcm_open("/dev/snd/pcmC0D0c");
	if (fd < 0) {
		printf("pcmtest: open /dev/snd/pcmC0D0c: %s\n", strerror(errno));
		return 1;
	}
	if (setup(fd, rate, ch, &period, &buffer) < 0)
		return 1;
	printf("pcmtest: capture %u Hz, %u ch, period %u, buffer %u frames\n",
	       rate, ch, period, buffer);
	if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0) {
		printf("pcmtest: START failed: %s\n", strerror(errno));
		return 1;
	}
	buf = calloc(period, 2 * ch);
	t0 = slot_t = now();
	while (now() - t0 < secs) {
		struct snd_xferi x = { .buf = buf, .frames = period };
		unsigned int i;

		if (ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) < 0) {
			printf("pcmtest: READI failed after %ld frames: %s\n",
			       total, strerror(errno));
			if (errno == EPIPE) {
				ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
				ioctl(fd, SNDRV_PCM_IOCTL_START);
				continue;
			}
			return 1;
		}
		for (i = 0; i < x.result * ch; i++) {
			int v = abs(buf[i]);

			if (v > peak)
				peak = v;
			sum += v;
		}
		total += x.result;
		slot_frames += x.result;
		if (now() - slot_t >= 0.5) {
			printf("pcmtest:   +%.1fs %ld frames, peak %d, mean |x| %lld\n",
			       now() - t0, slot_frames, peak,
			       slot_frames ? sum / (slot_frames * ch) : 0);
			slot_t = now();
			slot_frames = 0;
			peak = 0;
			sum = 0;
		}
	}
	ioctl(fd, SNDRV_PCM_IOCTL_DROP);
	close(fd);
	expect = (long)(secs * rate);
	printf("pcmtest: captured %ld frames in %.2f s (expected ~%ld)\n",
	       total, now() - t0, expect);
	return (total > expect * 8 / 10 && total < expect * 12 / 10) ? 0 : 2;
}

static int play(double secs, unsigned int rate)
{
	unsigned int period = rate / 50, buffer = 0, i;
	long total = 0, want = (long)(secs * rate);
	double t0, ph = 0;
	short *buf;
	int fd;

	fd = pcm_open("/dev/snd/pcmC0D0p");
	if (fd < 0) {
		printf("pcmtest: open /dev/snd/pcmC0D0p: %s\n", strerror(errno));
		return 1;
	}
	if (setup(fd, rate, 2, &period, &buffer) < 0)
		return 1;
	printf("pcmtest: play 440 Hz, %u Hz, period %u, buffer %u frames\n",
	       rate, period, buffer);
	buf = calloc(period, 4);
	t0 = now();
	while (total < want) {
		struct snd_xferi x = { .buf = buf, .frames = period };

		for (i = 0; i < period; i++) {
			short v = (short)(8000 * sin(ph));

			buf[2 * i] = buf[2 * i + 1] = v;
			ph += 2 * M_PI * 440 / rate;
		}
		if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
			printf("pcmtest: WRITEI failed after %ld frames: %s\n",
			       total, strerror(errno));
			if (errno == EPIPE) {
				ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
				continue;
			}
			return 1;
		}
		total += x.result;
	}
	ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
	close(fd);
	printf("pcmtest: played %ld frames in %.2f s (expected ~%.2f s)\n",
	       total, now() - t0, secs);
	return (now() - t0 > secs * 0.8 && now() - t0 < secs * 1.5) ? 0 : 2;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "capture";
	double secs = argc > 2 ? atof(argv[2]) : 2.0;
	unsigned int rate = argc > 3 ? (unsigned int)atoi(argv[3]) : 0;
	unsigned int ch = argc > 4 ? (unsigned int)atoi(argv[4]) : 1;
	int ret;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!strcmp(mode, "play"))
		ret = play(secs, rate ? rate : 44100);
	else
		ret = capture(secs, rate ? rate : 8000, ch);
	printf("pcmtest: %s %s\n", mode, ret ? "FAILED" : "OK");
	return ret;
}
