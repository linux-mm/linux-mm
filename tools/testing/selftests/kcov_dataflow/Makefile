# SPDX-License-Identifier: GPL-2.0
#
# kcov_dataflow selftests
#
# user_ioctl and binderfs are ordinary kselftest programs. The test modules
# (one per directory, listed in Kbuild) are built by kbuild against KDIR and
# are loaded, triggered and checked by test_modules.py; trigger-view.py is
# the interactive viewer the runner is built on.
#
# KDIR is the configured kernel build tree. It defaults to the source tree
# this directory lives in; point it at the O= directory for out-of-tree
# builds. Pass the same LLVM=1 CC=clang [RUSTC= RUST_LIB_SRC=] the kernel was
# built with so that kbuild picks the toolchain that has the trace-args and
# trace-ret passes.
KDIR ?= $(abspath ../../../..)

TEST_GEN_PROGS := user_ioctl/user_ioctl binderfs/binderfs_test
TEST_PROGS := test_modules.py
TEST_FILES := trigger-view.py

CFLAGS += -Wall -O2 $(KHDR_INCLUDES)

# The .ko files kbuild produces for KDIR's configuration, so that they are
# built by "all" and copied by "install"; the Rust modules need CONFIG_RUST.
KMODS := rust_ffi_contract eight_struct_args_c
ifneq ($(shell grep -s ^CONFIG_RUST=y $(KDIR)/.config),)
KMODS += eight_struct_args_rust rust_kworker_remote
endif
TEST_GEN_FILES := $(foreach m,$(KMODS),$(m)/$(m).ko)

include ../lib.mk

ifneq ($(wildcard $(KDIR)/.config),)
$(TEST_GEN_FILES): modules
modules:
	$(Q)$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean_modules:
	$(Q)$(MAKE) -C $(KDIR) M=$(CURDIR) clean
else
$(TEST_GEN_FILES):
	@echo "SKIP $(notdir $@): no configured kernel tree at $(KDIR), set KDIR="
clean_modules:
endif

clean: clean_modules
.PHONY: modules clean_modules
