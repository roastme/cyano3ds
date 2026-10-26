"""Shared helpers for the port's tests (after Android3DS/Octoblimp's
scripts/test_*.py): every test is a plain script that collects failures and
exits non-zero with a list of them, so run-tests.sh can run them all.

Environment:
  KERNEL_TREE  the linux-3ds tree the port is applied to
               (default ~/p3ds/src/linux-3ds); tests that need it skip
               without it.
  INITRAMFS    a built initramfs.cpio.gz to inspect (optional).
"""
import atexit
import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

PORT = pathlib.Path(__file__).resolve().parent.parent
REPO = PORT.parent
KERNEL = PORT / "kernel"
SCRIPTS = PORT / "scripts"
USERLAND = PORT / "userland"
KERNEL_TREE = pathlib.Path(os.environ.get(
    "KERNEL_TREE", os.path.expanduser("~/p3ds/src/linux-3ds")))

_failures = []
_name = pathlib.Path(sys.argv[0]).stem


def check(cond, message):
    if not cond:
        _failures.append(message)
    return cond


def read(path):
    return pathlib.Path(path).read_text(encoding="utf-8", errors="replace")


def order(text, *needles):
    """True when every needle occurs, in this order."""
    at = -1
    for n in needles:
        i = text.find(n, at + 1)
        if i < 0:
            return False
        at = i
    return True


def heredoc(text, name):
    """Body of the  cat > /bin/NAME <<'TAG' ... TAG  block in an init script."""
    marker = f"cat > /bin/{name} <<'"
    i = text.index(marker) + len(marker)
    tag = text[i:text.index("'", i)]
    start = text.index("\n", i) + 1
    end = text.index("\n" + tag + "\n", start)
    return text[start:end + 1]


def skip(reason):
    print(f"{_name}: SKIP ({reason})")
    sys.exit(0)


_ath6k = None


def ath6k_driver():
    """The ath6k_legacy tree as fix-wifi-octoblimp.py builds it (Octoblimp's
    base plus the Cyano3DS delta), fetched once into a temp dir."""
    global _ath6k
    if _ath6k is None:
        spec = importlib.util.spec_from_file_location(
            "fix_wifi_octoblimp", SCRIPTS / "fix-wifi-octoblimp.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        top = pathlib.Path(tempfile.mkdtemp(prefix="ath6k-test-"))
        atexit.register(shutil.rmtree, top, True)
        _ath6k = top / "ath6k_legacy"
        try:
            mod.fetch_octo_driver(_ath6k)
        except (subprocess.CalledProcessError, SystemExit) as e:
            skip("cannot fetch the ath6k base from Octoblimp: %s" % e)
    return _ath6k


def have(tool):
    return shutil.which(tool) is not None


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def done():
    if _failures:
        print(f"{_name}: FAIL")
        for f in _failures:
            print(f"  - {f}")
        sys.exit(1)
    print(f"{_name}: OK")
    sys.exit(0)
