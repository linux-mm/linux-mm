// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_COLD and MADV_PAGEOUT tests.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kselftest_harness.h"
#include "vm_util.h"

/* Reclaim can fail transiently, so retry for up to a second. */
#define PAGEOUT_RETRIES 100

static size_t page_size;

FIXTURE(madvise)
{
	int pagemap_fd;
};

FIXTURE_SETUP(madvise)
{
	page_size = getpagesize();

	self->pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	ASSERT_GE(self->pagemap_fd, 0);
}

FIXTURE_TEARDOWN(madvise)
{
	close(self->pagemap_fd);
}

/* Page out the range until its first page is swapped. */
static bool pageout_until_swapped(struct __test_metadata *_metadata,
				  char *mapping, size_t size, int pagemap_fd)
{
	int retry;

	for (retry = 0; retry < PAGEOUT_RETRIES; retry++) {
		ASSERT_EQ(madvise(mapping, size, MADV_PAGEOUT), 0);
		if (pagemap_is_swapped(pagemap_fd, mapping))
			return true;
		usleep(10000);
	}

	return false;
}

/*
 * Both hints are best effort, so these tests check what userspace can rely
 * on: which mappings are accepted, which pages must never be reclaimed, and
 * that neither hint changes memory contents or populates the range. A test
 * that needs a page to be reclaimed first checks that reclaim works at all,
 * and skips if it does not.
 */

/* Neither hint may populate a hole between two populated pages. */
static void check_skips_pte_hole(struct __test_metadata *_metadata,
				 int pagemap_fd, int advice)
{
	char *mapping;

	mapping = mmap(NULL, 3 * page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	mapping[0] = 1;
	mapping[2 * page_size] = 1;
	ASSERT_FALSE(pagemap_is_populated(pagemap_fd, mapping + page_size));

	ASSERT_EQ(madvise(mapping, 3 * page_size, advice), 0);
	ASSERT_FALSE(pagemap_is_populated(pagemap_fd, mapping + page_size));
	ASSERT_EQ(mapping[0], 1);
	ASSERT_EQ(mapping[2 * page_size], 1);

	ASSERT_EQ(munmap(mapping, 3 * page_size), 0);
}

TEST_F(madvise, cold_skips_pte_hole)
{
	check_skips_pte_hole(_metadata, self->pagemap_fd, MADV_COLD);
}

TEST_F(madvise, pageout_skips_pte_hole)
{
	check_skips_pte_hole(_metadata, self->pagemap_fd, MADV_PAGEOUT);
}

/* Neither hint may be applied to locked memory. */
TEST_F(madvise, locked_vma_is_rejected)
{
	char *mapping;

	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	mapping[0] = 1;
	if (mlock(mapping, page_size))
		SKIP(return, "could not lock a page");

	ASSERT_EQ(madvise(mapping, page_size, MADV_COLD), -1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(madvise(mapping, page_size, MADV_PAGEOUT), -1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(mapping[0], 1);

	ASSERT_EQ(munmap(mapping, page_size), 0);
}

/* Neither hint may fault a swapped page back in. */
static void check_skips_swapped_pte(struct __test_metadata *_metadata,
				    int pagemap_fd, int advice)
{
	char *mapping;

	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	mapping[0] = 1;
	if (!pageout_until_swapped(_metadata, mapping, page_size, pagemap_fd))
		SKIP(return, "MADV_PAGEOUT did not swap the page");

	ASSERT_EQ(madvise(mapping, page_size, advice), 0);
	ASSERT_TRUE(pagemap_is_swapped(pagemap_fd, mapping));
	ASSERT_EQ(mapping[0], 1);

	ASSERT_EQ(munmap(mapping, page_size), 0);
}

TEST_F(madvise, cold_skips_swapped_pte)
{
	check_skips_swapped_pte(_metadata, self->pagemap_fd, MADV_COLD);
}

TEST_F(madvise, pageout_skips_swapped_pte)
{
	check_skips_swapped_pte(_metadata, self->pagemap_fd, MADV_PAGEOUT);
}

TEST_HARNESS_MAIN
