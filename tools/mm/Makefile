# SPDX-License-Identifier: GPL-2.0
# Makefile for vm tools
#
include ../scripts/Makefile.include

BUILD_TARGETS=page-types slabinfo page_owner_sort page_owner_filter
BUILD_TARGETS += thp_swap_allocator_test gup_bench hmm-bench
BUILD_TARGETS+=memory-failure/hwpoison-panic
INSTALL_TARGETS = $(BUILD_TARGETS) thpmaps

LIB_DIR = ../lib/api
LIBS = $(LIB_DIR)/libapi.a
GUP_BENCH_OBJS = gup_bench.c ../lib/mm/hugepage_settings.c ../lib/mm/file_utils.c
SELFTESTS = ../testing/selftests
HMM_BENCH_SRCS = hmm-bench.c $(SELFTESTS)/mm/vm_util.c \
		 ../lib/mm/hugepage_settings.c ../lib/mm/file_utils.c
HMM_BENCH_HDRS = $(SELFTESTS)/mm/hmm-common.h $(SELFTESTS)/mm/vm_util.h \
		 $(SELFTESTS)/kselftest_harness.h $(SELFTESTS)/kselftest.h \
		 ../../lib/test_hmm_uapi.h ../lib/mm/hugepage_settings.h ../lib/mm/file_utils.h

CFLAGS += -Wall -Wextra -I../lib/ -I../.. -pthread
LDFLAGS += $(LIBS) -pthread

all: $(BUILD_TARGETS)

$(BUILD_TARGETS): $(LIBS)

$(LIBS):
	make -C $(LIB_DIR)

%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

gup_bench: $(GUP_BENCH_OBJS) $(LIBS)
	$(CC) $(CFLAGS) -o $@ $(GUP_BENCH_OBJS) $(LDFLAGS)

hmm-bench: $(HMM_BENCH_SRCS) $(HMM_BENCH_HDRS)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I$(SELFTESTS) -isystem ../include/uapi \
		-o $@ $(HMM_BENCH_SRCS) $(LDFLAGS)

clean:
	$(RM) $(BUILD_TARGETS)
	make -C $(LIB_DIR) clean

sbindir ?= /usr/sbin

install: all
	install -d $(DESTDIR)$(sbindir)
	install -m 755 -p $(INSTALL_TARGETS) $(DESTDIR)$(sbindir)
