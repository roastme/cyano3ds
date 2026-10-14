#!/usr/bin/env python3
"""Lid sleep must hold on real hardware (2026-10-14 fix).

The first real-console boot slept and woke ~20 ms later, every 0.7 s, with
wake reason "MCU": the MCU's LCD/backlight acknowledgement interrupts
(bits 24-29), which the kernel never masks, arrived after pm.c's last GPIO
check.  ctr_lid.c now masks every MCU interrupt but "shell opened" for the
duration of the sleep, clears what is pending, and restores the mask on
resume.  It also runs the /data save hook before sleeping and no longer gives
up when a suspend is refused.

Later the same day the first sleep that really reached PDN power-down on
hardware never came back (dark, power LED in sleep, forced power-off).  The
default is now a screen-off sleep: backlights off, CPU in WFI with the TWD
as heartbeat, woken by reading the MCU line; PDN sleep is ctr_deep_sleep=1.
And the Wi-Fi driver's suspend cut the AR6014's power, timed out on SDIO and
left Wi-Fi dead, so the chip is now kept powered (MMC_PM_KEEP_POWER).
"""
import hashlib
import shutil
import tempfile
from t3ds import *

lid = read(KERNEL / "drivers/platform/nintendo3ds/ctr_lid.c")

check("#define MCU_REG_IRQ_MASK\t0x18" in lid, "MCU interrupt mask register (0x18) not used")
check("#define  MCU_IRQ_SHELL_OPEN\tBIT(6)" in lid, "shell-open bit is not BIT(6)")
check("u32 sleep_mask = ~(u32)MCU_IRQ_SHELL_OPEN;" in lid,
      "suspend does not mask everything but shell-open")
# order inside suspend: mask first, then the LCD writes that cause the acks,
# then clear pending, then tell the MCU we sleep
check(order(lid, "static int ctr_lid_suspend", "sleep_mask", "MCU_LCD_TOP_BL_OFF",
            "if (ctr_pm_sleep_is_deep())", "MCU_LCD_PWR_OFF", "MCU_REG_IRQ_STAT + i",
            "if (ctr_pm_sleep_is_deep())", "MCU_SYS_PWR_SLEEP"),
      "ctr_lid_suspend(): mask -> backlights off -> (deep: LCD off) -> clear pending -> (deep: SYS_PWR)")
check(order(lid, "static int ctr_lid_resume", "if (ctr_pm_sleep_is_deep())", "MCU_LCD_PWR_ON",
            "MCU_LCD_TOP_BL_ON", "MCU_REG_IRQ_STAT + 3", "MCU_REG_IRQ_MASK + i", "lid->irq_mask[i]"),
      "ctr_lid_resume(): mask not restored after the backlights are back on")

pm = read(KERNEL / "arch/arm/mach-ctr/pm.c")
check("static bool ctr_deep_sleep;" in pm and "core_param(ctr_deep_sleep, ctr_deep_sleep, bool, 0644);" in pm,
      "PDN sleep is not opt-in (ctr_deep_sleep) any more")
check(order(pm, "static int ctr_pm_enter(suspend_state_t state)", "GPIO3_MCU_IRQ", "return -EBUSY;",
            "if (!ctr_deep_sleep)", "return ctr_pm_enter_light();", "ctr_gfx_sleep();"),
      "ctr_pm_enter(): screen-off sleep is not the default path")
light = pm[pm.index("static int ctr_pm_enter_light(void)"):pm.index("static int ctr_pm_enter(suspend_state_t state)")]
check(order(light, "TWD_CTRL_EN | TWD_CTRL_AUTO_RELOAD | TWD_CTRL_IRQ_EN",
            "while (readw(ctr_gpio3) & GPIO3_MCU_IRQ)", "cpu_do_idle();", "TWD_INTSTAT",
            "writel(ctrl, ctr_twd + TWD_CTRL);"),
      "screen-off sleep: no TWD heartbeat, or the MCU line is not polled after each WFI")
check("PDN_CNT" not in light and "ctr_gfx_sleep" not in light, "screen-off sleep touches PDN/graphics")

hif = read(KERNEL / "drivers/staging/ath6k_legacy/hif/sdio/linux_sdio/src/hif.c")
check(order(hif, "static int ath6kl_hifdev_suspend", "MMC_PM_KEEP_POWER",
            "sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER)", "return 0;", "deviceSuspendHandler"),
      "Wi-Fi suspend does not keep the chip powered before the power-cut path")
check(order(hif, "static int ath6kl_hifdev_resume", "device->kept_power", "return 0;", "deviceResumeHandler"),
      "Wi-Fi resume runs the power-cut resume for a kept-powered chip")
sdhc = read(KERNEL / "drivers/platform/nintendo3ds/ctr_sdhc.c")
check("mmc->pm_caps |= MMC_PM_KEEP_POWER;" in sdhc, "3ds-sdhc does not offer MMC_PM_KEEP_POWER")
check(order(sdhc, "static int ctr_sdhc_pm_resume", "ctr_sdhc_reset(host);",
            "__ctr_sdhc_set_ios(host->mmc, &host->mmc->ios);", "ctr_sdhc_sdio_arm(host, true);"),
      "3ds-sdhc resume does not restore clock/bus width and the SDIO poll")
cfg = read(KERNEL / "drivers/staging/ath6k_legacy/os/linux/cfg80211.c")
scan = cfg[cfg.index("ar6k_cfg80211_scan_node(void *arg, bss_t *ni)"):]
scan = scan[:scan.index("\n}\n")]
check("signal, GFP_ATOMIC);" in scan and "GFP_KERNEL" not in scan,
      "scan results are reported with GFP_KERNEL under the node-table lock")
check("irq_mask_saved" in lid, "resume must only restore a mask suspend saved")
check("enable_irq_wake" not in lid.split("*/", 1)[1].replace("No enable_irq_wake()", ""),
      "enable_irq_wake() is back (GPIO3 parent cannot wake: 'Unbalanced IRQ wake')")

# retrying instead of giving up with the screens lit
check("LID_SLEEP_RETRIES" not in lid, "sleep work gives up again after a few refusals")
check("msleep(tries <= LID_SLEEP_WARNINGS ? 1000 : 5000);" in lid, "retry back-off missing")

# the pre-sleep hook
check("call_usermodehelper(path, argv, envp, UMH_WAIT_PROC)" in lid, "sleep hook not run")
check(order(lid, "static void ctr_lid_sleep_work", "ctr_lid_run_hook(lid);",
            "pm_suspend(PM_SUSPEND_MEM)"), "hook must run before the first suspend")
check("&dev_attr_sleep_hook.attr" in lid, "sleep_hook sysfs attribute missing")
check("buf[0] != '/'" in lid, "sleep_hook accepts relative paths")

# the init sets the hook and the hook bounds its own time
init = read(SCRIPTS / "mkinitramfs.sh")
hook = heredoc(init, "lid-sleep-hook")
check("echo /bin/lid-sleep-hook > \"$f\"" in init, "init does not install the sleep hook")
check("[ $i -lt 20 ]" in hook, "lid-sleep-hook no longer bounds the save to 20 s")
check("exit 0" in hook, "lid-sleep-hook must always let the console sleep")
# armed only after Android booted (lid shut during boot must not sleep)
check("lid->sleep_on_close = false;" in lid and "lid->sleep_on_close = true;" not in lid,
      "lid sleep is armed at probe again")
probe = lid[lid.index("static int ctr_lid_probe"):lid.index("static int ctr_lid_remove")]
check("queue_work" not in probe, "probe sleeps right away when booted with the lid shut")
check(order(init, 'grep -q "PowerManagerService: bootCompleted" /mnt/probe/android-log.txt',
            "sleep 15", "*mcu-lid*/sleep_on_close", 'echo 1 > "$f"'),
      "init does not arm lid sleep 15 s after Android boot completes")
check(order(hook, "cp /mnt/probe/$f /mnt/sd/CYANO3DS/$f", "lid-sleep-hook: done, suspending", "sync"),
      "lid-sleep-hook does not put the logs on the card before sleeping")

# add-ctr-sleep.py is idempotent on the real tree
if (KERNEL_TREE / "arch/arm/mach-ctr/platsmp.c").exists():
    files = ["arch/arm/mach-ctr/Makefile", "arch/arm/mach-ctr/include/mach/platsmp.h",
             "arch/arm/mach-ctr/platsmp.c", "drivers/platform/nintendo3ds/Kconfig",
             "drivers/platform/nintendo3ds/Makefile", "arch/arm/boot/dts/nintendo3ds.dtsi"]
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        for f in files:
            (td / f).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(KERNEL_TREE / f, td / f)
        h = lambda: [hashlib.sha256((td / f).read_bytes()).hexdigest() for f in files]
        r1 = run(["python3", str(SCRIPTS / "add-ctr-sleep.py"), str(td)])
        before = h()
        r2 = run(["python3", str(SCRIPTS / "add-ctr-sleep.py"), str(td)])
        check(r1.returncode == 0 and r2.returncode == 0,
              "add-ctr-sleep.py failed: " + (r1.stderr + r2.stderr).strip())
        check(before == h(), "add-ctr-sleep.py is not idempotent")
done()
