.. SPDX-License-Identifier: GPL-2.0

KCOV-Dataflow Selftests: binderfs
=================================

Exercises the binder driver via binderfs with kcov_dataflow recording
active and verifies that argument records are captured at the binder
ioctl boundaries. Needs CONFIG_ANDROID_BINDERFS=y and binder instrumented
(``KCOV_DATAFLOW := y`` in drivers/android/Makefile or
CONFIG_KCOV_DATAFLOW_INSTRUMENT_ALL=y); SKIPs without binderfs::

  make -C tools/testing/selftests TARGETS=kcov_dataflow
  tools/testing/selftests/kcov_dataflow/binderfs/binderfs_test
