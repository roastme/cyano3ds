"""Shared helpers for the port's tests (after Android3DS/Octoblimp's
scripts/test_*.py): every test is a plain script that collects failures and
exits non-zero with a list of them, so run-tests.sh can run them all.

Environment:
  KERNEL_TREE  the linux-3ds tree the port is applied to
               (default ~/p3ds/src/linux-3ds); tests that need it skip
               without it.
  INITRAMFS    a built initramfs.cpio.gz to inspect (optional).
"""
import os
import pathlib
import shutil
import subprocess
import sys

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
