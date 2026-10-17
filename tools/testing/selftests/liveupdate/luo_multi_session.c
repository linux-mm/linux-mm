// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * A selftest to validate the end-to-end lifecycle of multiple LUO sessions
 * across a kexec reboot, including empty sessions and sessions with multiple
 * files.
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libliveupdate.h>
#include <linux/liveupdate.h>

#define SESSION_EMPTY_1 "multi-test-empty-1"
#define SESSION_EMPTY_2 "multi-test-empty-2"
#define SESSION_FILES_1 "multi-test-files-1"
#define SESSION_FILES_2 "multi-test-files-2"

#define MFD1_TOKEN 0x1001
#define MFD2_TOKEN 0x2002
#define MFD3_TOKEN 0x3003

#define MFD1_DATA "Data for session files 1"
#define MFD2_DATA "First file for session files 2"
#define MFD3_DATA "Second file for session files 2"
#define MFD3_POS 42

#define STATE_SESSION_NAME "kexec_multi_state"
#define STATE_MEMFD_TOKEN 998

/* Stage 1: Executed before the kexec reboot. */
static void run_stage_1(int luo_fd)
{
	int s_empty1_fd, s_empty2_fd, s_files1_fd, s_files2_fd;
	int tmpfs_fd;
	void *map;

	ksft_print_msg("[STAGE 1] Starting pre-kexec setup for multi-session test...\n");

	ksft_print_msg("[STAGE 1] Creating state file for next stage (2)...\n");
	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_MEMFD_TOKEN, 2);

	ksft_print_msg("[STAGE 1] Creating empty sessions '%s' and '%s'...\n",
		       SESSION_EMPTY_1, SESSION_EMPTY_2);
	s_empty1_fd = luo_create_session(luo_fd, SESSION_EMPTY_1);
	if (s_empty1_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_EMPTY_1);

	s_empty2_fd = luo_create_session(luo_fd, SESSION_EMPTY_2);
	if (s_empty2_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_EMPTY_2);

	ksft_print_msg("[STAGE 1] Creating session '%s' with one memfd...\n",
		       SESSION_FILES_1);

	s_files1_fd = luo_create_session(luo_fd, SESSION_FILES_1);
	if (s_files1_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_FILES_1);
	ksft_print_msg("  -> Preserving memfd (token %#x, content: \"%s\")\n",
		       MFD1_TOKEN, MFD1_DATA);
	if (create_and_preserve_memfd(s_files1_fd, MFD1_TOKEN, MFD1_DATA) < 0) {
		fail_exit("create_and_preserve_memfd for token %#x",
			  MFD1_TOKEN);
	}

	ksft_print_msg("[STAGE 1] Creating session '%s' with memfd and tmpfs file...\n",
		       SESSION_FILES_2);

	s_files2_fd = luo_create_session(luo_fd, SESSION_FILES_2);
	if (s_files2_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_FILES_2);
	ksft_print_msg("  -> Preserving memfd (token %#x, content: \"%s\")\n",
		       MFD2_TOKEN, MFD2_DATA);
	if (create_and_preserve_memfd(s_files2_fd, MFD2_TOKEN, MFD2_DATA) < 0) {
		fail_exit("create_and_preserve_memfd for token %#x",
			  MFD2_TOKEN);
	}

	ksft_print_msg("  -> Populating tmpfs file at /mnt/mfd3...\n");
	if (mkdir("/mnt", 0777) < 0 && errno != EEXIST)
		fail_exit("mkdir /mnt");
	if (mount("tmpfs", "/mnt", "tmpfs", 0, NULL) < 0)
		fail_exit("mount tmpfs");
	tmpfs_fd = open("/mnt/mfd3", O_CREAT | O_RDWR, 0666);
	if (tmpfs_fd < 0)
		fail_exit("failed to open tmpfs_fd");
	ksft_print_msg("  -> Preserving generic tmpfs file (token %#x, content: \"%s\")\n",
		       MFD3_TOKEN, MFD3_DATA);
	if (ftruncate(tmpfs_fd, getpagesize()) < 0)
		fail_exit("ftruncate tmpfs_fd");
	map = mmap(NULL, getpagesize(), PROT_WRITE, MAP_SHARED, tmpfs_fd, 0);
	if (map == MAP_FAILED)
		fail_exit("mmap tmpfs_fd");
	strcpy(map, MFD3_DATA);
	munmap(map, getpagesize());
	if (lseek(tmpfs_fd, MFD3_POS, SEEK_SET) != MFD3_POS)
		fail_exit("lseek tmpfs_fd");

	if (luo_session_preserve_fd(s_files2_fd, tmpfs_fd, MFD3_TOKEN) < 0)
		fail_exit("preserve tmpfs fd");

	close(tmpfs_fd);

	close(luo_fd);
	daemonize_and_wait();
}

/* Stage 2: Executed after the kexec reboot. */
static void run_stage_2(int luo_fd, int state_session_fd)
{
	int s_empty1_fd, s_empty2_fd, s_files1_fd, s_files2_fd;
	struct liveupdate_memfd_retrieve_arg rarg = {
		.size = sizeof(rarg),
	};
	int mfd1, mfd2, mfd3, ro_fd, ret, stage;

	ksft_print_msg("[STAGE 2] Starting post-kexec verification...\n");

	restore_and_read_stage(state_session_fd, STATE_MEMFD_TOKEN, &stage);
	if (stage != 2) {
		fail_exit("Expected stage 2, but state file contains %d",
			  stage);
	}

	ksft_print_msg("[STAGE 2] Retrieving all sessions...\n");
	s_empty1_fd = luo_retrieve_session(luo_fd, SESSION_EMPTY_1);
	if (s_empty1_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_EMPTY_1);

	s_empty2_fd = luo_retrieve_session(luo_fd, SESSION_EMPTY_2);
	if (s_empty2_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_EMPTY_2);

	s_files1_fd = luo_retrieve_session(luo_fd, SESSION_FILES_1);
	if (s_files1_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_FILES_1);

	s_files2_fd = luo_retrieve_session(luo_fd, SESSION_FILES_2);
	if (s_files2_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_FILES_2);

	ksft_print_msg("[STAGE 2] Verifying contents of session '%s'...\n",
		       SESSION_FILES_1);
	ksft_print_msg("  -> Retrieving memfd (token %#x) from session...\n",
		       MFD1_TOKEN);
	mfd1 = restore_and_verify_memfd(s_files1_fd, MFD1_TOKEN, MFD1_DATA);
	if (mfd1 < 0)
		fail_exit("restore_and_verify_memfd for token %#x", MFD1_TOKEN);
	close(mfd1);

	ksft_print_msg("[STAGE 2] Verifying contents of session '%s'...\n",
		       SESSION_FILES_2);

	ksft_print_msg("  -> Creating empty tmpfs file at /mnt/mfd3...\n");
	if (mkdir("/mnt", 0777) < 0 && errno != EEXIST)
		fail_exit("mkdir /mnt");
	if (mount("tmpfs", "/mnt", "tmpfs", 0, NULL) < 0)
		fail_exit("mount tmpfs");
	mfd3 = open("/mnt/mfd3", O_CREAT | O_RDWR, 0666);
	if (mfd3 < 0)
		fail_exit("open tmpfs file");
	ro_fd = open("/mnt/mfd3", O_RDONLY);
	if (ro_fd < 0)
		fail_exit("open read-only tmpfs file");

	ksft_print_msg("  -> Verifying retryable retrieve errors for memfd and tmpfs...\n");
	rarg.fd = mfd3;
	ret = luo_session_retrieve_fd_arg(s_files2_fd, MFD2_TOKEN,
					  (uintptr_t)&rarg);
	if (ret != -EINVAL)
		fail_exit("expected -EINVAL when passing arg to memfd retrieve, got %d",
			  ret);

	ksft_print_msg("  -> Retrieving memfd (token %#x) from session...\n",
		       MFD2_TOKEN);
	mfd2 = restore_and_verify_memfd(s_files2_fd, MFD2_TOKEN, MFD2_DATA);
	if (mfd2 < 0)
		fail_exit("restore_and_verify_memfd for token %#x", MFD2_TOKEN);
	close(mfd2);

	ret = luo_session_retrieve_fd(s_files2_fd, MFD3_TOKEN);
	if (ret != -EINVAL)
		fail_exit("expected -EINVAL when retrieving tmpfs without arg, got %d",
			  ret);

	rarg.fd = ro_fd;
	ret = luo_session_retrieve_fd_arg(s_files2_fd, MFD3_TOKEN,
					  (uintptr_t)&rarg);
	if (ret != -EACCES)
		fail_exit("expected -EACCES when retrieving tmpfs into read-only fd, got %d",
			  ret);
	close(ro_fd);

	if (ftruncate(mfd3, 1) < 0)
		fail_exit("ftruncate non-empty tmpfs file");
	rarg.fd = mfd3;
	ret = luo_session_retrieve_fd_arg(s_files2_fd, MFD3_TOKEN,
					  (uintptr_t)&rarg);
	if (ret != -EBUSY)
		fail_exit("expected -EBUSY when retrieving tmpfs into non-empty fd, got %d",
			  ret);
	if (ftruncate(mfd3, 0) < 0)
		fail_exit("ftruncate empty tmpfs file");

	ksft_print_msg("  -> Retrieving tmpfs file (token %#x) from session...\n",
		       MFD3_TOKEN);
	if (restore_and_verify_tmpfs(s_files2_fd, MFD3_TOKEN, mfd3,
				     MFD3_DATA) < 0)
		fail_exit("restore_and_verify_tmpfs for token %#x",
			  MFD3_TOKEN);
	if (lseek(mfd3, 0, SEEK_CUR) != MFD3_POS)
		fail_exit("expected f_pos %d on restored tmpfs file", MFD3_POS);
	close(mfd3);

	ksft_print_msg("[STAGE 2] Test data verified successfully.\n");

	ksft_print_msg("[STAGE 2] Finalizing all test sessions...\n");
	if (luo_session_finish(s_empty1_fd) < 0)
		fail_exit("luo_session_finish for '%s'", SESSION_EMPTY_1);
	close(s_empty1_fd);

	if (luo_session_finish(s_empty2_fd) < 0)
		fail_exit("luo_session_finish for '%s'", SESSION_EMPTY_2);
	close(s_empty2_fd);

	if (luo_session_finish(s_files1_fd) < 0)
		fail_exit("luo_session_finish for '%s'", SESSION_FILES_1);
	close(s_files1_fd);

	if (luo_session_finish(s_files2_fd) < 0)
		fail_exit("luo_session_finish for '%s'", SESSION_FILES_2);
	close(s_files2_fd);

	ksft_print_msg("[STAGE 2] Finalizing state session...\n");
	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	ksft_print_msg("\n--- MULTI-SESSION KEXEC TEST PASSED ---\n");
}

int main(int argc, char *argv[])
{
	return luo_test(argc, argv, STATE_SESSION_NAME,
			run_stage_1, run_stage_2);
}
