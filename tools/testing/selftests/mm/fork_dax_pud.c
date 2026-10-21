// SPDX-License-Identifier: GPL-2.0
/*
 * fork() of a process that maps a device DAX instance with a PUD, in a VMA
 * whose page tables fork copies because it has a guard region. The child
 * must account the copied PUD in its RssFile.
 *
 * Runs only on a device named in argv[1] or $FORK_DAX_PUD_DEVICE that
 * nothing else maps, with 1 GiB alignment and at least 2 GiB. To make one
 * on pmem emulated in RAM, with CONFIG_X86_PMEM_LEGACY and
 * CONFIG_DEV_DAX_PMEM:
 *
 *	boot with memmap=3G!4G
 *	ndctl create-namespace -f -e namespace0.0 -m devdax -a 1G
 *	FORK_DAX_PUD_DEVICE=/dev/dax0.0 ./fork_dax_pud
 */
#define _GNU_SOURCE
#include <asm-generic/mman.h> /* Force the import of the tools version. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "kselftest.h"
#include "vm_util.h"

#define SZ_1G		(1UL << 30)
#define DAX_SYSFS	"/sys/bus/dax/devices"

static sigjmp_buf sigbus_jmp;

static void sigbus_handler(int sig)
{
	siglongjmp(sigbus_jmp, 1);
}

static unsigned long sysfs_ul(const char *dev, const char *attr)
{
	char path[PATH_MAX], buf[64];
	unsigned long val = 0;
	FILE *f;

	snprintf(path, sizeof(path), DAX_SYSFS "/%s/%s", dev, attr);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fgets(buf, sizeof(buf), f))
		val = strtoul(buf, NULL, 0);
	fclose(f);
	return val;
}

static bool dax_dev_ok(const char *path)
{
	const char *name = strrchr(path, '/');

	name = name ? name + 1 : path;
	return sysfs_ul(name, "align") == SZ_1G &&
	       sysfs_ul(name, "size") >= 2 * SZ_1G;
}

static long rss_file_kb(void)
{
	char line[256];
	long kb = -1;
	FILE *f = fopen("/proc/self/status", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "RssFile: %ld", &kb) == 1)
			break;
	fclose(f);
	return kb;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : getenv("FORK_DAX_PUD_DEVICE");
	char *area, *mem;
	int fd, status;
	pid_t pid;

	ksft_print_header();
	if (!path)
		ksft_exit_skip("no devdax device given\n");
	if (!dax_dev_ok(path))
		ksft_exit_skip("%s: needs 1 GiB alignment and 2 GiB\n", path);

	fd = open(path, O_RDONLY);
	if (fd < 0)
		ksft_exit_skip("%s: %s\n", path, strerror(errno));
	area = mmap(NULL, 3 * SZ_1G, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		ksft_exit_fail_perror("mmap");
	mem = (char *)(((unsigned long)area + SZ_1G - 1) & ~(SZ_1G - 1));
	mem = mmap(mem, 2 * SZ_1G, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
	if (mem == MAP_FAILED)
		ksft_exit_skip("mmap %s: %s\n", path, strerror(errno));

	/* A PUD in the first GiB, a guard page at the end of the second. */
	signal(SIGBUS, sigbus_handler);
	if (sigsetjmp(sigbus_jmp, 1))
		ksft_exit_skip("%s cannot be mapped with a PUD\n", path);
	FORCE_READ(mem[0]);
	signal(SIGBUS, SIG_DFL);
	if (madvise(mem + 2 * SZ_1G - getpagesize(), getpagesize(),
		    MADV_GUARD_INSTALL))
		ksft_exit_skip("MADV_GUARD_INSTALL: %s\n", strerror(errno));

	ksft_set_plan(1);
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		ksft_exit_fail_perror("fork");
	if (!pid)
		_exit(rss_file_kb() < (long)(SZ_1G >> 10) ? 10 : 0);
	if (waitpid(pid, &status, 0) != pid)
		ksft_exit_fail_perror("waitpid");

	if (!WIFEXITED(status) || WEXITSTATUS(status))
		ksft_test_result_fail("child does not account the copied PUD\n");
	else
		ksft_test_result_pass("devdax PUD copied by fork\n");

	munmap(area, 3 * SZ_1G);
	close(fd);
	ksft_finished();
}
