// SPDX-License-Identifier: GPL-2.0
/*
 * fork() of a process that maps shmem THPs with PMDs, in a VMA whose page
 * tables fork copies: a private mapping with an anon_vma, and a shared
 * mapping with a guard region.
 *
 * The child must get the PMDs without faulting, and its exit must leave the
 * parent's mapping and the system-wide ShmemPmdMapped counter as they were.
 * Kernels that leave file THPs to a refault in the child skip the test.
 */
#define _GNU_SOURCE
#include <asm-generic/mman.h> /* Force the import of the tools version. */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include "kselftest.h"
#include "vm_util.h"

#define NR_PMDS		4
#define MAX_PMDSIZE	(32UL << 20)
/* ShmemPmdMapped is system-wide: retry if something else changed it. */
#define ATTEMPTS	3

static size_t pagesize, pmdsize;

static long meminfo_kb(const char *key)
{
	char line[256];
	long kb = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key))) {
			kb = strtol(line + strlen(key), NULL, 10);
			break;
		}
	fclose(f);
	return kb;
}

/* Skips the last page, which may be a guard page, and @skip. */
static bool data_ok(const char *mem, size_t size, size_t skip)
{
	size_t off;

	for (off = 0; off < size - pagesize; off += pagesize)
		if (off != skip && mem[off] != (char)(off / pmdsize + 1))
			return false;
	return true;
}

/* Child exit codes, apart from what kselftest helpers exit with. */
enum result { PASS, NO_PMDS = 10, FAIL_DATA, FAIL_COUNTER, FAIL_PARENT,
	      FAIL_KILLED, COUNTER_OFF, CHANGED };

static enum result fork_once(char *mem, size_t size, bool shared,
			     long *before, long *after)
{
	/* The parent alone maps this much with PMDs. */
	long own = (NR_PMDS - 1) * (pmdsize >> 10);
	int status;
	pid_t pid;

	*before = *after = meminfo_kb("ShmemPmdMapped:");
	if (*before < own)
		return COUNTER_OFF;
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		ksft_exit_fail_perror("fork");
	if (!pid) {
		/* Look before touching anything: the PMDs must be there. */
		if (!check_huge_shmem(mem, size - pmdsize, NR_PMDS - 1, pmdsize))
			_exit(NO_PMDS);
		/* A shared retry sees the previous child's write. */
		if (!data_ok(mem, size, pmdsize))
			_exit(FAIL_DATA);
		mem[pmdsize] = 'c';
		_exit(PASS);
	}
	if (waitpid(pid, &status, 0) != pid)
		ksft_exit_fail_perror("waitpid");
	*after = meminfo_kb("ShmemPmdMapped:");

	if (!WIFEXITED(status))
		return FAIL_KILLED;
	if (WEXITSTATUS(status) != PASS)
		return WEXITSTATUS(status);
	/* The child's exit took the parent's share off the counter. */
	if (*after < own || *before - *after >= own)
		return FAIL_COUNTER;
	if (*after != *before)
		return CHANGED;
	if (mem[pmdsize] != (shared ? 'c' : 2) || !data_ok(mem, size, pmdsize) ||
	    !check_huge_shmem(mem, size - pmdsize, NR_PMDS - 1, pmdsize))
		return FAIL_PARENT;
	return PASS;
}

static void test_fork(bool shared)
{
	size_t size = NR_PMDS * pmdsize, last = size - pagesize, off;
	const char *desc = shared ? "shared mapping with a guard region" :
				    "private mapping with an anon_vma";
	char *buf, *area, *mem;
	enum result res = FAIL_PARENT;
	long before, after;
	int fd, i;

	fd = open("/tmp/fork_file_thp", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		ksft_exit_fail_perror("open");
	unlink("/tmp/fork_file_thp");
	buf = malloc(pmdsize);
	if (!buf)
		ksft_exit_fail_msg("malloc failed\n");
	for (off = 0; off < size; off += pmdsize) {
		memset(buf, off / pmdsize + 1, pmdsize);
		if (pwrite(fd, buf, pmdsize, off) != (ssize_t)pmdsize)
			ksft_exit_fail_perror("pwrite");
	}
	free(buf);

	area = mmap(NULL, size + pmdsize, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		ksft_exit_fail_perror("mmap");
	mem = (char *)(((unsigned long)area + pmdsize - 1) & ~(pmdsize - 1));
	mem = mmap(mem, size, PROT_READ | PROT_WRITE,
		   (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED, fd, 0);
	if (mem == MAP_FAILED)
		ksft_exit_fail_perror("mmap");
	for (off = 0; off < size; off += pmdsize)
		FORCE_READ(mem[off]);

	/* Make fork copy the page tables; the last PMD gets zapped. */
	if (shared) {
		if (madvise(mem + last, pagesize, MADV_GUARD_INSTALL)) {
			ksft_test_result_skip("%s: MADV_GUARD_INSTALL: %s\n",
					      desc, strerror(errno));
			goto out;
		}
	} else {
		mem[last] = 'p';
	}

	if (!check_huge_shmem(mem, size - pmdsize, NR_PMDS - 1, pmdsize)) {
		ksft_test_result_skip("%s: shmem is not mapped with PMDs\n",
				      desc);
		goto out;
	}

	for (i = 0; i < ATTEMPTS; i++) {
		res = fork_once(mem, size, shared, &before, &after);
		if (res != CHANGED)
			break;
	}

	switch (res) {
	case PASS:
		ksft_test_result_pass("%s\n", desc);
		break;
	case NO_PMDS:
		ksft_test_result_skip("%s: fork does not copy file THPs\n", desc);
		break;
	case COUNTER_OFF:
		ksft_test_result_skip("%s: ShmemPmdMapped %ld kB is below what this process maps\n",
				      desc, before);
		break;
	case CHANGED:
		ksft_test_result_skip("%s: ShmemPmdMapped keeps changing\n", desc);
		break;
	case FAIL_DATA:
		ksft_test_result_fail("%s: child sees wrong data\n", desc);
		break;
	case FAIL_COUNTER:
		ksft_test_result_fail("%s: ShmemPmdMapped %ld kB before fork, %ld kB after\n",
				      desc, before, after);
		break;
	case FAIL_PARENT:
		ksft_test_result_fail("%s: parent's mapping changed\n", desc);
		break;
	case FAIL_KILLED:
		ksft_test_result_fail("%s: child killed by a signal\n", desc);
		break;
	default:
		ksft_test_result_fail("%s: child error %d\n", desc, res);
		break;
	}
out:
	munmap(area, size + pmdsize);
	close(fd);
}

int main(void)
{
	ksft_print_header();
	pagesize = getpagesize();
	pmdsize = read_pmd_pagesize();
	if (!pmdsize)
		ksft_exit_skip("THP is not supported\n");
	if (pmdsize > MAX_PMDSIZE)
		ksft_exit_skip("PMD size %zu MiB is too large\n", pmdsize >> 20);
	if (geteuid())
		ksft_exit_skip("needs root to mount tmpfs\n");
	/* The tmpfs over /tmp lives and dies with a private mount namespace. */
	if (unshare(CLONE_NEWNS) || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
		ksft_exit_skip("private mount namespace: %s\n", strerror(errno));
	if (mount("tmpfs", "/tmp", "tmpfs", 0, "huge=always"))
		ksft_exit_skip("tmpfs with huge=always: %s\n", strerror(errno));

	ksft_set_plan(2);
	test_fork(false);
	test_fork(true);
	ksft_finished();
}
