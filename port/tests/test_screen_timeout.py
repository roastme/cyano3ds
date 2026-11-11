#!/usr/bin/env python3
"""Screen timeout: the restored settings must not switch the panels off.

CM7's default screen_off_timeout is 60000; "stay on while plugged in" does
nothing on battery (the real MCU battery reports Discharging), so
PowerManagerService turns the screens off 60 s after the last touch and the
POWER button (a clean shutdown on this port) cannot bring them back.

The init writes screen_off_timeout (-1 = never, or the ms value in
CYANO3DS/screen-timeout) into the restored settings.db before Android starts.
This runs that statement against a Gingerbread-shaped settings table, so the
check is the row Android's SettingsProvider would read back - not the SQL text.
"""
import re
import sqlite3
from t3ds import *

init = read(SCRIPTS / "mkinitramfs.sh")
m = re.search(r'"(INSERT OR REPLACE INTO system [^"]*screen_off_timeout[^"]*)"', init)
check(m is not None, "no INSERT OR REPLACE of screen_off_timeout in the init")
if m:
    db = sqlite3.connect(":memory:")
    # frameworks/base/packages/SettingsProvider DatabaseHelper (gingerbread)
    db.execute("CREATE TABLE system (_id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "name TEXT UNIQUE ON CONFLICT REPLACE,value TEXT)")
    db.execute("INSERT INTO system (name, value) VALUES ('screen_off_timeout', '60000')")
    for want in ("-1", "120000"):
        sql = m.group(1).replace("$SCREEN_TIMEOUT", want)
        try:
            db.execute(sql)
        except sqlite3.Error as e:
            check(False, f"the init's screen_off_timeout SQL does not run: {e} ({sql})")
            continue
        rows = db.execute("SELECT value FROM system WHERE name='screen_off_timeout'").fetchall()
        check(rows == [(want,)], f"after the init's SQL screen_off_timeout rows are {rows}, want [({want!r},)]")
done()
