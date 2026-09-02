.. SPDX-License-Identifier: GPL-2.0

KCOV-Dataflow Selftests
=======================

Selftests for ``/sys/kernel/debug/kcov_dataflow`` (see
Documentation/dev-tools/kcov-dataflow.rst).

Layout
------

Makefile, Kbuild
    kselftest build: the C programs are built by lib.mk, the test modules
    (one directory each, listed in Kbuild) by kbuild against ``KDIR``.
user_ioctl/
    ioctl interface test (kselftest harness, TAP).
binderfs/
    binder ioctls under recording (TAP).
rust_ffi_contract/, eight_struct_args_c/, eight_struct_args_rust/,
rust_kworker_remote/
    test modules; each README.rst says what the module exercises.
test_modules.py
    KTAP runner: loads every module, triggers it with recording active and
    checks the captured arguments, struct fields and return values against
    the values the module uses. Modules that are not built are SKIPped.
trigger-view.py
    Interactive viewer the runner is built on (call tree or ``--raw``
    records, kallsyms/addr2line symbolization, ``--remote`` capture).

Kernel
------

The kernel and the modules must be built with a clang that has the
trace-args/trace-ret passes (and, for the Rust modules, a rustc built
against that LLVM). The config fragment ``config`` lists what the tests
need; with virtme-ng::

    vng --build --config tools/testing/selftests/kcov_dataflow/config \
        LLVM=1 CC=clang RUSTC=$RUSTC RUST_LIB_SRC=$RUST_LIB_SRC

Build
-----

From the kernel tree, with the same toolchain variables::

    make LLVM=1 headers
    make -C tools/testing/selftests TARGETS=kcov_dataflow \
        LLVM=1 CC=clang RUSTC=$RUSTC RUST_LIB_SRC=$RUST_LIB_SRC

``KDIR`` defaults to the source tree; pass ``KDIR=<O dir>`` for out-of-tree
builds. The Rust modules are built only when ``KDIR/.config`` has
``CONFIG_RUST=y``. ``make ... install INSTALL_PATH=<dir>`` produces a
self-contained tree with ``run_kselftest.sh``.

Run
---

On the target (root, debugfs mounted)::

    vng --user root --exec \
        "tools/testing/selftests/kcov_dataflow/test_modules.py"
    tools/testing/selftests/kcov_dataflow/user_ioctl/user_ioctl
    tools/testing/selftests/kcov_dataflow/binderfs/binderfs_test

or, from an installed tree, ``run_kselftest.sh -c kcov_dataflow``.
``test_modules.py -t <module> -C 8`` runs one module and echoes eight
records of context around each module record; ``trigger-view.py <module>
[--raw] [-C N] [--remote] [--vmlinux vmlinux]`` shows the capture
without checking it.
