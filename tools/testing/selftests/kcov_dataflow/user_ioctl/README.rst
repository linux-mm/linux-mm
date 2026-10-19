.. SPDX-License-Identifier: GPL-2.0

KCOV-Dataflow Selftests: user_ioctl
===================================

Automated ioctl interface test (kselftest harness, 9 TAP cases): INIT_TRACK
argument checking, double init, mmap before init, ENABLE/DISABLE pairing,
a second fd failing with -EBUSY, and record validity after a syscall::

  make -C tools/testing/selftests TARGETS=kcov_dataflow
  tools/testing/selftests/kcov_dataflow/user_ioctl/user_ioctl
