// SPDX-License-Identifier: GPL-2.0-only
/*
 * The argument block should land at a random offset within its page
 * exactly when the ELF loader randomizes the stack: always, unless
 * ADDR_NO_RANDOMIZE is set and the exec is not set-id. Each case
 * execs argblock-report repeatedly and compares both.
 *
 * Runs unprivileged: the set-user-ID copy is owned by the caller and
 * kept in a private directory.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "binfmt_misc_common.h"	/* artifact_path(), copy_file() */
#include "kselftest_harness.h"

/*
 * A page holds at least 4088 possible offsets, so 64 samples all landing
 * on one value happen by chance with probability at most (1/4088)^63.
 */
#define SAMPLES 64

/* How the helper is exec'd. */
#define NO_RAND	0x1	/* under ADDR_NO_RANDOMIZE */
#define NNP	0x2	/* under no_new_privs */
#define USERNS	0x4	/* in a new user namespace with no mappings */

FIXTURE(argblock) {
	char report[PATH_MAX];	/* the helper */
	char dir[32];		/* private directory for the set-id copy */
	char setid[PATH_MAX];	/* set-user-ID copy of the helper */
	bool have_dir, have_setid, nosuid;
	int randomize_va_space;
};

static int read_randomize_va_space(void)
{
	FILE *f = fopen("/proc/sys/kernel/randomize_va_space", "r");
	int v = -1;

	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

FIXTURE_SETUP(argblock)
{
	struct statvfs sv;

	ASSERT_EQ(artifact_path(self->report, sizeof(self->report),
				"argblock-report"), 0);
	self->randomize_va_space = read_randomize_va_space();

	/* A private directory keeps the set-user-ID copy out of reach. */
	strcpy(self->dir, "argblock-offset.XXXXXX");
	self->have_dir = mkdtemp(self->dir);
	if (!self->have_dir)
		return;
	snprintf(self->setid, sizeof(self->setid), "%s/setid", self->dir);
	self->have_setid = !copy_file(self->report, self->setid) &&
			   !chmod(self->setid, 04755);
	self->nosuid = !statvfs(self->dir, &sv) && (sv.f_flag & ST_NOSUID);
}

FIXTURE_TEARDOWN(argblock)
{
	if (!self->have_dir)
		return;
	unlink(self->setid);
	rmdir(self->dir);
}

struct sample {
	unsigned long offset;		/* argv[0] within its page */
	unsigned long stack_top;	/* end of [stack], chosen by the loader */
};

static int sample(const char *path, int how, struct sample *s)
{
	int pfd[2], status;
	char buf[64];
	ssize_t n;
	pid_t pid;

	if (pipe(pfd))
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		char *argv[] = { (char *)path, NULL };
		char *envp[] = { NULL };

		if ((how & NO_RAND) && personality(ADDR_NO_RANDOMIZE) < 0)
			_exit(2);
		if ((how & NNP) && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
			_exit(2);
		if ((how & USERNS) && unshare(CLONE_NEWUSER))
			_exit(2);
		dup2(pfd[1], STDOUT_FILENO);
		execve(path, argv, envp);
		_exit(3);
	}
	close(pfd[1]);
	n = read(pfd[0], buf, sizeof(buf) - 1);
	close(pfd[0]);
	waitpid(pid, &status, 0);
	if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status))
		return -1;
	buf[n] = '\0';
	return sscanf(buf, "%lx %lx", &s->offset, &s->stack_top) == 2 ? 0 : -1;
}

/*
 * Exec @path SAMPLES times. The in-page offset must vary as @expect says,
 * and vary exactly when the loader's stack top does.
 */
static void check(struct __test_metadata *_metadata, const char *path,
		  int how, bool expect)
{
	bool offset_varies = false, stack_varies = false;
	struct sample first, s;
	int i;

	for (i = 0; i < SAMPLES; i++) {
		ASSERT_EQ(sample(path, how, &s), 0)
			TH_LOG("exec of %s failed", path);
		if (!i)
			first = s;
		offset_varies |= s.offset != first.offset;
		stack_varies |= s.stack_top != first.stack_top;
	}
	EXPECT_EQ(stack_varies, expect)
		TH_LOG("the loader %s the stack",
		       stack_varies ? "randomized" : "did not randomize");
	EXPECT_EQ(offset_varies, expect)
		TH_LOG("the argument block offset is %s",
		       offset_varies ? "random" : "fixed");
}

/* Can this process create a user namespace? */
static bool have_userns(void)
{
	int status;
	pid_t pid = fork();

	if (pid == 0)
		_exit(unshare(CLONE_NEWUSER) ? 1 : 0);
	return pid > 0 && waitpid(pid, &status, 0) == pid &&
	       WIFEXITED(status) && !WEXITSTATUS(status);
}

/* Without privilege, ADDR_NO_RANDOMIZE keeps the offset fixed. */
TEST_F(argblock, addr_no_randomize_keeps_offset_fixed)
{
	check(_metadata, self->report, NO_RAND, false);
}

/* By default the offset is random. */
TEST_F(argblock, offset_is_random)
{
	if (self->randomize_va_space <= 0)
		SKIP(return, "randomize_va_space is 0");
	check(_metadata, self->report, 0, true);
}

/* A set-id exec clears ADDR_NO_RANDOMIZE, so the offset is random again. */
TEST_F(argblock, setid_ignores_addr_no_randomize)
{
	if (self->randomize_va_space <= 0)
		SKIP(return, "randomize_va_space is 0");
	if (!self->have_setid)
		SKIP(return, "cannot create a set-user-ID copy");
	if (self->nosuid)
		SKIP(return, "set-user-ID copy is on a nosuid mount");
	check(_metadata, self->setid, NO_RAND, true);
}

/* On a nosuid mount the exec gains nothing, so the flag holds. */
TEST_F(argblock, setid_on_nosuid_keeps_offset_fixed)
{
	if (!self->have_setid)
		SKIP(return, "cannot create a set-user-ID copy");
	if (!self->nosuid)
		SKIP(return, "set-user-ID copy is not on a nosuid mount");
	check(_metadata, self->setid, NO_RAND, false);
}

/* no_new_privs makes the exec ignore the set-id bit. */
TEST_F(argblock, setid_under_no_new_privs_keeps_offset_fixed)
{
	if (!self->have_setid)
		SKIP(return, "cannot create a set-user-ID copy");
	check(_metadata, self->setid, NO_RAND | NNP, false);
}

/* The copy's owner has no mapping there, so the set-id bit is ignored. */
TEST_F(argblock, setid_in_user_namespace_keeps_offset_fixed)
{
	if (!self->have_setid)
		SKIP(return, "cannot create a set-user-ID copy");
	if (!have_userns())
		SKIP(return, "cannot create a user namespace");
	check(_metadata, self->setid, NO_RAND | USERNS, false);
}

TEST_HARNESS_MAIN
