#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/common.sh"

cd $linux_dir

echo "Running SLUB KUnit test"
vng -- bash -c 'modprobe slub_kunit; dmesg' &> $log || fail "Failed to run test"

if grep -qE 'not ok [0-9]+ slub_test\b' $log; then
	fail "SLUB KUnit test failed"
elif grep -qE '\bok [0-9]+ slub_test\b' $log; then
	pass "SLUB KUnit test passed"
else
	fail "SLUB KUnit test did not run"
fi
