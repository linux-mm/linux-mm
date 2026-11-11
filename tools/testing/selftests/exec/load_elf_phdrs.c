// SPDX-License-Identifier: GPL-2.0
/*
 * execve() of an ELF file whose program header table cannot be loaded.
 *
 * A malformed, misplaced or truncated table fails with ENOEXEC, and with
 * ELIBBAD when it is the interpreter's. A failed allocation of the table is
 * reported as ENOMEM, never as one of those two: checked as root with
 * CONFIG_FAULT_INJECTION, CONFIG_FAILSLAB and CONFIG_FAULT_INJECTION_DEBUG_FS
 * (so CONFIG_DEBUG_FS), skipped without them.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <linux/magic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest_harness.h"

/* The test itself could not fork or arm the fault, as opposed to an errno. */
#define SETUP_FAILED	(-2)

enum table {
	BAD_PHENTSIZE,
	NO_PHDRS,
	TOO_BIG,
	HUGE_PHOFF,	/* on 32-bit only past the end of the file */
	PAST_EOF,
	TRUNCATED,
};

static int write_file(const char *path, const void *buf, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
	ssize_t ret;

	if (fd < 0)
		return -1;
	ret = write(fd, buf, len);
	close(fd);
	return ret == (ssize_t)len ? 0 : -1;
}

static int read_ehdr(ElfW(Ehdr) *eh)
{
	int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
	ssize_t ret;

	if (fd < 0)
		return -1;
	ret = read(fd, eh, sizeof(*eh));
	close(fd);
	return ret == sizeof(*eh) ? 0 : -1;
}

/* An ELF header for this machine with one program header, then that header. */
static size_t one_phdr_image(char *buf, const ElfW(Phdr) *ph)
{
	ElfW(Ehdr) eh;

	if (read_ehdr(&eh))
		return 0;
	eh.e_phoff = sizeof(eh);
	eh.e_phentsize = sizeof(*ph);
	eh.e_phnum = 1;
	eh.e_shoff = 0;
	eh.e_shnum = 0;
	eh.e_shstrndx = 0;
	memcpy(buf, &eh, sizeof(eh));
	memcpy(buf + sizeof(eh), ph, sizeof(*ph));
	return sizeof(eh) + sizeof(*ph);
}

static size_t bad_table_image(char *buf, enum table table)
{
	ElfW(Phdr) ph = { .p_type = PT_NULL };
	ElfW(Ehdr) *eh = (ElfW(Ehdr) *)buf;
	size_t len = one_phdr_image(buf, &ph);

	if (!len)
		return 0;
	switch (table) {
	case BAD_PHENTSIZE:
		eh->e_phentsize--;
		break;
	case NO_PHDRS:
		eh->e_phnum = 0;
		break;
	case TOO_BIG:
		eh->e_phnum = 65536 / sizeof(ph) + 1;
		break;
	case HUGE_PHOFF:
		eh->e_phoff = (ElfW(Off))1 << (8 * sizeof(eh->e_phoff) - 1);
		break;
	case PAST_EOF:
		eh->e_phoff = 1 << 20;
		break;
	case TRUNCATED:
		eh->e_phnum = 2;
		break;
	}
	return len;
}

/* An executable whose PT_INTERP names @interp. */
static size_t interp_image(char *buf, const char *interp)
{
	ElfW(Phdr) ph = {
		.p_type = PT_INTERP,
		.p_offset = sizeof(ElfW(Ehdr)) + sizeof(ElfW(Phdr)),
		.p_filesz = strlen(interp) + 1,
	};
	size_t len = one_phdr_image(buf, &ph);

	if (!len)
		return 0;
	memcpy(buf + len, interp, ph.p_filesz);
	return len + ph.p_filesz;
}

/* Returns the errno of a failed execve(), 0 if it did not return. */
static int exec_errno(const char *path, char *const argv[])
{
	char *const envp[] = { NULL };
	int pipefd[2], err = 0, status;
	pid_t pid;

	if (pipe2(pipefd, O_CLOEXEC))
		return SETUP_FAILED;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return SETUP_FAILED;
	}
	if (!pid) {
		execve(path, argv, envp);
		err = errno;
		write(pipefd[1], &err, sizeof(err));
		_exit(1);
	}
	close(pipefd[1]);
	if (read(pipefd[0], &err, sizeof(err)) != sizeof(err))
		err = 0;
	close(pipefd[0]);
	waitpid(pid, &status, 0);
	return err;
}

FIXTURE(table) {
	char dir[64];
	char exe[96];
	char interp[96];
};

FIXTURE_VARIANT(table) {
	enum table table;
	bool interp;
};

FIXTURE_VARIANT_ADD(table, bad_phentsize) { .table = BAD_PHENTSIZE };
FIXTURE_VARIANT_ADD(table, no_phdrs) { .table = NO_PHDRS };
FIXTURE_VARIANT_ADD(table, too_big) { .table = TOO_BIG };
FIXTURE_VARIANT_ADD(table, huge_phoff) { .table = HUGE_PHOFF };
FIXTURE_VARIANT_ADD(table, past_eof) { .table = PAST_EOF };
FIXTURE_VARIANT_ADD(table, truncated) { .table = TRUNCATED };
FIXTURE_VARIANT_ADD(table, interp_bad_phentsize) { .table = BAD_PHENTSIZE, .interp = true };
FIXTURE_VARIANT_ADD(table, interp_no_phdrs) { .table = NO_PHDRS, .interp = true };
FIXTURE_VARIANT_ADD(table, interp_too_big) { .table = TOO_BIG, .interp = true };
FIXTURE_VARIANT_ADD(table, interp_huge_phoff) { .table = HUGE_PHOFF, .interp = true };
FIXTURE_VARIANT_ADD(table, interp_past_eof) { .table = PAST_EOF, .interp = true };
FIXTURE_VARIANT_ADD(table, interp_truncated) { .table = TRUNCATED, .interp = true };

static void remove_files(FIXTURE_DATA(table) *self)
{
	unlink(self->exe);
	unlink(self->interp);
	rmdir(self->dir);
}

FIXTURE_SETUP(table)
{
	char bad[sizeof(ElfW(Ehdr)) + sizeof(ElfW(Phdr))];
	char exe[sizeof(bad) + sizeof(self->interp)];
	size_t bad_len, exe_len;
	struct statvfs st;
	int ret;

	ASSERT_EQ(0, statvfs("/tmp", &st));
	if (st.f_flag & ST_NOEXEC)
		SKIP(return, "/tmp is on a noexec mount");
	bad_len = bad_table_image(bad, variant->table);
	ASSERT_NE(0, bad_len);
	strcpy(self->dir, "/tmp/load_elf_phdrs.XXXXXX");
	ASSERT_NE(NULL, mkdtemp(self->dir));
	snprintf(self->exe, sizeof(self->exe), "%s/exe", self->dir);
	snprintf(self->interp, sizeof(self->interp), "%s/interp", self->dir);

	if (!variant->interp) {
		ret = write_file(self->exe, bad, bad_len);
	} else {
		exe_len = interp_image(exe, self->interp);
		ret = !exe_len || write_file(self->interp, bad, bad_len) ||
		      write_file(self->exe, exe, exe_len);
	}
	if (ret)
		remove_files(self);
	ASSERT_EQ(0, ret);
}

FIXTURE_TEARDOWN(table)
{
	remove_files(self);
}

TEST_F(table, execve)
{
	char *const argv[] = { self->exe, NULL };
	int expect = variant->interp ? ELIBBAD : ENOEXEC;
	int err = exec_errno(self->exe, argv);

	ASSERT_NE(SETUP_FAILED, err);
	EXPECT_EQ(expect, err) {
		TH_LOG("execve(): %s, expected %s", strerror(err), strerror(expect));
	}
}

#define DEBUGFS		"/sys/kernel/debug"
#define IGNORE_GFP_WAIT	DEBUGFS "/failslab/ignore-gfp-wait"
#define VERBOSE		DEBUGFS "/failslab/verbose"

static int read_str(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t ret;

	if (fd < 0)
		return -1;
	ret = read(fd, buf, size - 1);
	close(fd);
	if (ret <= 0)
		return -1;
	buf[ret] = '\0';
	return 0;
}

static int write_str(const char *path, const char *str)
{
	return write_file(path, str, strlen(str));
}

/* errno of execve() with fault @nth injected; 0: the new image exited, -1: it died */
static int exec_fail_nth(const char *path, unsigned int nth)
{
	char *const argv[] = { (char *)path, NULL };
	char *const envp[] = { NULL };
	int pipefd[2], err, status;
	char buf[16];
	pid_t pid;

	if (pipe2(pipefd, O_CLOEXEC))
		return SETUP_FAILED;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return SETUP_FAILED;
	}
	if (!pid) {
		struct rlimit no_core = { 0, 0 };
		int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
		int fd;

		/* Faults past the point of no return kill the new image. */
		setrlimit(RLIMIT_CORE, &no_core);
		if (null >= 0) {
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
		}
		fd = open("/proc/thread-self/fail-nth", O_WRONLY | O_CLOEXEC);
		if (fd < 0 || write(fd, buf, sprintf(buf, "%u", nth)) < 0)
			_exit(2);
		execve(path, argv, envp);
		err = errno;
		write(fd, "0", 1);
		write(pipefd[1], &err, sizeof(err));
		_exit(1);
	}
	close(pipefd[1]);
	if (read(pipefd[0], &err, sizeof(err)) != sizeof(err))
		err = 0;
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) != pid ||
	    (WIFEXITED(status) && WEXITSTATUS(status) == 2))
		return SETUP_FAILED;
	if (err)
		return err;
	return WIFEXITED(status) && WEXITSTATUS(status) == 1 ? 0 : -1;
}

FIXTURE(alloc) {
	char ignore_gfp_wait[8];
	char verbose[8];
	bool mounted;
};

static void failslab_restore(FIXTURE_DATA(alloc) *self)
{
	write_str(IGNORE_GFP_WAIT, self->ignore_gfp_wait);
	write_str(VERBOSE, self->verbose);
	if (self->mounted)
		umount(DEBUGFS);
}

FIXTURE_SETUP(alloc)
{
	struct statfs st;
	int ret;

	self->mounted = false;
	if (geteuid())
		SKIP(return, "needs root for the failslab settings");
	if (access("/proc/thread-self/fail-nth", W_OK))
		SKIP(return, "no fault injection (CONFIG_FAULT_INJECTION)");
	if (statfs(DEBUGFS, &st) || st.f_type != DEBUGFS_MAGIC)
		self->mounted = !mount("debugfs", DEBUGFS, "debugfs", 0, NULL);
	if (read_str(IGNORE_GFP_WAIT, self->ignore_gfp_wait, sizeof(self->ignore_gfp_wait)) ||
	    read_str(VERBOSE, self->verbose, sizeof(self->verbose))) {
		if (self->mounted)
			umount(DEBUGFS);
		SKIP(return, "no debugfs or no failslab (CONFIG_FAULT_INJECTION_DEBUG_FS)");
	}
	/* The allocations of an execve() may sleep, and a splat for each is noise. */
	ret = write_str(IGNORE_GFP_WAIT, "N") ?: write_str(VERBOSE, "0");
	if (ret) {
		ret = errno;
		failslab_restore(self);
		SKIP(return, "cannot set up failslab: %s", strerror(ret));
	}
}

FIXTURE_TEARDOWN(alloc)
{
	failslab_restore(self);
}

TEST_F(alloc, no_enoexec)
{
	unsigned int nth, clean = 0, failed = 0, wrong = 0;
	char path[PATH_MAX];
	ssize_t len;
	int err;

	/* Dynamically linked, so that the interpreter's table is allocated too. */
	len = readlink("/proc/self/exe", path, sizeof(path) - sizeof("false-dynamic"));
	ASSERT_LT(0, len);
	path[len] = '\0';
	strcpy(strrchr(path, '/') + 1, "false-dynamic");
	if (access(path, X_OK))
		SKIP(return, "false-dynamic not found next to the test");

	/* Done once the faults to inject lie past the whole exec and exit. */
	for (nth = 1; clean < 50 && nth < 100000; nth++) {
		err = exec_fail_nth(path, nth);
		ASSERT_NE(SETUP_FAILED, err) {
			TH_LOG("fail-nth %u: fork or fail-nth write failed", nth);
		}
		if (!err) {
			clean++;
			continue;
		}
		clean = 0;
		if (err < 0)
			continue;
		failed++;
		if (err == ENOEXEC || err == ELIBBAD) {
			wrong++;
			TH_LOG("fail-nth %u: execve(): %s", nth, strerror(err));
		}
	}
	ASSERT_EQ(50, clean) {
		TH_LOG("no clean exec after fail-nth %u", nth);
	}
	if (!failed)
		SKIP(return, "no execve() failed: failslab did not inject");
	EXPECT_EQ(0, wrong);
}

TEST_HARNESS_MAIN
