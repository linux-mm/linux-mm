.. SPDX-License-Identifier: GPL-2.0

KCOV-Dataflow: function argument and return value extraction
=============================================================

KCOV-Dataflow captures function arguments and return values, including
automatic struct field decomposition, at instrumented kernel function
boundaries. It provides per-task, lock-free ring buffers accessible via
``mmap()``, enabling data-flow-aware fuzzing and post-mortem contract
verification.

Unlike KCOV's ``trace-pc`` which reports *which* code executed,
KCOV-Dataflow reports *what values* were passed and returned. This is
a completely separate device from ``/sys/kernel/debug/kcov``.

Prerequisites
-------------

KCOV-Dataflow requires Clang/LLVM with the ``trace-args`` and
``trace-ret`` SanitizerCoverage extensions. Standard (unpatched)
compilers will not expose these Kconfig options.

To enable KCOV-Dataflow, configure the kernel with::

        CONFIG_KCOV=y
        CONFIG_KCOV_DATAFLOW_ARGS=y
        CONFIG_KCOV_DATAFLOW_RET=y

Optional: instrument the entire kernel (significant overhead)::

        CONFIG_KCOV_DATAFLOW_INSTRUMENT_ALL=y

Coverage data becomes accessible once debugfs is mounted::

        mount -t debugfs none /sys/kernel/debug

Per-module instrumentation
--------------------------

To instrument a specific module, add to its Makefile::

        KCOV_DATAFLOW_my_module.o := y

For example, to instrument the Android binder driver::

        # drivers/android/Makefile
        KCOV_DATAFLOW_binder.o := y
        KCOV_DATAFLOW_binder_alloc.o := y

To instrument an entire directory, set the variable without a filename::

        # fs/Makefile
        KCOV_DATAFLOW := y

The build system automatically adds the required compiler flags
(``-fsanitize-coverage=trace-args,trace-ret``). Debug info is provided
by ``CONFIG_DEBUG_INFO`` which is a Kconfig dependency.

Data collection
---------------

The following program demonstrates how to collect function argument and
return value data for a single syscall:

.. code-block:: c

    #include <stdio.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <sys/types.h>
    #include <sys/ioctl.h>
    #include <sys/mman.h>
    #include <unistd.h>
    #include <fcntl.h>

    #include <linux/kcov_dataflow.h>   /* ioctls, record layout, helpers */
    #define BUF_SIZE            (1 << 20)  /* 1M words = 8MB */

    int main(void)
    {
        int fd;
        uint64_t *buf, n, i;

        fd = open("/sys/kernel/debug/kcov_dataflow", O_RDWR);
        if (fd == -1)
            perror("open"), exit(1);

        /* Allocate buffer (size in u64 words). */
        if (ioctl(fd, KCOV_DF_INIT_TRACK, BUF_SIZE))
            perror("ioctl(INIT)"), exit(1);

        /* Map the buffer into user space. */
        buf = (uint64_t *)mmap(NULL, BUF_SIZE * sizeof(uint64_t),
                               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (buf == MAP_FAILED)
            perror("mmap"), exit(1);

        /* Enable data-flow collection for this task. */
        if (ioctl(fd, KCOV_DF_ENABLE, 0))
            perror("ioctl(ENABLE)"), exit(1);

        /* Reset counter. */
        __atomic_store_n(&buf[0], 0, __ATOMIC_RELAXED);

        /* === Trigger syscall(s) here === */
        read(-1, NULL, 0);

        /* Read how many words were written. */
        n = __atomic_load_n(&buf[0], __ATOMIC_RELAXED);

        /* Parse TLV records. */
        i = 1;
        while (i + KCOV_DF_RECORD_HDR_WORDS <= 1 + n) {
            uint64_t hdr      = buf[i];
            uint64_t pc       = buf[i + 1];   /* KASLR offset removed */
            uint64_t ptr      = buf[i + 2];   /* traced pointer (ENTRY/RET) */
            uint32_t type     = KCOV_DF_HDR_TYPE(hdr);
            uint32_t num_vals = KCOV_DF_HDR_NVALS(hdr);
            uint32_t seq      = KCOV_DF_HDR_SEQ(hdr);
            uint32_t arg_idx  = KCOV_DF_HDR_ARGIDX(hdr);
            uint32_t size     = KCOV_DF_HDR_SIZE(hdr);

            if (!num_vals || (type != KCOV_DF_TYPE_ENTRY &&
                              type != KCOV_DF_TYPE_RET &&
                              type != KCOV_DF_TYPE_CMP)) {
                i++;    /* garbage (e.g. reset mid-run): resync */
                continue;
            }
            if (type != KCOV_DF_TYPE_CMP)
                printf("[%s] seq=%u pc=0x%lx ptr=0x%lx arg_idx=%u size=%u val=0x%lx\n",
                       type == KCOV_DF_TYPE_ENTRY ? "ENTRY" : "RET",
                       seq, pc, ptr, arg_idx, size, buf[i + 3]);
            i += KCOV_DF_RECORD_WORDS(num_vals);
        }

        if (ioctl(fd, KCOV_DF_DISABLE, 0))
            perror("ioctl(DISABLE)"), exit(1);

        munmap(buf, BUF_SIZE * sizeof(uint64_t));
        close(fd);
        return 0;
    }

Ring buffer format
------------------

The buffer is an array of ``u64`` words::

        buf[0]: atomic counter -- total words written

Each record occupies 3 + N words:

.. list-table::
   :header-rows: 1

   * - Offset
     - Field
     - Description
   * - 0
     - header
     - bits[63:56] = arg_idx (0 for return), bits[55:48] = size in bytes
       (clamped to 255), bits[47:32] = num_vals (>= 1),
       bits[31:28] = type: ``KCOV_DF_TYPE_ENTRY`` (0xE),
       ``KCOV_DF_TYPE_RET`` (0xF) or ``KCOV_DF_TYPE_CMP`` (0xC),
       bits[23:0] = sequence number
   * - 1
     - pc
     - Instrumented function address with the KASLR offset removed (same
       as the PCs mainline kcov records), so it can be symbolized against
       vmlinux; add the runtime offset back for ``/proc/kallsyms``
   * - 2
     - ptr / cmp_type
     - ENTRY/RET: the full 64-bit traced pointer (may be NULL/ERR_PTR, in
       which case the values are ``0xBADADD85``). CMP: the comparison
       type, ``KCOV_CMP_SIZE()``/``KCOV_CMP_CONST`` bits from linux/kcov.h
   * - 3..3+num_vals
     - values
     - Struct field values, a single scalar, or the two CMP operands

``area[0]`` never exceeds the buffer size minus one and every counted word
has been written, so a consumer that walks ``area[0]`` words never leaves
its mapping. All of the above is defined in ``include/uapi/linux/kcov_dataflow.h``
(``KCOV_DF_HDR_*()``, ``KCOV_DF_RECORD_WORDS()``).

Magic values:

- ``0xBADADD85``: field read failed (pointer was invalid/freed/poisoned)

Safety
------

- Callbacks are ``notrace``, ``__no_sanitize_coverage``, ``noinline``
  to prevent recursion.
- All pointer reads use ``copy_from_kernel_nofault()`` -- survives
  freed, poisoned, or unmapped memory.
- An ``in_task()`` guard rejects calls from hardirq/softirq/NMI context,
  preventing reentrant buffer corruption.
- No ``printk`` or allocation in the data path.
- When not enabled for a task, overhead is a single boolean check.

Ioctl interface
---------------

.. list-table::
   :header-rows: 1

   * - Command
     - Value
     - Description
   * - KCOV_DF_INIT_TRACK
     - ``_IOR('d', 1, unsigned long)``
     - Allocate buffer (size in u64 words)
   * - KCOV_DF_ENABLE
     - ``_IO('d', 100)``
     - Start collection for current task
   * - KCOV_DF_DISABLE
     - ``_IO('d', 101)``
     - Stop collection
   * - KCOV_DF_REMOTE_ENABLE
     - ``_IOW('d', 102, __u64)`` -- argument is a pointer to the handle
     - Publish buffer for kworker/kthread remote capture
   * - KCOV_DF_REMOTE_DISABLE
     - ``_IO('d', 103)``
     - Unpublish buffer from remote capture

Compatibility
-------------

KCOV-Dataflow is completely independent from legacy KCOV:

- Separate device: ``/sys/kernel/debug/kcov_dataflow``
- Separate ioctl namespace (``'d'`` vs ``'c'``)
- Separate per-task buffer
- Both can be used simultaneously without interference
- syzkaller and other KCOV users are unaffected

Rust module support
-------------------

Rust kernel modules are instrumented natively through the build system.
The ``KCOV_DATAFLOW_<module>.o := y`` mechanism works identically for
Rust and C modules. The build system passes
``-Cllvm-args=-sanitizer-coverage-trace-args`` and
``-Cllvm-args=-sanitizer-coverage-trace-ret`` to rustc via
``RUSTFLAGS_KCOV_DATAFLOW``.

Example Makefile for a Rust module::

        obj-m := my_rust_module.o
        KCOV_DATAFLOW_my_rust_module.o := y

Requires a rustc built against LLVM with trace-args/trace-ret support
and ``CONFIG_RUST=y`` in the kernel config.

Selftests
---------

Automated tests and visualization tools are in
``tools/testing/selftests/kcov_dataflow/``::

        # Automated ioctl interface test (TAP output):
        make -C tools/testing/selftests/kcov_dataflow
        vng --user root --exec \
          tools/testing/selftests/kcov_dataflow/user_ioctl/user_ioctl

        # Load a test module and view captured records:
        make LLVM=1 CC=clang M=tools/testing/selftests/kcov_dataflow/eight_struct_args_c modules
        vng --user root --exec \
          "python3 tools/testing/selftests/kcov_dataflow/trigger-view.py \
            eight_struct_args_c --ko \
            tools/testing/selftests/kcov_dataflow/eight_struct_args_c/eight_struct_args_c.ko"

        # Binderfs ioctl capture test (requires CONFIG_ANDROID_BINDER_IPC):
        make -C tools/testing/selftests/kcov_dataflow/binderfs
        vng --user root --exec \
          tools/testing/selftests/kcov_dataflow/binderfs/binderfs_test

See ``tools/testing/selftests/kcov_dataflow/README.rst`` for details.

Tracing child processes
-----------------------

KCOV-Dataflow is per-task: after ``fork()``, the child does not inherit
the enabled state. To trace child processes, re-enable on the inherited
file descriptor in the child before ``exec()``. The ``mmap``'d buffer is
shared (``MAP_SHARED``), so both parent and child write to the same ring
buffer atomically.

.. code-block:: c

    #include <stdio.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <sys/ioctl.h>
    #include <sys/mman.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #include <fcntl.h>

    #include <linux/kcov_dataflow.h>   /* ioctls, record layout, helpers */
    #define BUF_SIZE            (1 << 20)

    int main(int argc, char **argv)
    {
        int fd = open("/sys/kernel/debug/kcov_dataflow", O_RDWR);
        ioctl(fd, KCOV_DF_INIT_TRACK, BUF_SIZE);
        uint64_t *buf = mmap(NULL, BUF_SIZE * 8,
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        /* Enable for parent task. */
        ioctl(fd, KCOV_DF_ENABLE, 0);
        __atomic_store_n(&buf[0], 0, __ATOMIC_RELAXED);

        pid_t pid = fork();
        if (pid == 0) {
            /*
             * Child: re-enable on inherited fd.
             * The shared mmap buffer receives records from both tasks.
             */
            ioctl(fd, KCOV_DF_ENABLE, 0);
            execvp(argv[1], &argv[1]);
            _exit(1);
        }

        waitpid(pid, NULL, 0);
        ioctl(fd, KCOV_DF_DISABLE, 0);

        uint64_t n = __atomic_load_n(&buf[0], __ATOMIC_RELAXED);
        printf("Captured %lu words from parent + child\n", n);

        munmap(buf, BUF_SIZE * 8);
        close(fd);
        return 0;
    }

Note: the child's ``ioctl(fd, KCOV_DF_ENABLE)`` will fail if the parent
has not yet called ``KCOV_DF_DISABLE``, because only one task can be
associated with a descriptor at a time. For true multi-process tracing,
open a separate ``kcov_dataflow`` fd per child, or disable in the parent
before the child enables (as shown above -- the parent is blocked in
``waitpid`` so it generates no records during that time anyway).

Remote tracing (kworker/kthread)
--------------------------------

To capture data from kernel threads (kworkers, kthreads) that are not
direct descendants of user space, use the remote API:

1. User space allocates and publishes a buffer with ``KCOV_DF_REMOTE_ENABLE``
2. The kernel module calls ``kcov_df_remote_start()`` at work entry
3. The kernel module calls ``kcov_df_remote_stop()`` at work exit
4. User space reads the buffer and unpublishes with ``KCOV_DF_REMOTE_DISABLE``

User space setup:

.. code-block:: c

    #include <stdio.h>
    #include <stdint.h>
    #include <sys/ioctl.h>
    #include <sys/mman.h>
    #include <unistd.h>
    #include <fcntl.h>

    #include <linux/kcov.h>            /* kcov_remote_handle() */
    #include <linux/kcov_dataflow.h>
    #define BUF_SIZE                (1 << 20)

    int main(void)
    {
        int fd = open("/sys/kernel/debug/kcov_dataflow", O_RDWR);
        ioctl(fd, KCOV_DF_INIT_TRACK, BUF_SIZE);
        uint64_t *buf = mmap(NULL, BUF_SIZE * 8,
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        __atomic_store_n(&buf[0], 0, __ATOMIC_RELAXED);

        /*
         * Publish the buffer under a remote handle. The handle must be a
         * valid kcov_remote_handle() encoding (KCOV_SUBSYSTEM_COMMON with a
         * nonzero instance, or KCOV_SUBSYSTEM_USB) and is the value the
         * kernel side passes to kcov_df_remote_start(); one handle per fd,
         * and not while KCOV_DF_ENABLE is active on the same fd.
         */
        __u64 handle = kcov_remote_handle(KCOV_SUBSYSTEM_COMMON, 1);
        if (ioctl(fd, KCOV_DF_REMOTE_ENABLE, &handle))
            perror("ioctl(REMOTE_ENABLE)"), exit(1);

        /* Trigger kworker activity (e.g., write to a file, ioctl). */
        /* ... */
        sleep(1);

        /* Unpublish and read results. */
        ioctl(fd, KCOV_DF_REMOTE_DISABLE, 0);

        uint64_t n = __atomic_load_n(&buf[0], __ATOMIC_RELAXED);
        printf("Captured %lu words from kworker\n", n);

        munmap(buf, BUF_SIZE * 8);
        close(fd);
        return 0;
    }

Kernel module side (called from kworker context):

.. code-block:: c

    #include <linux/kcov.h>

    void my_work_fn(struct work_struct *work)
    {
        kcov_df_remote_start();
        /* ... instrumented code runs here ... */
        kcov_df_remote_stop();
    }

Only one buffer can be published at a time. ``kcov_df_remote_start()``
is a no-op if no buffer is published or if the current task already has
dataflow enabled.

Limitations
-----------

ABI argument mapping
    The LLVM pass maps IR-level arguments to source-level parameters using
    ``DILocalVariable`` debug records (``-g`` required). This correctly
    handles hidden ``sret`` pointers, struct decomposition into multiple
    registers, and C++ ``this`` pointers.

    When debug info is absent or stripped, the pass falls back to positional
    indexing which may misattribute arguments in functions with ABI-inserted
    hidden parameters. The kernel is always built with ``-g``, so this
    limitation does not apply to kernel use.

Struct-by-value reassembly
    When a small struct is passed by value and the ABI decomposes it into
    multiple scalar registers (e.g., ``struct { int x; int y; }`` as two
    ``i32`` values on x86_64), the pass reassembles the fragments into a
    stack slot. The struct field offsets are preserved, but if a field was
    entirely optimized away (no debug record), that slot contains zero.

    In kernel code, structs are always passed by pointer, so this case
    does not arise.

Optimized builds
    At ``-O2`` and above, LLVM may eliminate ``#dbg_value`` records for
    arguments that are dead or fully inlined. Such arguments will emit a
    trace with a null pointer (producing ``0xBADADD85`` in all field
    positions), indicating the argument existed but its value was
    unavailable at runtime.
