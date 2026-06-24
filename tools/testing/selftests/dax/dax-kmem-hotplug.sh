#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Exercise the dax/kmem "state" sysfs attribute:
#   /sys/bus/dax/devices/daxX.Y/state  ->  unplugged | online | online_movable
#
# The test needs a dax device already bound to the kmem driver.
# If no suitable device is found the tests SKIP.
#
# A dax device can be provisioned with the memmap= boot param, e.g.:
#   memmap=2G!4G
#
# then, in the booted system:
#
#   ndctl create-namespace -m devdax -e namespace0.0 -f
#   daxctl reconfigure-device -N -m system-ram dax0.0   # bind kmem
#   ./dax-kmem-hotplug.sh

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh

DAX_BASE=/sys/bus/dax/devices

memtotal_kb() { awk '/^MemTotal:/ {print $2}' /proc/meminfo; }
get_state() { cat "$HP" 2>/dev/null; }
# set_state STATE -- write a state to the state attribute; returns the
# write's exit status (0 = accepted by the kernel)
set_state() { echo "$1" > "$HP" 2>/dev/null; }

find_kmem_dax() {
	local d drv
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/state" ] || continue
		drv=$(readlink "$d/driver" 2>/dev/null)
		[ "$(basename "${drv:-}")" = kmem ] || continue
		basename "$d"
		return 0
	done
	return 1
}

ktap_print_header

if [ "$UID" != 0 ]; then
	ktap_skip_all "must be run as root"
	exit "$KSFT_SKIP"
fi

DAX=$(find_kmem_dax)
if [ -z "$DAX" ]; then
	ktap_skip_all "no kmem-bound dax device with a state attribute"
	exit "$KSFT_SKIP"
fi
HP=$DAX_BASE/$DAX/state
ORIG=$(get_state)

# A failure to reach the baseline is environmental (memory in use), not an
# interface failure, so skip rather than fail.
set_state unplugged; rc=$?
if [ "$rc" != 0 ] || [ "$(get_state)" != unplugged ]; then
	ktap_skip_all "$DAX: cannot reach 'unplugged' baseline (memory in use?)"
	[ -n "$ORIG" ] && set_state "$ORIG"
	exit "$KSFT_SKIP"
fi
mt_unplugged=$(memtotal_kb)

DRV=/sys/bus/dax/drivers/kmem
AOB=/sys/devices/system/memory/auto_online_blocks

ktap_print_msg "using $DAX (initial state was: $ORIG)"
ktap_set_plan 11

set_state online; rc=$?
mt_online=$(memtotal_kb)
if [ "$rc" = 0 ] && [ "$(get_state)" = online ] && [ "$mt_online" -gt "$mt_unplugged" ]; then
	ktap_test_pass "online: state=online, MemTotal $mt_unplugged -> $mt_online kB"
else
	ktap_test_fail "online: rc=$rc state=$(get_state) MemTotal $mt_unplugged -> $mt_online"
fi

set_state online; rc=$?
if [ "$rc" = 0 ] && [ "$(get_state)" = online ]; then
	ktap_test_pass "online idempotent"
else
	ktap_test_fail "online idempotent: rc=$rc state=$(get_state)"
fi

set_state online_movable; rc=$?
if [ "$rc" != 0 ] && [ "$(get_state)" = online ]; then
	ktap_test_pass "reject online_movable without intervening unplug"
else
	ktap_test_fail "online->online_movable not rejected: rc=$rc state=$(get_state)"
fi

set_state unplugged; rc=$?
mt=$(memtotal_kb)
if [ "$rc" = 0 ] && [ "$(get_state)" = unplugged ] && [ "$mt" -lt "$mt_online" ]; then
	ktap_test_pass "unplug from online: MemTotal $mt_online -> $mt kB"
else
	ktap_test_fail "unplug from online: rc=$rc state=$(get_state) MemTotal $mt_online -> $mt"
fi

set_state online_movable; rc=$?
mt_movable=$(memtotal_kb)
if [ "$rc" = 0 ] && [ "$(get_state)" = online_movable ] && [ "$mt_movable" -gt "$mt_unplugged" ]; then
	ktap_test_pass "online_movable after unplug: MemTotal $mt_unplugged -> $mt_movable kB"
else
	ktap_test_fail "online_movable after unplug: rc=$rc state=$(get_state) MemTotal=$mt_movable"
fi

# The online -> unplug -> online_movable -> unplug cycle once regressed:
# a re-online failed to re-reserve the per-range resources, so the final unplug
# reported success while leaving the memory online.  Assert it is really freed.
set_state unplugged; rc=$?
mt=$(memtotal_kb)
if [ "$rc" != 0 ]; then
	ktap_test_skip "unplug from movable not accepted (memory in use?) rc=$rc"
elif [ "$(get_state)" = unplugged ] && [ "$mt" -lt "$mt_movable" ]; then
	ktap_test_pass "unplug from online_movable removed memory: $mt_movable -> $mt kB"
else
	ktap_test_fail "unplug from movable reported success but memory remained: state=$(get_state) MemTotal $mt_movable -> $mt"
fi

set_state online_kernel; rc=$?
mt=$(memtotal_kb)
if [ "$rc" = 0 ] && [ "$(get_state)" = online_kernel ] && [ "$mt" -gt "$mt_unplugged" ]; then
	ktap_test_pass "online_kernel: MemTotal $mt_unplugged -> $mt kB"
else
	ktap_test_fail "online_kernel: rc=$rc state=$(get_state) MemTotal=$mt"
fi
set_state unplugged

before=$(get_state)
set_state bogus_state; rc=$?
if [ "$rc" != 0 ] && [ "$(get_state)" = "$before" ]; then
	ktap_test_pass "reject invalid state string"
else
	ktap_test_fail "invalid state not rejected: rc=$rc state=$(get_state)"
fi

# Run several online/unplug cycles and require that each one adds/removes memory
set_state unplugged
cycle_ok=1; fail_i=0
for i in 1 2 3; do
	if ! set_state online; then cycle_ok=0; fail_i=$i; break; fi
	on=$(memtotal_kb)
	if ! set_state unplugged; then cycle_ok=0; fail_i=$i; break; fi
	off=$(memtotal_kb)
	if [ "$on" -le "$mt_unplugged" ] || [ "$off" -ge "$on" ]; then
		cycle_ok=0; fail_i=$i; break
	fi
done
if [ "$cycle_ok" = 1 ]; then
	ktap_test_pass "online/unplug cycle re-acquires resources (3x: memory added and freed each time)"
else
	ktap_test_fail "online/unplug cycle regressed at iteration $fail_i (on=$on off=$off baseline=$mt_unplugged)"
fi

# change system default online policy while the device is unbound, and show
# the new system default policy is utilized across bindings.
set_state unplugged
if [ -w "$AOB" ] && [ -w "$DRV/unbind" ] && [ -w "$DRV/bind" ]; then
	orig_aob=$(cat "$AOB")
	echo "$DAX" > "$DRV/unbind" 2>/dev/null
	echo offline > "$AOB" 2>/dev/null
	echo "$DAX" > "$DRV/bind" 2>/dev/null
	sleep 1
	st=$(get_state)
	echo "$orig_aob" > "$AOB" 2>/dev/null		# restore system policy
	if [ "$st" = offline ]; then
		ktap_test_pass "online policy resolved at bind: auto_online_blocks=offline -> state=offline"
	else
		ktap_test_fail "bind-time policy not honored: state=$st (expected offline)"
	fi
	set_state unplugged 2>/dev/null
else
	ktap_test_skip "auto_online_blocks or driver bind/unbind not writable"
fi

[ -n "$ORIG" ] && set_state "$ORIG"

# DESTRUCTIVE: unbinding the driver while memory is online causes the resources
# to leak - but the unbind should not deadlock.  Instead the driver leaks it
# with a single "suck online" warning. This leaves the memory online and the
# device unbound until reboot, so it runs last.
set_state unplugged; set_state online
if [ "$(get_state)" = online ] && [ -w "$DRV/unbind" ]; then
	mt_on=$(memtotal_kb)
	dmesg -C 2>/dev/null
	echo "$DAX" > "$DRV/unbind" 2>/dev/null
	mt_after=$(memtotal_kb)
	# The leaked "System RAM (kmem)" regions stay in the iomem tree; reading
	# their names dereferences res_name, which a buggy unbind already freed.
	# Walk /proc/iomem to provoke that use-after-free (caught by KASAN).
	cat /proc/iomem > /dev/null 2>&1
	splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|use-after-free|general protection|Oops|refcount_t")
	if [ "$splat" = 0 ] && [ "$mt_after" -ge "$mt_on" ]; then
		ktap_test_pass "unbind while online: memory left online, no UAF/oops (MemTotal $mt_on -> $mt_after kB)"
	else
		ktap_test_fail "unbind while online regressed: splat=$splat MemTotal $mt_on -> $mt_after kB"
	fi
else
	ktap_test_skip "could not online device for unbind-while-online test"
fi

ktap_finished
