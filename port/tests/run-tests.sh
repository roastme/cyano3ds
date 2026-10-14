#!/bin/sh
# Run the port's tests (port/tests/test_*.py).  Each prints OK, SKIP or FAIL
# with reasons.  KERNEL_TREE and INITRAMFS are passed through (see t3ds.py).
cd "$(dirname "$0")" || exit 1
fail=0
for t in test_*.py; do
	python3 "$t" || fail=$((fail + 1))
done
echo "--- $fail test file(s) failed"
exit $fail
