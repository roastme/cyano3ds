/*
 * alsaprobe.c - open the port's kernel ALSA PCM and push a few periods of
 * silence through it, printing each step.  Run from the initramfs *before*
 * Android's init so a kernel-side PCM hang can be told apart from a hang in
 * CM7's hardware/alsa_sound HAL.
 *
 * Built against CM7's alsa-lib (arm-eabi-4.4.3) so it runs as a normal bionic
 * binary out of /system/bin/linker with LD_LIBRARY_PATH=/system/lib.
 */
#include <stdio.h>
#include <string.h>
#include <alsa/asoundlib.h>

#define FRAMES 2048

int main(int argc, char **argv)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	const char *dev = (argc > 1) ? argv[1] : "hw:0,0";
	unsigned int rate = 44100;
	snd_pcm_uframes_t buffer = 8192, period = FRAMES;
	short buf[FRAMES * 2];
	snd_pcm_sframes_t n;
	int err, i;

	printf("alsaprobe: opening %s ...\n", dev);
	fflush(stdout);
	err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		printf("alsaprobe: open failed: %s\n", snd_strerror(err));
		return 1;
	}
	printf("alsaprobe: opened\n");

	snd_pcm_hw_params_alloca(&hw);
	if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_access(pcm, hw,
			SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_format(pcm, hw,
			SND_PCM_FORMAT_S16_LE)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw,
			&buffer)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw,
			&period, 0)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params(pcm, hw)) < 0)
		goto fail;
	printf("alsaprobe: hw_params ok (rate=%u buffer=%lu period=%lu)\n",
	       rate, (unsigned long)buffer, (unsigned long)period);
	fflush(stdout);

	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer);
	snd_pcm_sw_params_set_avail_min(pcm, sw, period);
	snd_pcm_sw_params(pcm, sw);

	memset(buf, 0, sizeof(buf));
	printf("alsaprobe: writing 8 periods ...\n");
	fflush(stdout);
	for (i = 0; i < 8; i++) {
		n = snd_pcm_writei(pcm, buf, FRAMES);
		if (n < 0) {
			printf("alsaprobe: write %d failed: %s (recovering)\n",
			       i, snd_strerror((int)n));
			fflush(stdout);
			snd_pcm_recover(pcm, (int)n, 1);
		} else {
			printf("alsaprobe: period %d written (%ld frames)\n",
			       i, (long)n);
			fflush(stdout);
		}
	}
	printf("alsaprobe: draining ...\n");
	fflush(stdout);
	snd_pcm_drain(pcm);
	printf("alsaprobe: closing\n");
	fflush(stdout);
	snd_pcm_close(pcm);
	printf("alsaprobe: DONE ok\n");
	return 0;

fail:
	printf("alsaprobe: hw_params/step failed: %s\n", snd_strerror(err));
	snd_pcm_close(pcm);
	return 1;
}
