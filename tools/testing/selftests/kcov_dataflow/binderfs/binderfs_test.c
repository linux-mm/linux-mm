// SPDX-License-Identifier: GPL-2.0
/*
 * binderfs selftest for kcov_dataflow
 *
 * Exercises the binder driver via binderfs with kcov_dataflow recording
 * active, then verifies that function argument records were captured at
 * binder ioctl boundaries.
 *
 * Requires: CONFIG_ANDROID_BINDER_IPC=y (or _RUST), CONFIG_ANDROID_BINDERFS=y
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include <linux/kcov_dataflow.h>


#define BUF_SIZE	(1 << 20)
#define BINDERFS_PATH	"/tmp/binderfs_test"
#define BINDER_DEV	BINDERFS_PATH "/my_binder"

static int setup_binderfs(void)
{
	struct binderfs_device dev = {};

	mkdir(BINDERFS_PATH, 0755);

	if (mount("binder", BINDERFS_PATH, "binder", 0, NULL)) {
		if (errno == ENODEV || errno == ENOENT) {
			printf("SKIP: binderfs not available\n");
			return -1;
		}
		perror("mount binderfs");
		return -1;
	}

	/* Create a binder device via BINDER_CTL_ADD ioctl */
	int ctl_fd;

	ctl_fd = open(BINDERFS_PATH "/binder-control", O_RDONLY);
	if (ctl_fd < 0) {
		perror("open binder-control");
		umount(BINDERFS_PATH);
		return -1;
	}

	strcpy(dev.name, "my_binder");
	if (ioctl(ctl_fd, BINDER_CTL_ADD, &dev) && errno != EEXIST) {
		perror("BINDER_CTL_ADD");
		close(ctl_fd);
		umount(BINDERFS_PATH);
		return -1;
	}
	close(ctl_fd);
	return 0;
}

static void cleanup_binderfs(void)
{
	umount(BINDERFS_PATH);
	rmdir(BINDERFS_PATH);
}

int main(void)
{
	uint64_t *buf;
	int df_fd, binder_fd;
	uint64_t total;
	int valid = 0;

	printf("TAP version 13\n");
	printf("1..3\n");

	/* Setup binderfs */
	if (setup_binderfs()) {
		printf("ok 1 # SKIP binderfs not available\n");
		printf("ok 2 # SKIP\n");
		printf("ok 3 # SKIP\n");
		return 0;
	}

	/* Open kcov_dataflow */
	df_fd = open("/sys/kernel/debug/kcov_dataflow", O_RDWR);
	if (df_fd < 0) {
		printf("not ok 1 cannot open kcov_dataflow\n");
		cleanup_binderfs();
		return 1;
	}

	if (ioctl(df_fd, KCOV_DF_INIT_TRACK, BUF_SIZE)) {
		printf("not ok 1 INIT_TRACK failed\n");
		close(df_fd);
		cleanup_binderfs();
		return 1;
	}

	buf = mmap(NULL, BUF_SIZE * sizeof(uint64_t),
		   PROT_READ | PROT_WRITE, MAP_SHARED, df_fd, 0);
	if (buf == MAP_FAILED) {
		printf("not ok 1 mmap failed\n");
		close(df_fd);
		cleanup_binderfs();
		return 1;
	}

	printf("ok 1 kcov_dataflow.binderfs_setup\n");

	/* Open binder device */
	binder_fd = open(BINDER_DEV, O_RDWR | O_CLOEXEC);
	if (binder_fd < 0) {
		printf("not ok 2 cannot open %s: %s\n", BINDER_DEV,
		       strerror(errno));
		munmap(buf, BUF_SIZE * sizeof(uint64_t));
		close(df_fd);
		cleanup_binderfs();
		return 1;
	}

	/* Enable recording and exercise binder ioctls */
	ioctl(df_fd, KCOV_DF_ENABLE, 0);
	__atomic_store_n(&buf[0], 0, __ATOMIC_RELAXED);

	/* BINDER_VERSION - simple ioctl that exercises the binder path */
	struct binder_version ver = {};

	ioctl(binder_fd, BINDER_VERSION, &ver);

	/* BINDER_SET_MAX_THREADS */
	uint32_t max_threads = 4;

	ioctl(binder_fd, BINDER_SET_MAX_THREADS, &max_threads);

	ioctl(df_fd, KCOV_DF_DISABLE, 0);

	total = __atomic_load_n(&buf[0], __ATOMIC_RELAXED);
	close(binder_fd);

	if (total > 0)
		printf("ok 2 kcov_dataflow.binderfs_captured # %lu words\n",
		       (unsigned long)total);
	else
		printf("not ok 2 kcov_dataflow.binderfs_captured # 0 words\n");

	/*
	 * Walk the records: every header must carry a known type and at least
	 * one value word, the walk must end exactly at area[0], and at least one
	 * ENTRY/RET record must come from the binder ioctls (CMP records are
	 * interleaved with CONFIG_KCOV_ENABLE_COMPARISONS=y).
	 */
	if (total <= BUF_SIZE - 1) {
		uint64_t pos = 1, end = 1 + total;
		unsigned long nargs = 0;

		while (pos + KCOV_DF_RECORD_HDR_WORDS <= end) {
			uint64_t hdr = buf[pos];
			uint32_t type = KCOV_DF_HDR_TYPE(hdr);
			uint32_t nvals = KCOV_DF_HDR_NVALS(hdr);

			if (nvals < 1 || (type != KCOV_DF_TYPE_ENTRY &&
					  type != KCOV_DF_TYPE_RET &&
					  type != KCOV_DF_TYPE_CMP))
				break;
			if (type != KCOV_DF_TYPE_CMP)
				nargs++;
			pos += KCOV_DF_RECORD_WORDS(nvals);
		}
		if (pos == end && nargs > 0)
			valid = 1;
		else
			printf("# walk stopped at word %lu of %lu, %lu ENTRY/RET records\n",
			       (unsigned long)pos, (unsigned long)end, nargs);
	}

	if (valid)
		printf("ok 3 kcov_dataflow.binderfs_valid_records\n");
	else
		printf("not ok 3 kcov_dataflow.binderfs_valid_records\n");

	printf("# Totals: pass:%d fail:%d skip:0\n",
	       valid ? 3 : 2, valid ? 0 : 1);

	munmap(buf, BUF_SIZE * sizeof(uint64_t));
	close(df_fd);
	cleanup_binderfs();
	return valid ? 0 : 1;
}
