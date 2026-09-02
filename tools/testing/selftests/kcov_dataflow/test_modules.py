#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
test_modules.py - run the kcov_dataflow test modules, one KTAP test each.

Every module is loaded, triggered with recording active and unloaded by
trigger-view.py's run_capture(). The records that belong to the module are
then compared with the values its trigger function passes and returns, so a
test passes only when the instrumented arguments, struct field expansions
and return values came back intact through the kcov_dataflow buffer. The
module's call tree is echoed as KTAP diagnostics.

    ./test_modules.py                 # all modules
    ./test_modules.py -t rust_ffi_contract -C 8 --vmlinux vmlinux

Modules that were not built (no CONFIG_RUST, no toolchain) are reported as
SKIP; a kernel without /sys/kernel/debug/kcov_dataflow skips everything.
"""
import argparse
import contextlib
import importlib.util
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "kselftest"))
import ksft  # noqa: E402


def _load_trigger_view():
    spec = importlib.util.spec_from_file_location(
        "trigger_view", os.path.join(HERE, "trigger-view.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


tv = _load_trigger_view()


class Check:
    """Collects expectation failures for one module."""

    def __init__(self):
        self.failures = []

    def eq(self, what, got, want):
        if got != want:
            self.failures.append(f"{what}: got {fmt(got)}, want {fmt(want)}")

    def true(self, what, cond):
        if not cond:
            self.failures.append(what)


def fmt(v):
    if isinstance(v, list):
        return "[" + ", ".join(fmt(x) for x in v) + "]"
    if isinstance(v, int):
        return f"0x{v:x}"
    return str(v)


def entries(cap, recs, func):
    return [r for r in recs if r["type"] == tv.DF_TYPE_ENTRY and func in cap.funcs(r)]


def rets(cap, recs, func):
    return [r["val"] for r in recs if r["type"] == tv.DF_TYPE_RET and func in cap.funcs(r)]


def flat_sum(n):
    """sf_n() returns a->a + b->b + ... over s1..sn: 0x11 + 0x22 + ..."""
    return sum(0x11 * k for k in range(1, n + 1))


def nested_sum(n, _memo={}):
    """
    stf_n()/stpf_n() return field0 (0x11 * n) plus the recursive sums of the
    embedded st1..st(n-1); the same values are used for the value-nested and
    the pointer-linked towers.
    """
    if n not in _memo:
        _memo[n] = 0x11 * n + sum(nested_sum(k) for k in range(1, n))
    return _memo[n]


def check_struct_family(cap, recs, c, p, flat_ns, stpf8_runs):
    """
    Shared expectations for eight_struct_args_c (p="") and
    eight_struct_args_rust (p="r"): @flat_ns are the sf_N called by the
    trigger, @stpf8_runs how often the pointer-linked tower is walked.
    """
    for n in flat_ns:
        ents = entries(cap, recs, f"{p}sf_{n}")
        c.true(f"{p}sf_{n}: ENTRY records", bool(ents))
        for k in range(n):
            # arg k is a struct s(k+1) * whose fields are 0x11, 0x22, ...
            got = [r["vals"] for r in ents if r["arg_idx"] == k]
            c.true(f"{p}sf_{n} arg[{k}]: ENTRY record", bool(got))
            for vals in got:
                c.eq(f"{p}sf_{n} arg[{k}] expanded fields", vals,
                     [0x11 * (j + 1) for j in range(k + 1)])
        # rustc may alias identical bodies (rsf_1 == rstf_1), so the RET
        # list can carry the alias's calls too: check every value.
        got = rets(cap, recs, f"{p}sf_{n}")
        c.true(f"{p}sf_{n} RET values all {fmt(flat_sum(n))}: {fmt(got)}",
               bool(got) and all(v == flat_sum(n) for v in got))

    for fam, calls in ((f"{p}stf", 1), (f"{p}stpf", stpf8_runs)):
        for n in range(1, 9):
            got = rets(cap, recs, f"{fam}_{n}")
            c.true(f"{fam}_{n}: RET records", bool(got))
            c.true(f"{fam}_{n} RET values all {fmt(nested_sum(n))}: {fmt(got)}",
                   all(v == nested_sum(n) for v in got))
        c.eq(f"{fam}_8 RET count", len(rets(cap, recs, f"{fam}_8")), calls)

    for f in (f"{p}sf_fwd", f"{p}sf_fwd_inner"):
        c.eq(f"{f} RET", rets(cap, recs, f), [flat_sum(4)])

    c.true(f"{p}sf_ret_struct: ENTRY records",
           bool(entries(cap, recs, f"{p}sf_ret_struct")))
    c.true(f"{p}sf_ret_struct: RET record",
           bool(rets(cap, recs, f"{p}sf_ret_struct")))


def check_eight_struct_args_c(cap, recs, c):
    check_struct_family(cap, recs, c, "", range(1, 9), stpf8_runs=2)


def check_eight_struct_args_rust(cap, recs, c):
    check_struct_family(cap, recs, c, "r", (1, 2, 4, 8), stpf8_runs=1)


def check_rust_ffi_contract(cap, recs, c):
    """
    ffi_alloc_buf(&alloc = {NULL, 0, 0, 0}, 256, 16, is_async=1) records
    data_size + offsets_size and returns 0 without filling alloc->buffer;
    ffi_check_result() then sees {NULL, 0x110, 0, 0}. The records must show
    the violated contract at both boundaries.
    """
    ents = entries(cap, recs, "ffi_alloc_buf")
    by_arg = {r["arg_idx"]: r for r in ents}
    c.eq("ffi_alloc_buf ENTRY arg indexes", sorted(by_arg), [0, 1, 2, 3])
    if 0 in by_arg:
        c.eq("ffi_alloc_buf arg[0] struct ffi_alloc fields",
             by_arg[0]["vals"], [0, 0, 0, 0])
    if 1 in by_arg:
        c.eq("ffi_alloc_buf arg[1] data_size", by_arg[1]["val"], 256)
    if 2 in by_arg:
        c.eq("ffi_alloc_buf arg[2] offsets_size", by_arg[2]["val"], 16)
    if 3 in by_arg:
        c.eq("ffi_alloc_buf arg[3] is_async", by_arg[3]["val"], 1)
    c.eq("ffi_alloc_buf RET (claims success)", rets(cap, recs, "ffi_alloc_buf"), [0])

    ents = entries(cap, recs, "ffi_check_result")
    c.true("ffi_check_result: ENTRY record", bool(ents))
    for r in ents:
        c.eq("ffi_check_result arg[0] {buffer NULL: contract violated, "
             "data_size, free_async, flags}", r["vals"], [0, 0x110, 0, 0])
    got = rets(cap, recs, "ffi_check_result")
    c.true(f"ffi_check_result RET -EFAULT: {fmt(got)}",
           len(got) == 1 and got[0] & 0xffffffff == 0xfffffff2)


def check_rust_kworker_remote(cap, recs, c):
    """
    The trigger only queues a work item and waits; the records come from the
    kworker that called kcov_df_remote_start(REMOTE_HANDLE). All three phases
    of CompositeStore must show up (v0-mangled names keep the method names).
    """
    c.true("records captured from the kworker", bool(recs))
    names = set().union(*(cap.funcs(r) for r in recs)) if recs else set()
    for phase in ("populate", "update", "drain"):
        c.true(f"CompositeStore::{phase} recorded",
               any("CompositeStore" in n and phase in n for n in names))


TESTS = (
    ("rust_ffi_contract", False, check_rust_ffi_contract),
    ("eight_struct_args_c", False, check_eight_struct_args_c),
    ("eight_struct_args_rust", False, check_eight_struct_args_rust),
    ("rust_kworker_remote", True, check_rust_kworker_remote),
)


def diag_tree(cap, recs, vmlinux):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        tv.print_tree(recs, cap.syms, vmlinux, {}, cap.ko_path,
                      cap.mod_text_start)
    for line in out.getvalue().splitlines():
        ksft.print_msg(line)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("-t", "--test", action="append",
                        help="run only this module (repeatable)")
    parser.add_argument("-C", "--context", type=int, default=0,
                        help="echo N records before/after each module record")
    parser.add_argument("--vmlinux", help="vmlinux for addr2line and KASLR")
    args = parser.parse_args()

    tests = [t for t in TESTS if not args.test or t[0] in args.test]
    ksft.print_header()
    ksft.set_plan(len(tests))

    skip_all = None
    if not os.path.exists(tv.KCOV_DF_PATH):
        skip_all = f"{tv.KCOV_DF_PATH} not available (CONFIG_KCOV_DATAFLOW_ARGS/RET)"
    elif os.geteuid() != 0:
        skip_all = "must run as root"

    vmlinux = tv.find_vmlinux(args.vmlinux)
    for name, remote, check in tests:
        if skip_all:
            ksft.test_result_skip(f"{name}: {skip_all}")
            continue
        ko = tv.find_module(name)
        if not ko:
            ksft.test_result_skip(f"{name}: {name}.ko not built")
            continue
        try:
            cap = tv.run_capture(ko, remote=remote, vmlinux=vmlinux,
                                 log=ksft.print_msg)
        except OSError as e:
            ksft.test_result_fail(f"{name}: {e}")
            continue

        recs = cap.module_records()
        ksft.print_msg(f"{name}: {cap.total_words} words, {len(cap.records)} "
                       f"records, {len(recs)} from {name} "
                       f"(kaslr_offset=0x{cap.kaslr_offset:x})")
        diag_tree(cap, cap.context_records(args.context) if args.context
                  else recs, vmlinux)

        c = Check()
        check(cap, recs, c)
        for f in c.failures:
            ksft.print_msg(f"FAIL {name}: {f}")
        ksft.test_result(not c.failures, name)

    ksft.finished()


if __name__ == "__main__":
    main()
