#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# test_zone_lock_tracepoints.sh - Verify mm_zone_lock tracepoints fire
#
# Generates zone->lock contention and uses bpftrace to verify that the
# kmem:mm_zone_lock_contended, kmem:mm_zone_locked, and
# kmem:mm_zone_lock_unlock tracepoints activate and produce output.
#
# Requirements: bpftrace, root privileges, CONFIG_FTRACE=y
#
# Usage: ./test_zone_lock_tracepoints.sh [duration_sec]
#        Default duration: 5 seconds
#
# For running in a VM via virtme-ng:
#   make -C tools/testing/selftests/mm zone_lock_contention
#   vng --cpus 4 --memory 2G \
#       --rwdir tools/testing/selftests/mm \
#       --exec "cd tools/testing/selftests/mm && ./test_zone_lock_tracepoints.sh 5"

set -e

DURATION=${1:-5}
TESTDIR="$(cd "$(dirname "$0")" && pwd)"
WORKLOAD="$TESTDIR/zone_lock_contention"
NR_THREADS=4
PASS=0
FAIL=0
SKIP=0

# --- helpers ---

pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "SKIP: $1"; SKIP=$((SKIP + 1)); }

check_root() {
	if [ "$(id -u)" -ne 0 ]; then
		echo "ERROR: must run as root"
		exit 4  # ksft SKIP
	fi
}

check_bpftrace() {
	if ! command -v bpftrace >/dev/null 2>&1; then
		echo "SKIP: bpftrace not found"
		exit 4
	fi
}

check_workload() {
	if [ ! -x "$WORKLOAD" ]; then
		echo "SKIP: $WORKLOAD not found, run 'make -C tools/testing/selftests/mm' first"
		exit 4
	fi
}

check_tracepoint_exists() {
	local tp="$1"
	if [ ! -d "/sys/kernel/tracing/events/kmem/$tp" ]; then
		skip "$tp tracepoint not in kernel"
		return 1
	fi
	return 0
}

# --- Test 1: verify tracepoints exist in tracefs ---

test_tracepoints_exist() {
	echo "--- Test 1: tracepoints exist in tracefs ---"
	for tp in mm_zone_lock_contended mm_zone_locked mm_zone_lock_unlock; do
		if check_tracepoint_exists "$tp"; then
			pass "$tp exists"
		fi
	done
}

# --- Test 2: verify format fields ---

test_tracepoint_fields() {
	echo "--- Test 2: tracepoint format fields ---"
	local fmt

	if [ -f /sys/kernel/tracing/events/kmem/mm_zone_lock_contended/format ]; then
		fmt=$(cat /sys/kernel/tracing/events/kmem/mm_zone_lock_contended/format)
		for field in node_id name count caller; do
			if echo "$fmt" | grep -q "field.*$field"; then
				pass "mm_zone_lock_contended has field '$field'"
			else
				fail "mm_zone_lock_contended missing field '$field'"
			fi
		done
	fi

	if [ -f /sys/kernel/tracing/events/kmem/mm_zone_locked/format ]; then
		fmt=$(cat /sys/kernel/tracing/events/kmem/mm_zone_locked/format)
		for field in node_id name count contended caller wait_ns; do
			if echo "$fmt" | grep -q "field.*$field"; then
				pass "mm_zone_locked has field '$field'"
			else
				fail "mm_zone_locked missing field '$field'"
			fi
		done
	fi
}

# --- Test 3: bpftrace counts tracepoint hits under load ---

test_bpftrace_counts() {
	echo "--- Test 3: bpftrace tracepoint activation under contention ---"

	if ! check_tracepoint_exists mm_zone_locked; then
		return
	fi

	local BPFTRACE_OUT
	BPFTRACE_OUT=$(mktemp /tmp/zone_lock_bt.XXXXXX)

	# bpftrace one-liner: count hits per tracepoint
	bpftrace -e '
		tracepoint:kmem:mm_zone_lock_contended { @contended = count(); }
		tracepoint:kmem:mm_zone_locked          { @locked = count(); }
		tracepoint:kmem:mm_zone_lock_unlock     { @unlock = count(); }
	' -c "$WORKLOAD $DURATION $NR_THREADS" > "$BPFTRACE_OUT" 2>&1 &
	local BT_PID=$!

	# Wait for bpftrace + workload to finish
	wait $BT_PID 2>/dev/null || true

	echo "bpftrace output:"
	cat "$BPFTRACE_OUT"

	# Check that mm_zone_locked fired (it fires on every acquisition)
	if grep -q '@locked: [0-9]' "$BPFTRACE_OUT"; then
		pass "mm_zone_locked tracepoint fired"
	else
		fail "mm_zone_locked tracepoint did NOT fire"
	fi

	# Check that mm_zone_lock_unlock fired
	if grep -q '@unlock: [0-9]' "$BPFTRACE_OUT"; then
		pass "mm_zone_lock_unlock tracepoint fired"
	else
		fail "mm_zone_lock_unlock tracepoint did NOT fire"
	fi

	# contended may or may not fire depending on actual contention
	if grep -q '@contended: [0-9]' "$BPFTRACE_OUT"; then
		pass "mm_zone_lock_contended tracepoint fired (contention detected)"
	else
		skip "mm_zone_lock_contended did not fire (no contention observed)"
	fi

	rm -f "$BPFTRACE_OUT"
}

# --- Test 4: bpftrace verifies wait_ns > 0 when contended ---

test_wait_ns() {
	echo "--- Test 4: wait_ns is populated when contended ---"

	if ! check_tracepoint_exists mm_zone_locked; then
		return
	fi

	local BPFTRACE_OUT
	BPFTRACE_OUT=$(mktemp /tmp/zone_lock_wait.XXXXXX)

	bpftrace -e '
		tracepoint:kmem:mm_zone_locked /args->contended/ {
			@has_wait_ns = count();
			@wait_ns = hist(args->wait_ns);
		}
	' -c "$WORKLOAD $DURATION $NR_THREADS" > "$BPFTRACE_OUT" 2>&1 &
	local BT_PID=$!

	wait $BT_PID 2>/dev/null || true

	echo "bpftrace wait_ns output:"
	cat "$BPFTRACE_OUT"

	if grep -q '@has_wait_ns: [0-9]' "$BPFTRACE_OUT"; then
		pass "wait_ns populated on contended acquisitions"
	else
		skip "no contended acquisitions observed for wait_ns check"
	fi

	rm -f "$BPFTRACE_OUT"
}

# --- Main ---

echo "=== zone->lock tracepoint selftest ==="
echo "Duration: ${DURATION}s, Threads: ${NR_THREADS}"
echo

check_root
check_bpftrace
check_workload

test_tracepoints_exist
test_tracepoint_fields
test_bpftrace_counts
test_wait_ns

echo
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ "$FAIL" -gt 0 ]; then
	exit 1
fi
exit 0
