# PortHelper - bring-up helper (runs as root via app_process)
#
# This helper does two things:
#
#   1. Dismiss the keyguard.  The PhoneWindowManager refuses HOME while
#      KeyguardViewMediator reports the keyguard showing, and on this port the
#      lock screen is never drawn but can still be "showing", so the physical
#      Home button appeared dead.  IWindowManager.disableKeyguard() clears it
#      and the normal framework HOME handling takes over from there - this
#      helper does not launch anything itself.
#      (Needs DISABLE_KEYGUARD; this helper runs as root.)
#
#   2. Keep the screen awake.  This port has no display power management, so
#      PowerManagerService runs its normal ~60 s screen-off timer and Android
#      then drops touch/keys.  A SCREEN_BRIGHT_WAKE_LOCK plus a periodic
#      IPowerManager.userActivity() resets it.
#
# It does *not* touch the status bar / notification shade: touch works, so
# the shade is allowed to behave normally.
#
# Diagnostics go to logcat (tag "PortHelper"), i.e. android-log.txt, because
# System.out is buffered and the supervisor keeps this process alive forever.
#
# Run with:
#   CLASSPATH=/bin/porthelper.jar BOOTCLASSPATH=... \
#       app_process /system/bin com.cyano3ds.PortHelper

.class public Lcom/cyano3ds/PortHelper;
.super Ljava/lang/Object;
.source "PortHelper.java"

.field private static sToken:Landroid/os/IBinder;


.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method


.method public static main([Ljava/lang/String;)V
    .registers 8

    # binder token held for the lifetime of disableKeyguard()
    new-instance v0, Landroid/os/Binder;
    invoke-direct {v0}, Landroid/os/Binder;-><init>()V
    sput-object v0, Lcom/cyano3ds/PortHelper;->sToken:Landroid/os/IBinder;

    # --- wait for the window manager ---
    :wait_window
    const-string v1, "window"
    invoke-static {v1}, Landroid/os/ServiceManager;->getService(Ljava/lang/String;)Landroid/os/IBinder;
    move-result-object v1
    if-nez v1, :window_ok
    const-wide/16 v2, 0x3e8
    invoke-static {v2, v3}, Ljava/lang/Thread;->sleep(J)V
    goto :wait_window

    :window_ok
    invoke-static {v1}, Landroid/view/IWindowManager$Stub;->asInterface(Landroid/os/IBinder;)Landroid/view/IWindowManager;
    move-result-object v5

    # --- power manager (best effort) ---
    const/4 v7, 0x0
    const-string v1, "power"
    invoke-static {v1}, Landroid/os/ServiceManager;->getService(Ljava/lang/String;)Landroid/os/IBinder;
    move-result-object v1
    if-eqz v1, :get_power_done
    invoke-static {v1}, Landroid/os/IPowerManager$Stub;->asInterface(Landroid/os/IBinder;)Landroid/os/IPowerManager;
    move-result-object v7
    :get_power_done

    # --- hold a SCREEN_BRIGHT_WAKE_LOCK for the life of this process ---
    # userActivity() alone is NOT enough: PowerManagerService calls
    # enableUserActivity(false) whenever the keyguard shows, and then every
    # userActivity() is ignored.  A SCREEN_BRIGHT_WAKE_LOCK puts SCREEN_BRIGHT
    # into mWakeLockState and keeps the panel on regardless.  It is released
    # automatically when sToken dies (i.e. if this VM is reaped).
    if-eqz v7, :wake_done
    :try_wake
    const/16 v6, 0xa
    const-string v2, "3ds-port"
    sget-object v0, Lcom/cyano3ds/PortHelper;->sToken:Landroid/os/IBinder;
    const/4 v3, 0x0
    invoke-interface {v7, v6, v0, v2, v3}, Landroid/os/IPowerManager;->acquireWakeLock(ILandroid/os/IBinder;Ljava/lang/String;Landroid/os/WorkSource;)V
    :try_wake_end
    .catch Ljava/lang/Throwable; {:try_wake .. :try_wake_end} :wake_err
    const-string v1, "PortHelper"
    const-string v0, "SCREEN_BRIGHT_WAKE_LOCK acquired"
    invoke-static {v1, v0}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I
    goto :wake_done

    :wake_err
    move-exception v2
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const-string v1, "acquireWakeLock failed: "
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v2}, Ljava/lang/Throwable;->toString()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    const-string v1, "PortHelper"
    invoke-static {v1, v0}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I
    :wake_done

    # --- log whether the keyguard is restricting input ---
    :try_q
    invoke-interface {v5}, Landroid/view/IWindowManager;->inKeyguardRestrictedInputMode()Z
    move-result v4
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const-string v1, "keyguardRestrictedInput="
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v0, v4}, Ljava/lang/StringBuilder;->append(Z)Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    const-string v1, "PortHelper"
    invoke-static {v1, v0}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I
    :try_q_end
    .catch Ljava/lang/Throwable; {:try_q .. :try_q_end} :q_err
    goto :do_kg

    :q_err
    move-exception v2
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const-string v1, "keyguard state query failed: "
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v2}, Ljava/lang/Throwable;->toString()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    const-string v1, "PortHelper"
    invoke-static {v1, v0}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I

    # --- dismiss the keyguard ---
    :do_kg
    :try_kg
    sget-object v2, Lcom/cyano3ds/PortHelper;->sToken:Landroid/os/IBinder;
    const-string v3, "3ds-port"
    invoke-interface {v5, v2, v3}, Landroid/view/IWindowManager;->disableKeyguard(Landroid/os/IBinder;Ljava/lang/String;)V
    :try_kg_end
    .catch Ljava/lang/Throwable; {:try_kg .. :try_kg_end} :kg_err
    const-string v1, "PortHelper"
    const-string v0, "keyguard disabled"
    invoke-static {v1, v0}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I
    goto :hold

    :kg_err
    move-exception v2
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const-string v1, "keyguard disable FAILED: "
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v2}, Ljava/lang/Throwable;->toString()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    const-string v1, "PortHelper"
    invoke-static {v1, v0}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I

    # --- keep the screen awake ---
    :hold
    # If the keyguard came back (e.g. after one screen timeout) it gates
    # userActivity, so disable it again.  The SCREEN_BRIGHT_WAKE_LOCK should
    # prevent the timeout, but this makes recovery immediate if it ever slips.
    if-eqz v5, :hold_pm
    :try_kg2
    invoke-interface {v5}, Landroid/view/IWindowManager;->inKeyguardRestrictedInputMode()Z
    move-result v4
    if-eqz v4, :hold_pm
    sget-object v2, Lcom/cyano3ds/PortHelper;->sToken:Landroid/os/IBinder;
    const-string v3, "3ds-port"
    invoke-interface {v5, v2, v3}, Landroid/view/IWindowManager;->disableKeyguard(Landroid/os/IBinder;Ljava/lang/String;)V
    :try_kg2_end
    .catch Ljava/lang/Throwable; {:try_kg2 .. :try_kg2_end} :kg2_err
    goto :hold_pm

    :kg2_err
    move-exception v4

    :hold_pm
    if-nez v7, :hold_sleep
    :try_ua
    invoke-static {}, Landroid/os/SystemClock;->uptimeMillis()J
    move-result-wide v2
    const/4 v4, 0x0
    invoke-interface {v7, v2, v3, v4}, Landroid/os/IPowerManager;->userActivity(JZ)V
    :try_ua_end
    .catch Ljava/lang/Throwable; {:try_ua .. :try_ua_end} :ua_err
    goto :hold_sleep

    :ua_err
    move-exception v2

    :hold_sleep
    const-wide/16 v2, 0x1388
    invoke-static {v2, v3}, Ljava/lang/Thread;->sleep(J)V
    goto :hold
.end method
