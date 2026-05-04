// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit Userspace testing API.
 *
 * Copyright (C) 2026, Linutronix GmbH.
 * Author: Thomas Weißschuh <thomas.weissschuh@linutronix.de>
 */

#include <linux/binfmts.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/pid.h>
#include <linux/pipe_fs_i.h>
#include <linux/sched/task.h>
#include <linux/seq_buf.h>
#include <linux/types.h>
#include <linux/umh.h>

#include <kunit/test-bug.h>
#include <kunit/test.h>
#include <kunit/uapi.h>

enum {
	KSFT_PASS	= 0,
	KSFT_FAIL	= 1,
	KSFT_XFAIL	= 2,
	KSFT_XPASS	= 3,
	KSFT_SKIP	= 4,
};

static struct vfsmount *kunit_uapi_mount_fs(const char *name)
{
	struct file_system_type *type;

	type = get_fs_type(name);
	if (!type)
		return ERR_PTR(-ENODEV);

	return kern_mount(type);
}

static int kunit_uapi_write_file(struct vfsmount *mnt, const char *name, mode_t mode,
				 const u8 *data, size_t size)
{
	struct file *file;
	ssize_t written;

	file = file_open_root_mnt(mnt, name, O_CREAT | O_WRONLY, mode);
	if (IS_ERR(file))
		return PTR_ERR(file);

	written = kernel_write(file, data, size, NULL);
	filp_close(file, NULL);
	if (written != size) {
		if (written >= 0)
			return -ENOMEM;
		return written;
	}

	return 0;
}

static const char *kunit_uapi_executable_target(const struct kunit_uapi_blob *executable)
{
	return kbasename(executable->path);
}

static int kunit_uapi_write_executable(struct vfsmount *mnt,
				       const struct kunit_uapi_blob *executable)
{
	return kunit_uapi_write_file(mnt, kunit_uapi_executable_target(executable), 0755,
				     executable->data, executable->end - executable->data);
}

struct kunit_uapi_usermodehelper_ctx {
	struct vfsmount *mnt;
	struct kunit *test;
};

static int kunit_uapi_get_cwd(struct vfsmount *mnt)
{
	CLASS(get_unused_fd, fd)(O_RDONLY);
	if (fd < 0)
		return fd;

	struct file *file __free(fput) = file_open_root_mnt(mnt, "/", O_DIRECTORY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	fd_install(fd, no_free_ptr(file));

	return take_fd(fd);
}

static int kunit_uapi_usermodehelper_init(struct subprocess_info *info, struct cred *new)
{
	struct kunit_uapi_usermodehelper_ctx *ctx = info->data;
	int dirfd;

	dirfd = kunit_uapi_get_cwd(ctx->mnt);
	if (dirfd < 0)
		return dirfd;

	kernel_sigaction(SIGKILL, SIG_DFL);
	kernel_sigaction(SIGABRT, SIG_DFL);

	current->kunit_test = ctx->test;

	info->dirfd = dirfd;

	return 0;
}

static int kunit_uapi_run_executable_in_mount(struct kunit *test,
					      const struct kunit_uapi_blob *executable,
					      struct vfsmount *mnt)
{
	const char *executable_target = kunit_uapi_executable_target(executable);
	struct kunit_uapi_usermodehelper_ctx ctx = {
		.test	= test,
		.mnt	= mnt,
	};
	struct subprocess_info *info;
	const char *const argv[] = {
		executable_target,
		NULL
	};

	info = call_usermodehelper_setup(AT_FDCWD, executable_target, (char **)argv, NULL,
					 GFP_KERNEL, kunit_uapi_usermodehelper_init, NULL, &ctx);
	if (!info)
		return -ENOMEM;

	/* Flush delayed fput so exec can open the file read-only */
	flush_delayed_fput();

	return call_usermodehelper_exec(info, UMH_WAIT_PROC);
}

static int kunit_uapi_run_executable(struct kunit *test, const struct kunit_uapi_blob *executable)
{
	int err;

	struct vfsmount *mnt __free(kern_unmount) = kunit_uapi_mount_fs("ramfs");
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	err = kunit_uapi_write_executable(mnt, executable);
	if (err)
		return err;

	err = kunit_uapi_run_executable_in_mount(test, executable, mnt);
	if (err)
		return err;

	return 0;
}

void kunit_uapi_run_kselftest(struct kunit *test, const struct kunit_uapi_blob *executable)
{
	u8 exit_code, exit_signal;
	int err;

	err = kunit_uapi_run_executable(test, executable);
	if (err < 0)
		KUNIT_FAIL_AND_ABORT(test, "Could not run test executable: %pe\n", ERR_PTR(err));

	exit_code = err >> 8;
	exit_signal = err & 0xff;

	if (exit_signal)
		KUNIT_FAIL_AND_ABORT(test, "kselftest exited with signal: %d\n", exit_signal);
	else if (exit_code == KSFT_PASS)
		; /* Noop */
	else if (exit_code == KSFT_FAIL)
		KUNIT_FAIL_AND_ABORT(test, "kselftest exited with code KSFT_FAIL\n");
	else if (exit_code == KSFT_XPASS)
		KUNIT_FAIL_AND_ABORT(test, "kselftest exited with code KSFT_XPASS\n");
	else if (exit_code == KSFT_XFAIL)
		; /* Noop */
	else if (exit_code == KSFT_SKIP)
		kunit_mark_skipped(test, "kselftest exited with code KSFT_SKIP\n");
	else
		KUNIT_FAIL_AND_ABORT(test, "kselftest exited with unknown exit code: %d\n",
				     exit_code);
}
EXPORT_SYMBOL_GPL(kunit_uapi_run_kselftest);

MODULE_DESCRIPTION("KUnit UAPI testing framework");
MODULE_AUTHOR("Thomas Weißschuh <thomas.weissschuh@linutronix.de");
MODULE_LICENSE("GPL");
