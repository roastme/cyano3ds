#!/usr/bin/env python3
"""Audio (2026-10-14): Octoblimp's codec sequence, output re-arm, microphone.

ctr_snd.c keeps its ALSA interface (CM7's audio HAL is alsa_sound) but now
programs the hardware the way Android3DS/Octoblimp's ctr_csnd.c does, which
is verified to make sound on a real console: their codec bring-up (no second
reset, no EQ upload, both I2S lines at 32.73 kHz), their output-stage
monitor that re-arms the speaker path when it drops mid-stream, and their
MIC FIFO capture as an ALSA capture stream.
"""
from t3ds import *

snd = read(KERNEL / "drivers/platform/nintendo3ds/ctr_snd.c")
init = snd[snd.index("int ctr_snd_codec_init(struct regmap *map)\n{"):
           snd.index("EXPORT_SYMBOL_GPL(ctr_snd_codec_init);")]

check("CDC_SOFT_RST_CTR, 1" not in init, "codec init soft-resets again (Octoblimp does not)")
check("cdc_write_be16" not in snd, "EQ coefficient upload is back (not in the verified sequence)")
check(order(init, "CDC_100_67, 0x11", "CDC_101_119, 1, 1", "CDC_GPI1_GPI2_PIN_CTRL, 0x66, 0x66",
            "CDC_101_122, 1", "CDC_100_34, 0x18, 0x18", "CDC_HEADSET_SEL, 0x20, 0x30",
            "ctr_set_i2s_freq(map, 1, false)", "ctr_set_i2s_freq(map, 2, i2s2_47k)",
            "CDC_101_17, 0x10, 0x1C", "CDC_100_122, 0", "CDC_100_120, 0",
            "CDC_100_118, 0xC0, 0xC0", "CDC_100_37, 0x88, 0x88", "CDC_101_10, 0xA",
            "CDC_DAC_DATA_PATH_SETUP, 0xC0, 0xC0", "CDC_DAC_VOLUME_CTRL, 0",
            "CDC_100_119, 0, 0xC", "CDC_101_11, hpval", "CDC_101_17, 0xC0, 0xC0",
            "CDC_101_27, ANALOG_VOLUME_SP", "msleep(38)"),
      "codec init no longer follows Octoblimp's order")
check("static bool i2s2_47k;" in snd, "I2S line 2 is no longer 32.73 kHz by default")
check("I2S1_MIC_TIMING" in init, "I2S1 mic timing bit not set")
check("#define ANALOG_VOLUME_SP\t0" in snd and "#define DRIVER_GAIN_SP\t\t1" in snd,
      "speaker gains differ from Octoblimp's")

check("ctr_snd_outmon_work" in snd and "CTR_SND_OUTMON_MAX" in snd, "output-stage re-arm monitor missing")
check(order(snd, "SNDRV_PCM_TRIGGER_START", "mod_delayed_work(system_wq, &snd->outmon"),
      "monitor not started with playback")

check("snd_pcm_new(snd->card, \"3ds-pcm\", 0, 1, 1, &snd->pcm)" in snd, "no capture substream")
check("SNDRV_PCM_STREAM_CAPTURE, &ctr_snd_cap_ops" in snd, "capture ops not registered")
check("for (word = 0; word < 17; word++)" in snd, "FIFO drain no longer bounded by the FIFO depth")
check("if (cnt & MIC_CNT_FIFO_EMPTY)\n\t\t\tbreak;" in snd, "FIFO read while empty (stall hazard)")
check("MIC_CNT_FIFO_OVERRUN" in snd and "mic_hw_start(snd);" in snd, "overrun does not restart the block")
check(order(snd, "static int ctr_snd_cap_prepare", "ctr_snd_adc_on(snd);"),
      "ADC powered outside prepare (codec writes sleep; trigger is atomic)")
check("HRTIMER_MODE_REL_SOFT" in snd[snd.index("ctr_snd_cap_trigger"):], "capture timer is not a soft hrtimer")

mk = read(SCRIPTS / "mkinitramfs.sh")
check('-o "$ROOT/bin/pcmtest" "$PORT/userland/tests/pcmtest.c" -lm' in mk, "pcmtest not built into the initramfs")

# pcmtest compiles for the host too (same raw ioctls)
# Playback pointer (2026-10-14 "audio was glitchy"): the wall-clock pointer
# was up to 131 frames ahead of the CSND's real read position at every stream
# start and drifted 26 frames/s, so Android overwrote unplayed samples.
ptr = snd[snd.index("static snd_pcm_uframes_t ctr_snd_pcm_pointer"):snd.index("static int ctr_snd_copy_user")]
check("snd->pos = snd->hw_live ? hw : sw;" in ptr, "pointer does not follow the live CSND position")
check(order(ptr, "snd->hw_moved = now;", "ktime_ms_delta(ktime_get(), snd->hw_moved) > 100",
            "snd->hw_live = false;"), "no fallback to the clock when st_addr stops moving")
rc = snd[snd.index("static void ctr_snd_rate_check"):snd.index("static snd_pcm_uframes_t ctr_snd_pcm_pointer")]
check(order(rc, "snd->hw_total +=", "snd->hw_live ? 1000000 : 100000", "diff * 100 <= expect * 3",
            "snd->hw_live = true;", "snd->hw_live = false;"),
      "st_addr is trusted without checking it advances at the stream rate")
check("ctr_snd_rate_check(snd, rt, hw, now);" in ptr, "pointer does not run the st_addr rate check")
check("DIV_ROUND_CLOSEST(67027964u, sample_rate)" in snd, "CSND rate divider is truncated again")
for rate in (44100, 48000, 32000, 22050, 16000, 8000):
    div = (67027964 + rate // 2) // rate
    check(abs(67027964 / div - rate) / rate < 0.0005,
          f"CSND divider for {rate} Hz is off by more than 0.05%")

if have("gcc"):
    r = run(["gcc", "-O2", "-Wall", "-Werror", "-o", "/dev/null",
             str(USERLAND / "tests/pcmtest.c"), "-lm"])
    check(r.returncode == 0, "pcmtest.c does not build cleanly: " + r.stderr[:400])
done()
