/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HMM_COMMON_H
#define HMM_COMMON_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "kselftest_harness.h"
#include <mm/vm_util.h>

/*
 * This is a private UAPI to the kernel test module so it isn't exported
 * in the usual include/uapi/... directory.
 */
#include <lib/test_hmm_uapi.h>

struct hmm_buffer {
	void		*ptr;
	void		*mirror;
	unsigned long	size;
	int		fd;
	uint64_t	cpages;
	uint64_t	faults;
};

enum {
	HMM_PRIVATE_DEVICE_ONE,
	HMM_PRIVATE_DEVICE_TWO,
	HMM_COHERENCE_DEVICE_ONE,
	HMM_COHERENCE_DEVICE_TWO,
};

#define TWOMEG		(1 << 21)
#define HMM_PATH_MAX    64

FIXTURE(hmm)
{
	int		fd;
	unsigned int	page_size;
	unsigned int	page_shift;
};

FIXTURE_VARIANT(hmm)
{
	int     device_number;
};

FIXTURE_VARIANT_ADD(hmm, hmm_device_private)
{
	.device_number = HMM_PRIVATE_DEVICE_ONE,
};

FIXTURE_VARIANT_ADD(hmm, hmm_device_coherent)
{
	.device_number = HMM_COHERENCE_DEVICE_ONE,
};

static int hmm_open(int unit)
{
	char pathname[HMM_PATH_MAX];
	int fd;

	snprintf(pathname, sizeof(pathname), "/dev/hmm_dmirror%d", unit);
	fd = open(pathname, O_RDWR, 0);
	if (fd < 0)
		fprintf(stderr, "could not open hmm dmirror driver (%s)\n",
			pathname);
	return fd;
}

static bool hmm_is_coherent_type(int dev_num)
{
	return (dev_num >= HMM_COHERENCE_DEVICE_ONE);
}

FIXTURE_SETUP(hmm)
{
	self->page_size = sysconf(_SC_PAGE_SIZE);
	self->page_shift = ffs(self->page_size) - 1;

	self->fd = hmm_open(variant->device_number);
	if (self->fd < 0 && hmm_is_coherent_type(variant->device_number))
		SKIP(return, "DEVICE_COHERENT not available");
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(hmm)
{
	int ret = close(self->fd);

	ASSERT_EQ(ret, 0);
	self->fd = -1;
}

static int hmm_dmirror_cmd(int fd,
			   unsigned long request,
			   struct hmm_buffer *buffer,
			   unsigned long npages)
{
	struct hmm_dmirror_cmd cmd;
	int ret;

	/* Simulate a device reading system memory. */
	cmd.addr = ptr_to_u64(buffer->ptr);
	cmd.ptr = ptr_to_u64(buffer->mirror);
	cmd.npages = npages;

	for (;;) {
		ret = ioctl(fd, request, &cmd);
		if (ret == 0)
			break;
		if (errno == EINTR)
			continue;
		return -errno;
	}
	buffer->cpages = cmd.cpages;
	buffer->faults = cmd.faults;

	return 0;
}

static void hmm_buffer_free(struct hmm_buffer *buffer)
{
	if (buffer == NULL)
		return;

	if (buffer->ptr) {
		munmap(buffer->ptr, buffer->size);
		buffer->ptr = NULL;
	}
	free(buffer->mirror);
	free(buffer);
}

/*
 * Allocate a buffer structure with memory mapping and mirror.
 *
 * @mmap_size:   total size of the mmap region (may differ from @mirror_size
 *               for alignment padding in THP tests).
 * @mirror_size: size of the mirror data buffer (the actual working set).
 * @prot:        protection flags for the mmap (e.g. PROT_READ | PROT_WRITE).
 * @flags:       flags for the mmap (e.g. MAP_PRIVATE, MAP_SHARED,
 *               MAP_ANONYMOUS, MAP_HUGETLB).
 * @fd:          file descriptor for the mmap; pass -1 for MAP_ANONYMOUS.
 *
 * All internal allocations are checked; returns NULL and cleans up on any
 * failure. Caller must ASSERT_NE or otherwise check the return value.
 */
static struct hmm_buffer *hmm_buffer_alloc(unsigned long mmap_size,
						unsigned long mirror_size,
						int prot, int flags,
						int fd)
{
	struct hmm_buffer *buffer;

	buffer = malloc(sizeof(*buffer));
	if (!buffer) {
		perror("malloc buffer");
		return NULL;
	}

	buffer->fd = fd;
	buffer->size = mmap_size;
	buffer->mirror = malloc(mirror_size);
	if (!buffer->mirror) {
		perror("malloc mirror");
		free(buffer);
		return NULL;
	}

	buffer->ptr = mmap(NULL, mmap_size, prot, flags, fd, 0);
	if (buffer->ptr == MAP_FAILED) {
		perror("mmap");
		free(buffer->mirror);
		free(buffer);
		return NULL;
	}

	return buffer;
}

static int hmm_migrate_sys_to_dev(int fd,
				   struct hmm_buffer *buffer,
				   unsigned long npages)
{
	return hmm_dmirror_cmd(fd, HMM_DMIRROR_MIGRATE_TO_DEV, buffer, npages);
}

static int hmm_migrate_dev_to_sys(int fd,
				   struct hmm_buffer *buffer,
				   unsigned long npages)
{
	return hmm_dmirror_cmd(fd, HMM_DMIRROR_MIGRATE_TO_SYS, buffer, npages);
}

#endif /* HMM_COMMON_H */
