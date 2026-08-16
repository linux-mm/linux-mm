# SPDX-License-Identifier: GPL-2.0
.PHONY: all clean test

CC = gcc
CFLAGS = -Wall -O2 -I../shared -I. -I../../include -I../../arch/x86/include -pthread
KERNEL_SUBPOOL_H = ../../../mm/hugetlb_subpool.h
KERNEL_SUBPOOL_C = ../../../mm/hugetlb_subpool.c

all: test

test_subpool: test_subpool.c $(KERNEL_SUBPOOL_C) $(KERNEL_SUBPOOL_H)
	$(CC) $(CFLAGS) test_subpool.c -o test_subpool

test: test_subpool
	./test_subpool

clean:
	rm -f test_subpool
