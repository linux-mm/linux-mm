// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_COLD and MADV_PAGEOUT tests.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
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
 * Keep this task on one CPU, so that draining that CPU's LRU batches drains
 * the folios this task faulted in.
 */
static void pin_to_current_cpu(struct __test_metadata *_metadata)
{
	int cpu = sched_getcpu();
	cpu_set_t mask;

	ASSERT_GE(cpu, 0);
	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	ASSERT_EQ(sched_setaffinity(0, sizeof(mask), &mask), 0);
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

/* Neither hint may be applied to hugetlb memory. */
TEST_F(madvise, hugetlb_vma_is_rejected)
{
	size_t size = default_huge_page_size();
	char *mapping;

	if (!size)
		SKIP(return, "hugetlb is not supported");
	mapping = mmap(NULL, size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (mapping == MAP_FAILED)
		SKIP(return, "no hugetlb pages are available");
	mapping[0] = 1;

	ASSERT_EQ(madvise(mapping, size, MADV_COLD), -1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(madvise(mapping, size, MADV_PAGEOUT), -1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(mapping[0], 1);

	ASSERT_EQ(munmap(mapping, size), 0);
}

/* A zero-length range is accepted. */
TEST_F(madvise, zero_length_is_accepted)
{
	char *mapping;

	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);

	ASSERT_EQ(madvise(mapping, 0, MADV_COLD), 0);
	ASSERT_EQ(madvise(mapping, 0, MADV_PAGEOUT), 0);

	ASSERT_EQ(munmap(mapping, page_size), 0);
}

/* The start of the range must be page-aligned. */
TEST_F(madvise, unaligned_start_is_rejected)
{
	char *mapping;

	mapping = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	mapping[0] = 1;

	ASSERT_EQ(madvise(mapping + 1, page_size, MADV_COLD), -1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(madvise(mapping + 1, page_size, MADV_PAGEOUT), -1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(mapping[0], 1);

	ASSERT_EQ(munmap(mapping, 2 * page_size), 0);
}

/*
 * An unmapped gap in the range makes either hint fail with ENOMEM, after
 * advising the mapped pages on both sides of the gap. Only MADV_PAGEOUT has
 * an effect on those pages that the test can observe.
 */
TEST_F(madvise, unmapped_gap_fails_after_advising)
{
	char *mapping, *control;
	bool swapped = false;
	int retry;

	pin_to_current_cpu(_metadata);

	/* Make sure MADV_PAGEOUT can swap at all before relying on it. */
	control = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(control, MAP_FAILED);
	control[0] = 1;
	if (!pageout_until_swapped(_metadata, control, page_size,
				   self->pagemap_fd))
		SKIP(return, "MADV_PAGEOUT did not swap the page");

	mapping = mmap(NULL, 3 * page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	mapping[0] = 1;
	mapping[2 * page_size] = 1;
	ASSERT_EQ(munmap(mapping + page_size, page_size), 0);

	ASSERT_EQ(madvise(mapping, 3 * page_size, MADV_COLD), -1);
	ASSERT_EQ(errno, ENOMEM);
	ASSERT_EQ(mapping[0], 1);
	ASSERT_EQ(mapping[2 * page_size], 1);

	for (retry = 0; retry < PAGEOUT_RETRIES && !swapped; retry++) {
		ASSERT_EQ(madvise(mapping, 3 * page_size, MADV_PAGEOUT), -1);
		ASSERT_EQ(errno, ENOMEM);
		swapped = pagemap_is_swapped(self->pagemap_fd, mapping) &&
			  pagemap_is_swapped(self->pagemap_fd,
					     mapping + 2 * page_size);
		if (!swapped)
			usleep(10000);
	}
	ASSERT_TRUE(swapped)
		TH_LOG("MADV_PAGEOUT did not page out both sides of the gap");
	ASSERT_EQ(mapping[0], 1);
	ASSERT_EQ(mapping[2 * page_size], 1);

	ASSERT_EQ(munmap(mapping, page_size), 0);
	ASSERT_EQ(munmap(mapping + 2 * page_size, page_size), 0);
	ASSERT_EQ(munmap(control, page_size), 0);
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
