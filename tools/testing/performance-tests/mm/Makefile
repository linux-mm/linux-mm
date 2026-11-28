# SPDX-License-Identifier: GPL-2.0
TEST_GEN_PROGS := hmm-bench

CFLAGS += -Wall -O2 -I$(top_srcdir) -I$(top_srcdir)/tools/lib
CFLAGS += $(KHDR_INCLUDES) $(TOOLS_INCLUDES)
LOCAL_HDRS += $(selfdir)/mm/hmm-common.h $(selfdir)/mm/vm_util.h
LOCAL_HDRS += $(top_srcdir)/lib/test_hmm_uapi.h

include ../../selftests/lib.mk

$(OUTPUT)/hmm-bench: $(selfdir)/mm/vm_util.c \
		     $(top_srcdir)/tools/lib/mm/hugepage_settings.c \
		     $(top_srcdir)/tools/lib/mm/file_utils.c
