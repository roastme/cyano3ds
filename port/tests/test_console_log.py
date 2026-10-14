#!/usr/bin/env python3
"""Top-screen console log (2026-10-14), after Octoblimp's logcat_console.sh:
a second logger reader mirrors Android's log to /dev/tty0, warnings and up by
default (fbcon draws rotated glyphs in software; Octoblimp traced a freeze to
a verbose mirror), level from CYANO3DS/console-log, off with
CYANO3DS/noconsolelog."""
from t3ds import *

mk = read(SCRIPTS / "mkinitramfs.sh")
check('/system/bin/logcat -v time "*:$CLVL" > /dev/tty0 2>&1' in mk, "logcat mirror to /dev/tty0 missing")
check("CLVL=W\n" in mk, "default level is no longer W")
check('case "$CLVL" in V|D|I|W|E|F) ;; *) CLVL=W ;; esac' in mk, "level from the card is not validated")
check('[ ! -e "$LOGDIR/noconsolelog" ]' in mk, "no way to turn the mirror off")
check(order(mk, "while true; do", '/system/bin/logcat -v time', "sleep 5"),
      "mirror is not restarted when logcat exits")
check("busybox stty -echo -isig < /dev/console" in mk, "console echo must stay off (buttons would echo)")
done()
