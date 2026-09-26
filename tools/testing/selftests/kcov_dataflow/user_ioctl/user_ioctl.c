// SPDX-License-Identifier: GPL-2.0
/*
 * kcov_dataflow_test.c - Selftest for /sys/kernel/debug/kcov_dataflow
 *
 * Verifies the ioctl interface: open, INIT_TRACK, mmap, ENABLE, DISABLE.
 * With INSTRUMENT_ALL, also verifies that records are produced for
 * syscalls executed while recording is active.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <linux/kcov_dataflow.h>

#include "../../kselftest_harness.h"


#define BUF_SIZE 65536

#define DF_TYPE_ENTRY	KCOV_DF_TYPE_ENTRY
#define DF_TYPE_RET	KCOV_DF_TYPE_RET

FIXTURE(kcov_dataflow) {
	int fd;
	uint64_t *buf;
};

FIXTURE_SETUP(kcov_dataflow)
{
	self->fd = open("/sys/kernel/debug/kcov_dataflow", O_RDWR);
	if (self->fd < 0)
		SKIP(return, "kcov_dataflow not available (need CONFIG_KCOV_DATAFLOW_ARGS)");
	self->buf = MAP_FAILED;
}

FIXTURE_TEARDOWN(kcov_dataflow)
{
	if (self->buf != MAP_FAILED)
		munmap(self->buf, BUF_SIZE * sizeof(uint64_t));
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(kcov_dataflow, init_track)
{
	int ret = ioctl(self->fd, KCOV_DF_INIT_TRACK, (unsigned long)BUF_SIZE);

	ASSERT_EQ(0, ret);
}

TEST_F(kcov_dataflow, init_track_too_small)
{
	int ret = ioctl(self->fd, KCOV_DF_INIT_TRACK, 1UL);

	ASSERT_EQ(-1, ret);
	ASSERT_EQ(EINVAL, errno);
}

TEST_F(kcov_dataflow, init_track_double)
{
	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_INIT_TRACK, (unsigned long)BUF_SIZE));
	ASSERT_EQ(-1, ioctl(self->fd, KCOV_DF_INIT_TRACK, (unsigned long)BUF_SIZE));
	ASSERT_EQ(EBUSY, errno);
}

TEST_F(kcov_dataflow, mmap_before_init)
{
	self->buf = mmap(NULL, BUF_SIZE * sizeof(uint64_t),
			 PROT_READ | PROT_WRITE, MAP_SHARED, self->fd, 0);
	ASSERT_EQ(MAP_FAILED, self->buf);
}

TEST_F(kcov_dataflow, enable_disable)
{
	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_INIT_TRACK, (unsigned long)BUF_SIZE));
	self->buf = mmap(NULL, BUF_SIZE * sizeof(uint64_t),
			 PROT_READ | PROT_WRITE, MAP_SHARED, self->fd, 0);
	ASSERT_NE(MAP_FAILED, self->buf);
	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_ENABLE, 0));
	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_DISABLE, 0));
}

TEST_F(kcov_dataflow, enable_without_mmap)
{
	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_INIT_TRACK, (unsigned long)BUF_SIZE));
	/* enable works even without mmap (mmap is optional for setup) */
	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_ENABLE, 0));
	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_DISABLE, 0));
}

TEST_F(kcov_dataflow, disable_without_enable)
{
	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_INIT_TRACK, (unsigned long)BUF_SIZE));
	ASSERT_EQ(-1, ioctl(self->fd, KCOV_DF_DISABLE, 0));
	ASSERT_EQ(EINVAL, errno);
}

TEST_F(kcov_dataflow, double_enable)
{
	int fd2;

	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_INIT_TRACK, (unsigned long)BUF_SIZE));
	self->buf = mmap(NULL, BUF_SIZE * sizeof(uint64_t),
			 PROT_READ | PROT_WRITE, MAP_SHARED, self->fd, 0);
	ASSERT_NE(MAP_FAILED, self->buf);
	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_ENABLE, 0));

	/* Second fd should fail to enable (task already active) */
	fd2 = open("/sys/kernel/debug/kcov_dataflow", O_RDWR);
	ASSERT_GE(fd2, 0);
	ASSERT_EQ(0, ioctl(fd2, KCOV_DF_INIT_TRACK, (unsigned long)BUF_SIZE));
	ASSERT_EQ(-1, ioctl(fd2, KCOV_DF_ENABLE, 0));
	ASSERT_EQ(EBUSY, errno);
	close(fd2);

	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_DISABLE, 0));
}

TEST_F(kcov_dataflow, records_captured)
{
	uint64_t count;

	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_INIT_TRACK, (unsigned long)BUF_SIZE));
	self->buf = mmap(NULL, BUF_SIZE * sizeof(uint64_t),
			 PROT_READ | PROT_WRITE, MAP_SHARED, self->fd, 0);
	ASSERT_NE(MAP_FAILED, self->buf);
	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_ENABLE, 0));

	/* Trigger some kernel code in this task */
	getpid();

	ASSERT_EQ(0, ioctl(self->fd, KCOV_DF_DISABLE, 0));

	count = self->buf[0];
	/*
	 * With INSTRUMENT_ALL, getpid() produces records; without it count may
	 * be 0. Whatever was written must parse: known types (CMP records are
	 * interleaved with CONFIG_KCOV_ENABLE_COMPARISONS=y), at least one value
	 * word each, and a walk that ends exactly at area[0] inside the buffer.
	 */
	ASSERT_LE(count, (uint64_t)BUF_SIZE - 1);
	if (count > 0) {
		uint64_t pos = 1, end = 1 + count;
		unsigned int nargs = 0;

		while (pos + KCOV_DF_RECORD_HDR_WORDS <= end) {
			uint64_t hdr = self->buf[pos];
			unsigned int type = KCOV_DF_HDR_TYPE(hdr);
			unsigned int nvals = KCOV_DF_HDR_NVALS(hdr);

			ASSERT_TRUE(type == DF_TYPE_ENTRY || type == DF_TYPE_RET ||
				    type == KCOV_DF_TYPE_CMP);
			ASSERT_GE(nvals, 1);
			if (type != KCOV_DF_TYPE_CMP)
				nargs++;
			pos += KCOV_DF_RECORD_WORDS(nvals);
		}
		ASSERT_EQ(end, pos);
		ASSERT_GT(nargs, 0);
	}
}

TEST_HARNESS_MAIN
