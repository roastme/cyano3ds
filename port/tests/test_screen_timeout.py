#!/usr/bin/env python3
"""Screen timeout: the panels must not go dark for good.

CM7's default screen_off_timeout is 60000; without the keep-awake PortHelper
(it needs java/smali on the build host) and with "stay on while plugged in"
doing nothing on battery, PowerManagerService turns the screens off 60 s
after the last touch and the POWER button (a clean shutdown on this port)
cannot bring them back.  The init writes screen_off_timeout (-1 = never, or
the ms value in CYANO3DS/screen-timeout) into the restored settings.db
before Android starts.  This runs the init's SQL against a
Gingerbread-shaped settings table.
"""
import re
import sqlite3
from t3ds import *

init = read(SCRIPTS / "mkinitramfs.sh")
check(order(init, 'APPS_TAR="${DATA_TAR%.tar}-apps.tar"', "SCREEN_TIMEOUT=-1",
            "/system/xbin/sqlite3", "/bin/android-init"),
      "screen_off_timeout is not set after the /data restore and before Android starts")
check('"$LOGDIR/screen-timeout"' in init, "CYANO3DS/screen-timeout override is gone")
m = re.search(r'"(INSERT OR REPLACE INTO system [^"]*)"', init)
check(m is not None, "no INSERT OR REPLACE of screen_off_timeout in the init")
if m:
    db = sqlite3.connect(":memory:")
    # frameworks/base/packages/SettingsProvider DatabaseHelper (gingerbread)
    db.execute("CREATE TABLE system (_id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "name TEXT UNIQUE ON CONFLICT REPLACE,value TEXT)")
    db.execute("INSERT INTO system (name, value) VALUES ('screen_off_timeout', '60000')")
    for want in ("-1", "120000"):
        db.execute(m.group(1).replace("$SCREEN_TIMEOUT", want))
        rows = db.execute("SELECT value FROM system WHERE name='screen_off_timeout'").fetchall()
        check(rows == [(want,)], f"after the init's SQL screen_off_timeout rows are {rows}, want [({want!r},)]")
done()
