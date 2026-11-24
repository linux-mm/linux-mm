// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <linux/limits.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include "kselftest.h"
#include "cgroup_util.h"

#define ADDR ((void *)(0x0UL))
#define FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB)
/* mapping 8 MBs == 4 hugepages */
#define LENGTH (8UL*1024*1024)
#define PROTECTION (PROT_READ | PROT_WRITE)

/* borrowed from mm/hmm-tests.c */
static long get_meminfo(const char *tag)
{
	int fd;
	char buf[2048];
	int len;
	char *p, *q, *path = "/proc/meminfo";
	long val;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		/* Error opening the file */
		return -1;
	}

	len = read(fd, buf, sizeof(buf));
	close(fd);
	if (len < 0) {
		/* Error in reading the file */
		return -1;
	}
	if (len == sizeof(buf)) {
		/* Error file is too large */
		return -1;
	}
	buf[len] = '\0';

	/* Search for a tag if provided */
	if (tag) {
		p = strstr(buf, tag);
		if (!p)
			return -1; /* looks like the line we want isn't there */
		p += strlen(tag);
	} else
		p = buf;

	val = strtol(p, &q, 0);
	if (*q != ' ' && *q != '\n') {
		/* Error parsing the file */
		return -1;
	}

	return val;
}

static long get_hugepage_size(void)
{
	return get_meminfo("Hugepagesize:");
}

static int set_file(const char *path, long value)
{
	FILE *file;
	int ret;

	file = fopen(path, "w");
	if (!file)
		return -1;
	ret = fprintf(file, "%ld\n", value);
	fclose(file);
	return ret;
}

static int set_nr_hugepages(long value)
{
	return set_file("/proc/sys/vm/nr_hugepages", value);
}

static unsigned int check_first(char *addr)
{
	return *(unsigned int *)addr;
}

static void write_data(char *addr)
{
	unsigned long i;

	for (i = 0; i < LENGTH; i++)
		*(addr + i) = (char)i;
}

static int hugetlb_test_program(const char *cgroup, void *arg)
{
	char *test_group = (char *)arg;
	void *addr;
	long old_current, expected_current, current;
	int ret = EXIT_FAILURE;

	old_current = cg_read_long(test_group, "memory.current");
	set_nr_hugepages(20);
	current = cg_read_long(test_group, "memory.current");
	if (current - old_current >= MB(2)) {
		ksft_print_msg(
			"setting nr_hugepages should not increase hugepage usage.\n");
		ksft_print_msg("before: %ld, after: %ld\n", old_current, current);
		return EXIT_FAILURE;
	}

	addr = mmap(ADDR, LENGTH, PROTECTION, FLAGS, 0, 0);
	if (addr == MAP_FAILED) {
		ksft_print_msg("fail to mmap.\n");
		return EXIT_FAILURE;
	}
	current = cg_read_long(test_group, "memory.current");
	if (current - old_current >= MB(2)) {
		ksft_print_msg("mmap should not increase hugepage usage.\n");
		ksft_print_msg("before: %ld, after: %ld\n", old_current, current);
		goto out_failed_munmap;
	}
	old_current = current;

	/* read the first page */
	check_first(addr);
	expected_current = old_current + MB(2);
	current = cg_read_long(test_group, "memory.current");
	if (!values_close(expected_current, current, 5)) {
		ksft_print_msg("memory usage should increase by around 2MB.\n");
		ksft_print_msg(
			"expected memory: %ld, actual memory: %ld\n",
			expected_current, current);
		goto out_failed_munmap;
	}

	/* write to the whole range */
	write_data(addr);
	current = cg_read_long(test_group, "memory.current");
	expected_current = old_current + MB(8);
	if (!values_close(expected_current, current, 5)) {
		ksft_print_msg("memory usage should increase by around 8MB.\n");
		ksft_print_msg(
			"expected memory: %ld, actual memory: %ld\n",
			expected_current, current);
		goto out_failed_munmap;
	}

	/* unmap the whole range */
	munmap(addr, LENGTH);
	current = cg_read_long(test_group, "memory.current");
	expected_current = old_current;
	if (!values_close(expected_current, current, 5)) {
		ksft_print_msg("memory usage should go back down.\n");
		ksft_print_msg(
			"expected memory: %ld, actual memory: %ld\n",
			expected_current, current);
		return ret;
	}

	ret = EXIT_SUCCESS;
	return ret;

out_failed_munmap:
	munmap(addr, LENGTH);
	return ret;
}

static int test_hugetlb_memcg(char *root)
{
	int ret = KSFT_FAIL;
	char *test_group;

	test_group = cg_name(root, "hugetlb_memcg_test");
	if (!test_group || cg_create(test_group)) {
		ksft_print_msg("fail to create cgroup.\n");
		goto out;
	}

	if (cg_write(test_group, "memory.max", "100M")) {
		ksft_print_msg("fail to set cgroup memory limit.\n");
		goto out;
	}

	/* disable swap */
	if (cg_write(test_group, "memory.swap.max", "0")) {
		ksft_print_msg("fail to disable swap.\n");
		goto out;
	}

	if (!cg_run(test_group, hugetlb_test_program, (void *)test_group))
		ret = KSFT_PASS;
out:
	cg_destroy(test_group);
	free(test_group);
	return ret;
}

struct opt_out_args {
	const char *root;
	const char *child;
	const char *parent;
	const char *grandparent;
	bool parent_charged;
	bool grandparent_charged;
};

static bool opt_out_charged(const char *name, long delta, bool charged)
{
	if (charged && values_close(MB(8), delta, 10))
		return true;
	if (!charged && delta < MB(2))
		return true;

	ksft_print_msg("hugetlb usage should%s be charged to the %s.\n",
		       charged ? "" : " not", name);
	ksft_print_msg("%s memory increased by %ld\n", name, delta);
	return false;
}

static long root_hugetlb(const char *root)
{
	return cg_read_key_long(root, "memory.stat", "hugetlb ");
}

/*
 * Use 4 hugepages in the child. They must never be charged to the child, and
 * be charged to the parent and the grandparent as the caller expects.
 * memory.current is hierarchical, so a charge to the parent also shows up in
 * the grandparent. Wherever the charge lands, the root must count the pages.
 */
static int hugetlb_opt_out_program(const char *cgroup, void *arg)
{
	struct opt_out_args *args = arg;
	long root, child, parent, grandparent, cur;
	int ret = EXIT_FAILURE;
	void *addr;

	root = root_hugetlb(args->root);
	child = cg_read_long(args->child, "memory.current");
	parent = cg_read_long(args->parent, "memory.current");
	grandparent = cg_read_long(args->grandparent, "memory.current");
	if (root < 0 || child < 0 || parent < 0 || grandparent < 0) {
		ksft_print_msg("fail to read the memory usage.\n");
		return EXIT_FAILURE;
	}

	addr = mmap(ADDR, LENGTH, PROTECTION, FLAGS, 0, 0);
	if (addr == MAP_FAILED) {
		ksft_print_msg("fail to mmap.\n");
		return EXIT_FAILURE;
	}
	write_data(addr);

	cur = cg_read_long(args->child, "memory.current");
	if (cur < 0 || !opt_out_charged("child", cur - child, false))
		goto out;

	cur = cg_read_long(args->parent, "memory.current");
	if (cur < 0 ||
	    !opt_out_charged("parent", cur - parent, args->parent_charged))
		goto out;

	cur = cg_read_long(args->grandparent, "memory.current");
	if (cur < 0 || !opt_out_charged("grandparent", cur - grandparent,
					args->grandparent_charged))
		goto out;

	/* memory.stat is flushed lazily, give the root time to catch up */
	cur = cg_read_key_long_poll(args->root, "memory.stat", "hugetlb ",
				    root + LENGTH, 30, DEFAULT_WAIT_INTERVAL_US);
	if (cur < 0 || !opt_out_charged("root", cur - root, true))
		goto out;

	munmap(addr, LENGTH);
	cur = cg_read_long(args->grandparent, "memory.current");
	if (cur < 0 || cur - grandparent >= MB(2)) {
		ksft_print_msg("memory usage should go back down.\n");
		ksft_print_msg("grandparent memory is now %ld, was %ld\n",
			       cur, grandparent);
		return ret;
	}

	return EXIT_SUCCESS;

out:
	munmap(addr, LENGTH);
	return ret;
}

static int test_hugetlb_memcg_opt_out(char *root)
{
	char *grandparent, *parent = NULL, *child = NULL;
	struct opt_out_args args;
	int ret = KSFT_FAIL;

	grandparent = cg_name(root, "hugetlb_memcg_grandparent");
	if (!grandparent || cg_create(grandparent)) {
		ksft_print_msg("fail to create cgroup.\n");
		goto out;
	}

	if (cg_write(grandparent, "cgroup.subtree_control", "+memory")) {
		ksft_print_msg("fail to enable the memory controller.\n");
		goto out;
	}

	parent = cg_name(grandparent, "parent");
	if (!parent || cg_create(parent)) {
		ksft_print_msg("fail to create cgroup.\n");
		goto out;
	}

	if (cg_write(parent, "cgroup.subtree_control", "+memory")) {
		ksft_print_msg("fail to enable the memory controller.\n");
		goto out;
	}

	child = cg_name(parent, "child");
	if (!child || cg_create(child)) {
		ksft_print_msg("fail to create cgroup.\n");
		goto out;
	}

	if (cg_read_long(child, "memory.hugetlb_accounting") < 0) {
		ret = KSFT_SKIP;
		goto out;
	}

	set_nr_hugepages(20);
	if (get_meminfo("HugePages_Free:") < 4) {
		ksft_print_msg("not enough free hugepages.\n");
		ret = KSFT_SKIP;
		goto out;
	}

	args.root = root;
	args.child = child;
	args.parent = parent;
	args.grandparent = grandparent;

	/* The child opts out: its hugetlb usage is charged to the parent. */
	if (cg_write(child, "memory.hugetlb_accounting", "0"))
		goto out;
	args.parent_charged = true;
	args.grandparent_charged = true;
	if (cg_run(child, hugetlb_opt_out_program, &args))
		goto out;

	/* The parent opts out too: the grandparent is charged, not the parent. */
	if (cg_write(parent, "memory.hugetlb_accounting", "0"))
		goto out;
	args.parent_charged = false;
	if (cg_run(child, hugetlb_opt_out_program, &args))
		goto out;

	/* The child cannot turn the accounting back on for itself. */
	if (cg_write(child, "memory.hugetlb_accounting", "1"))
		goto out;
	if (cg_run(child, hugetlb_opt_out_program, &args))
		goto out;

	/* The grandparent opts out as well: only the root counts the pages. */
	if (cg_write(grandparent, "memory.hugetlb_accounting", "0"))
		goto out;
	args.grandparent_charged = false;
	if (cg_run(child, hugetlb_opt_out_program, &args))
		goto out;

	ret = KSFT_PASS;
out:
	if (child)
		cg_destroy(child);
	if (parent)
		cg_destroy(parent);
	cg_destroy(grandparent);
	free(child);
	free(parent);
	free(grandparent);
	return ret;
}

int main(int argc, char **argv)
{
	char root[PATH_MAX];
	int has_memory_hugetlb_acc;

	ksft_print_header();
	ksft_set_plan(2);

	has_memory_hugetlb_acc = proc_mount_contains("memory_hugetlb_accounting");
	if (has_memory_hugetlb_acc < 0)
		ksft_exit_skip("Failed to query cgroup mount option\n");
	else if (!has_memory_hugetlb_acc)
		ksft_exit_skip("memory hugetlb accounting is disabled\n");

	/* Unit is kB! */
	if (get_hugepage_size() != 2048) {
		ksft_print_msg("test_hugetlb_memcg requires 2MB hugepages\n");
		ksft_test_result_skip("test_hugetlb_memcg\n");
		ksft_test_result_skip("test_hugetlb_memcg_opt_out\n");
		ksft_finished();
	}

	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");

	if (cg_read_strstr(root, "cgroup.controllers", "memory"))
		ksft_exit_skip("memory controller isn't available\n");

	if (cg_read_strstr(root, "cgroup.subtree_control", "memory")) {
		if (cg_write(root, "cgroup.subtree_control", "+memory"))
			ksft_exit_skip("Failed to set memory controller\n");
	}

	switch (test_hugetlb_memcg(root)) {
	case KSFT_PASS:
		ksft_test_result_pass("test_hugetlb_memcg\n");
		break;
	case KSFT_SKIP:
		ksft_test_result_skip("test_hugetlb_memcg\n");
		break;
	default:
		ksft_test_result_fail("test_hugetlb_memcg\n");
		break;
	}

	switch (test_hugetlb_memcg_opt_out(root)) {
	case KSFT_PASS:
		ksft_test_result_pass("test_hugetlb_memcg_opt_out\n");
		break;
	case KSFT_SKIP:
		ksft_test_result_skip("test_hugetlb_memcg_opt_out\n");
		break;
	default:
		ksft_test_result_fail("test_hugetlb_memcg_opt_out\n");
		break;
	}

	ksft_finished();
}
