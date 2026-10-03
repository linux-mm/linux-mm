#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Stress test for HugeTLB vmemmap optimization (HVO).
#
# Phases:
#   1. accounting: allocate N hugepages, check that nr_memmap_pages +
#      nr_memmap_boot_pages drops by exactly N * (vmemmap pages per folio - 1),
#      then free them and check it returns to the baseline.
#   2. stress: churn the pool (optimize/restore) while concurrently reading
#      struct pages (/proc/kpageflags, /proc/kpagecount), compacting memory,
#      and optionally reading page_owner and offlining/onlining memory.
#   3. pte-inject: like 2, with fail_hugetlb_vmemmap_pte enabled; this hits
#      both the optimize rollback and the restore (partial HVO) paths.
#
# After each phase the pool is drained back to its original size, memmap
# accounting must be back at the phase's baseline, and the kernel log must
# not contain warnings/oopses.
#
# The fault-injection phases require CONFIG_FAIL_HUGETLB_VMEMMAP and
# CONFIG_FAULT_INJECTION_DEBUG_FS, and are skipped otherwise.

set -u

KSFT_PASS=0
KSFT_FAIL=1
KSFT_SKIP=4

size_kb=
nr=16
duration=60
prob=20
readers=4
struct_page_size=64
do_page_owner=0
do_hotplug=0

usage() {
	cat <<EOF
Usage: $0 [options]
  -s KB     hugepage size in kB (default: default hugepage size)
  -n N      number of hugepages to churn (default: $nr)
  -t SEC    duration of each stress phase (default: $duration)
  -p PCT    fault-injection probability in percent (default: $prob)
  -j N      number of struct page reader processes (default: $readers)
  -S BYTES  sizeof(struct page) (default: $struct_page_size)
  -o        also read /sys/kernel/debug/page_owner during stress
  -m        also offline/online memory blocks during stress (dissolves
            free hugepages, exercising the restore path)

For exact accounting checks, run on a hugepage size whose pool is
initially empty (e.g. no boot-time reservations of that size).
EOF
	exit $KSFT_SKIP
}

while getopts "s:n:t:p:j:S:omh" opt; do
	case $opt in
	s) size_kb=$OPTARG ;;
	n) nr=$OPTARG ;;
	t) duration=$OPTARG ;;
	p) prob=$OPTARG ;;
	j) readers=$OPTARG ;;
	S) struct_page_size=$OPTARG ;;
	o) do_page_owner=1 ;;
	m) do_hotplug=1 ;;
	*) usage ;;
	esac
done

log() { echo "# $*"; }
skip() { echo "SKIP: $*"; exit $KSFT_SKIP; }

failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
pass() { echo "PASS: $*"; }

[ "$(id -u)" -eq 0 ] || skip "must be run as root"

[ -r /proc/sys/vm/hugetlb_optimize_vmemmap ] ||
	skip "HVO not supported (no vm.hugetlb_optimize_vmemmap)"
[ "$(cat /proc/sys/vm/hugetlb_optimize_vmemmap)" = 1 ] ||
	skip "HVO disabled (vm.hugetlb_optimize_vmemmap=0)"
grep -q '^nr_memmap_pages ' /proc/vmstat ||
	skip "no nr_memmap_pages in /proc/vmstat"

[ -n "$size_kb" ] || size_kb=$(awk '/^Hugepagesize:/ {print $2}' /proc/meminfo)
hp_dir=/sys/kernel/mm/hugepages/hugepages-${size_kb}kB
[ -d "$hp_dir" ] || skip "no ${size_kb}kB hugepages"

page_size=$(getconf PAGESIZE)
vmemmap_pages=$(( size_kb * 1024 / page_size * struct_page_size / page_size ))
freed_per_folio=$(( vmemmap_pages - 1 ))
[ "$freed_per_folio" -gt 0 ] ||
	skip "${size_kb}kB hugepages are not HVO-optimizable"

dbgfs=/sys/kernel/debug
mountpoint -q $dbgfs || mount -t debugfs none $dbgfs 2>/dev/null

orig_nr=$(cat "$hp_dir/nr_hugepages")
target_nr=$(( orig_nr + nr ))
marker="hvo-stress-$$-$(date +%s)"
tmpdir=$(mktemp -d)
pids=()

log "hugepage size ${size_kb}kB, page size $page_size"
log "struct page size $struct_page_size"
log "vmemmap pages/folio: $vmemmap_pages ($freed_per_folio freed by HVO)"
log "pool: $orig_nr initially, churning between $orig_nr and $target_nr"
[ "$orig_nr" -eq 0 ] ||
	log "WARNING: pool not initially empty, accounting may be inexact"

memmap_total() {
	awk '/^nr_memmap_(boot_)?pages / {s += $2} END {print s + 0}' \
		/proc/vmstat
}

set_nr() {
	echo "$1" > "$hp_dir/nr_hugepages" 2>/dev/null
	cat "$hp_dir/nr_hugepages"
}

# Shrink the pool back to orig_nr. Restore may transiently fail (e.g. with
# fault injection active), so retry for a while.
drain() {
	local i cur surplus

	for i in $(seq 20); do
		# Pages whose vmemmap could not be restored are kept as free
		# surplus pages, which shrinking nr_hugepages does not free.
		# Writing the current size converts them back to persistent
		# pages first.
		set_nr "$(cat "$hp_dir/nr_hugepages")" > /dev/null
		cur=$(set_nr "$orig_nr")
		[ "$cur" -eq "$orig_nr" ] && return 0
		sleep 1
	done
	surplus=$(cat "$hp_dir/surplus_hugepages")
	fail "pool stuck at $cur hugepages ($surplus surplus)," \
		"expected $orig_nr"
	return 1
}

fa_dir() { echo "$dbgfs/$1"; }

fa_enable() {
	local d
	d=$(fa_dir "$1")
	echo 0 > "$d/verbose"
	echo N > "$d/task-filter"
	echo 1 > "$d/interval"
	echo 1000000 > "$d/times"
	echo "$prob" > "$d/probability"
}

# Disable and print the number of injected failures.
fa_disable() {
	local d left
	d=$(fa_dir "$1")
	echo 0 > "$d/probability"
	left=$(cat "$d/times")
	echo 0 > "$d/times"
	echo $(( 1000000 - left ))
}

check_dmesg() {
	local bad pat

	pat='WARNING:|BUG[: ]|Oops|Unable to handle kernel'
	pat+='|Internal error|KASAN:|UBSAN:'
	pat+='|list_(add|del) corruption|page dumped because'
	bad=$(dmesg | sed -n "/$marker/,\$p" | grep -E "$pat")
	if [ -n "$bad" ]; then
		fail "kernel log reports problems:"
		echo "$bad" | head -20 | sed 's/^/#   /'
	fi
}

## Workers

churn() {
	while :; do
		set_nr "$target_nr" > /dev/null
		set_nr "$orig_nr" > /dev/null
	done
}

kpage_reader() {
	while :; do
		dd if=/proc/kpageflags of=/dev/null bs=4M status=none
		dd if=/proc/kpagecount of=/dev/null bs=4M status=none
	done
}

compactor() {
	while :; do
		echo 1 > /proc/sys/vm/compact_memory
		sleep 1
	done
}

page_owner_reader() {
	while :; do
		cat $dbgfs/page_owner > /dev/null
	done
}

hotplugger() {
	local blk state removable

	while :; do
		for blk in /sys/devices/system/memory/memory*; do
			state=$(cat "$blk/state" 2>/dev/null)
			removable=$(cat "$blk/removable" 2>/dev/null || echo 1)
			[ "$state" = online ] || continue
			[ "$removable" = 1 ] || continue
			echo "$blk" >> "$tmpdir/hotplug"
			# A signal aborts a pending offline_pages().
			timeout 10 sh -c "echo offline > $blk/state" 2>/dev/null
			echo online > "$blk/state" 2>/dev/null
			sleep 1
		done
	done
}

start_workers() {
	local i

	churn & pids+=($!)
	for i in $(seq "$readers"); do
		kpage_reader & pids+=($!)
	done
	compactor & pids+=($!)
	if [ "$do_page_owner" -eq 1 ]; then
		if [ -r $dbgfs/page_owner ]; then
			page_owner_reader & pids+=($!)
		else
			log "page_owner not available, not reading it"
		fi
	fi
	[ "$do_hotplug" -eq 1 ] && { hotplugger & pids+=($!); }
}

stop_workers() {
	[ "${#pids[@]}" -gt 0 ] || return 0
	kill "${pids[@]}" 2>/dev/null
	wait "${pids[@]}" 2>/dev/null
	pids=()
}

cleanup() {
	local blk t

	stop_workers
	for t in fail_hugetlb_vmemmap_pte; do
		[ -d "$(fa_dir $t)" ] && fa_disable $t > /dev/null
	done
	if [ -f "$tmpdir/hotplug" ]; then
		sort -u "$tmpdir/hotplug" | while read -r blk; do
			[ "$(cat "$blk/state")" = online ] ||
				echo online > "$blk/state" 2>/dev/null
		done
	fi
	set_nr "$orig_nr" > /dev/null
	rm -rf "$tmpdir"
}
trap cleanup EXIT
trap 'exit $KSFT_FAIL' INT TERM

# Run a stress phase. $1: name, $2: fault attr to enable ("" for none).
stress_phase() {
	local name=$1 fa=$2 base after injected

	if [ -n "$fa" ] && [ ! -d "$(fa_dir "$fa")" ]; then
		echo "SKIP: $name (no $(fa_dir "$fa"))"
		return
	fi

	log "phase $name: ${duration}s"
	base=$(memmap_total)
	[ -n "$fa" ] && fa_enable "$fa"
	start_workers
	sleep "$duration"
	stop_workers
	if [ -n "$fa" ]; then
		injected=$(fa_disable "$fa")
		log "$name: injected $injected failures"
		[ "$injected" -gt 0 ] ||
			log "WARNING: $name: no failures injected"
	fi

	drain || return
	after=$(memmap_total)
	if [ "$after" -ne "$base" ]; then
		fail "$name: memmap pages $after after drain, expected $base"
	else
		pass "$name"
	fi
}

accounting_phase() {
	local base got added after expect

	log "phase accounting"
	base=$(memmap_total)
	got=$(set_nr "$target_nr")
	added=$(( got - orig_nr ))
	if [ "$added" -le 0 ]; then
		fail "accounting: could not allocate any ${size_kb}kB hugepages"
		return
	fi
	[ "$added" -eq "$nr" ] || log "accounting: only allocated $added of $nr"

	after=$(memmap_total)
	expect=$(( base - added * freed_per_folio ))
	if [ "$after" -ne "$expect" ]; then
		fail "accounting: memmap pages $after after optimizing" \
			"$added folios, expected $expect (baseline $base)"
	else
		pass "accounting: optimize freed $(( base - after ))" \
			"vmemmap pages"
	fi

	drain || return
	after=$(memmap_total)
	if [ "$after" -ne "$base" ]; then
		fail "accounting: memmap pages $after after restore," \
			"expected $base"
	else
		pass "accounting: restore"
	fi
}

echo "$marker" > /dev/kmsg

accounting_phase
stress_phase stress ""
stress_phase pte-inject fail_hugetlb_vmemmap_pte

check_dmesg

if [ "$failures" -ne 0 ]; then
	echo "FAILED: $failures check(s)"
	exit $KSFT_FAIL
fi
echo "OK"
exit $KSFT_PASS
