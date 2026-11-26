# SPDX-License-Identifier: GPL-2.0+ OR MIT

.PHONY: default clean

default: hpcc

CFLAGS += -O2 -g -Wall -I./include -I../../include -fno-strict-overflow -fno-strict-aliasing
CFLAGS += -DCONFIG_SMP -DCONFIG_KUNIT=1 -D'NR_CPUS=(1U << 20)'

DEPS = ../../../lib/percpu_counter_tree.c \
	../../../include/linux/percpu_counter_tree.h \
	include/hpcc_shim.h

hpcc: main.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ main.c $(LDLIBS)

clean:
	$(RM) hpcc
