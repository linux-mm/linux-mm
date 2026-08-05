#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Functional test for the DAMOS_SPLIT action.
#
# A child process allocates a MADV_HUGEPAGE-backed anonymous region and
# faults it in so that it is backed by (m)THPs.  The parent then runs a
# DAMON/DAMOS scheme with action 'split' and target_order 0 against the
# child and checks that the huge pages are split into base pages, i.e. the
# child's AnonHugePages (as reported by /proc/<pid>/smaps) drops.

import ctypes
import os
import signal
import sys
import time

import _damon_sysfs

PMD_SIZE = 2 * 1024 * 1024
MADV_HUGEPAGE = 14
PROT_READ_WRITE = 0x1 | 0x2
MAP_PRIVATE_ANON = 0x2 | 0x20
REGION_SIZE = 32 * PMD_SIZE

def child_workload():
    '''Allocate a PMD-aligned, THP-backed region, fault it in, then idle.'''
    libc = ctypes.CDLL('libc.so.6', use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                          ctypes.c_int, ctypes.c_int, ctypes.c_long]
    libc.madvise.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]

    # Over-allocate so that a PMD-aligned window is available.
    raw = libc.mmap(None, REGION_SIZE + PMD_SIZE, PROT_READ_WRITE,
                    MAP_PRIVATE_ANON, -1, 0)
    if raw is None or raw == ctypes.c_void_p(-1).value:
        os._exit(2)
    base = (raw + PMD_SIZE - 1) & ~(PMD_SIZE - 1)
    libc.madvise(ctypes.c_void_p(base), REGION_SIZE, MADV_HUGEPAGE)

    buf = (ctypes.c_char * REGION_SIZE).from_address(base)
    for off in range(0, REGION_SIZE, 4096):
        buf[off] = 1

    # Ready; idle until the parent tears us down.
    signal.pause()

def anon_huge_kb(pid):
    total = 0
    try:
        with open('/proc/%d/smaps' % pid) as f:
            for line in f:
                if line.startswith('AnonHugePages:'):
                    total += int(line.split()[1])
    except FileNotFoundError:
        return -1
    return total

def main():
    if not os.path.exists('/sys/kernel/mm/transparent_hugepage/enabled'):
        print('SKIP: transparent hugepage is not available')
        exit(0)

    pid = os.fork()
    if pid == 0:
        child_workload()
        os._exit(0)

    try:
        # Give the child time to fault in its huge pages.
        time.sleep(2)
        before = anon_huge_kb(pid)
        if before <= 0:
            print('SKIP: workload did not get any THP (AnonHugePages=%d)'
                  % before)
            os.kill(pid, signal.SIGKILL)
            exit(0)

        # Split every large folio in the target down to order-0 base pages.
        kdamonds = _damon_sysfs.Kdamonds([_damon_sysfs.Kdamond(
            contexts=[_damon_sysfs.DamonCtx(
                ops='vaddr',
                targets=[_damon_sysfs.DamonTarget(pid=pid)],
                schemes=[_damon_sysfs.Damos(
                    action='split',
                    target_order=0,
                    # match every region regardless of access/age/size, so
                    # the ARM64 stale-TLB blind spot cannot mask the target
                    access_pattern=_damon_sysfs.DamosAccessPattern(
                        size=[0, 2**64 - 1],
                        nr_accesses=[0, 2**64 - 1],
                        age=[0, 2**64 - 1]),
                    apply_interval_us=0)])])])
        err = kdamonds.start()
        if err is not None:
            print('kdamonds start failed: %s' % err)
            os.kill(pid, signal.SIGKILL)
            exit(1)

        # Let the scheme find and split the regions.
        after = before
        for _ in range(50):
            time.sleep(0.2)
            after = anon_huge_kb(pid)
            if after == 0:
                break

        kdamonds.stop()
        os.kill(pid, signal.SIGKILL)

        if after >= before:
            print('FAIL: AnonHugePages did not shrink: before=%d KiB '
                  'after=%d KiB' % (before, after))
            exit(1)
        print('PASS: AnonHugePages %d KiB -> %d KiB after DAMOS_SPLIT'
              % (before, after))
    finally:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

if __name__ == '__main__':
    main()
