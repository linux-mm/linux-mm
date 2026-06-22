// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Google LLC.
 *
 * Author: Tarun Sahu <tarunsahu@google.com>
 *
 * Test for VM and guest_memfd preservation across kexec (Live Update) via LUO.
 *
 * NOTE: This is a MANUAL test and is excluded from automated CI/testing
 * frameworks because Stage 1 daemonizes into the background to pin resources
 * and requires a human operator to manually trigger kexec before Stage 2
 * is executed. Running Stage 1 automatically would leak the background daemon
 * and cause CI runners to falsely interpret it as a passed test.
 *
 * Usage:
 * Stage 1: ./guest_memfd_preservation_test --stage 1
 * Stage 2: ./guest_memfd_preservation_test --stage 2
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/sizes.h>
#include <linux/falloc.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "ucall_common.h"
#include "../kselftest.h"
#include "../kselftest_harness.h"

#include <libliveupdate.h>

#define SESSION_NAME "gmem_vm_preservation_session"
#define VM_TOKEN 0x1001
#define GMEM_TOKEN 0x1002

#define STATE_SESSION_NAME "gmem_preservation_state"
#define STATE_TOKEN 0x999

#define GMEM_SIZE (16ULL * 1024 * 1024)
#define DATA_SIZE (5ULL * 1024 * 1024)

static size_t page_size;

/* Deterministic byte pattern generation based on offset */
static inline uint8_t get_pattern_byte(size_t offset)
{
	return (uint8_t)(offset ^ 0x5A);
}

static void guest_code_phase1(uint64_t gpa, uint64_t size, uint64_t data_size)
{
	uint8_t *mem = (uint8_t *)gpa;
	size_t i;

	for (i = 0; i < data_size; i++)
		mem[i] = get_pattern_byte(i);

	GUEST_DONE();
}

static void guest_code_phase2(uint64_t gpa, uint64_t size, uint64_t data_size)
{
	uint8_t *mem = (uint8_t *)gpa;
	size_t i;

	for (i = 0; i < data_size; i++) {
		uint8_t val = get_pattern_byte(i);

		__GUEST_ASSERT(mem[i] == val,
			       "Data mismatch at offset %lu! Expected 0x%x, got 0x%x",
			       i, val, mem[i]);
	}

	GUEST_DONE();
}

static void run_stage_1(int luo_fd)
{
	uint64_t flags = GUEST_MEMFD_FLAG_MMAP | GUEST_MEMFD_FLAG_INIT_SHARED;
	int gmem_fd, session_fd, ret;
	const uint64_t gpa = SZ_4G;
	struct kvm_vcpu *vcpu;
	const int slot = 1;
	struct kvm_vm *vm;

	ksft_print_msg("[STAGE 1] Starting pre-kexec setup...\n");

	ksft_print_msg("[STAGE 1] Creating state file for next stage (2)...\n");
	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_TOKEN, 2);

	vm = __vm_create_shape_with_one_vcpu(VM_SHAPE_DEFAULT, &vcpu, 1,
					guest_code_phase1);
	gmem_fd = vm_create_guest_memfd(vm, GMEM_SIZE, flags);
	vm_set_user_memory_region2(vm, slot, KVM_MEM_GUEST_MEMFD, gpa, GMEM_SIZE, NULL,
				 gmem_fd, 0);

	for (size_t i = 0; i < GMEM_SIZE; i += page_size)
		virt_pg_map(vm, gpa + i, gpa + i);

	vcpu_args_set(vcpu, 3, gpa, GMEM_SIZE, DATA_SIZE);

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, NULL), UCALL_DONE);

	ksft_print_msg("[STAGE 1] Creating session '%s' and preserving VM/guest_memfd...\n",
		       SESSION_NAME);
	session_fd = luo_create_session(luo_fd, SESSION_NAME);
	TEST_ASSERT(session_fd >= 0, "Failed to create LUO session");

	ret = luo_session_preserve_fd(session_fd, vm->fd, VM_TOKEN);
	TEST_ASSERT(ret == 0, "Failed to preserve VM file descriptor");

	ret = luo_session_preserve_fd(session_fd, gmem_fd, GMEM_TOKEN);
	TEST_ASSERT(ret == 0, "Failed to preserve guest_memfd file descriptor");

	printf("\n============================================================\n");
	printf("Phase 1 Complete Successfully!\n");
	printf("VM file and guest_memfd file have been preserved via LUO.\n");
	printf("Tokens: VM_TOKEN=0x%x, GMEM_TOKEN=0x%x\n", VM_TOKEN, GMEM_TOKEN);
	printf("Machine Size: %llu MB, Data Size: %llu MB\n", GMEM_SIZE / SZ_1M,
				 DATA_SIZE / SZ_1M);
	printf("------------------------------------------------------------\n");

	close(luo_fd);
	daemonize_and_wait();
}

static struct kvm_vm *vm_create_from_fd(int resurrected_vm_fd,
					struct vm_shape shape)
{
	struct kvm_vm *vm;

	vm = calloc(1, sizeof(*vm));
	TEST_ASSERT(vm != NULL, "Insufficient Memory");

	vm_init_fields(vm, shape);

	vm->kvm_fd = open_path_or_exit(KVM_DEV_PATH, O_RDWR);
	vm->fd = resurrected_vm_fd;

	if (kvm_has_cap(KVM_CAP_BINARY_STATS_FD))
		vm->stats.fd = vm_get_stats_fd(vm);
	else
		vm->stats.fd = -1;

	vm_init_memory_properties(vm);

	return vm;
}

static void run_stage_2(int luo_fd, int state_session_fd)
{
	int retrieved_vm_fd, retrieved_gmem_fd, session_fd, stage;
	struct vm_shape shape = VM_SHAPE_DEFAULT;
	const uint64_t gpa = SZ_4G;
	struct kvm_vcpu *vcpu;
	const int slot = 1;
	struct kvm_vm *vm;

	ksft_print_msg("[STAGE 2] Starting post-kexec verification...\n");

	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	if (stage != 2)
		fail_exit("Expected stage 2, but state file contains %d", stage);

	ksft_print_msg("[STAGE 2] Retrieving session '%s'...\n", SESSION_NAME);
	session_fd = luo_retrieve_session(luo_fd, SESSION_NAME);
	TEST_ASSERT(session_fd >= 0, "Failed to retrieve LUO session");

	retrieved_vm_fd = luo_session_retrieve_fd(session_fd, VM_TOKEN);
	TEST_ASSERT(retrieved_vm_fd >= 0, "Failed to retrieve VM file descriptor");

	retrieved_gmem_fd = luo_session_retrieve_fd(session_fd, GMEM_TOKEN);
	TEST_ASSERT(retrieved_gmem_fd >= 0, "Failed to retrieve guest_memfd file descriptor");

	vm = vm_create_from_fd(retrieved_vm_fd, shape);

	u64 nr_pages = 2048; /* 8MB is plenty for slot0 pages */

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, 0, 0, nr_pages, 0);
	kvm_vm_elf_load(vm, program_invocation_name);

	for (int i = 0; i < NR_MEM_REGIONS; i++)
		vm->memslots[i] = 0;

	struct userspace_mem_region *slot0 = memslot2region(vm, 0);

	ucall_init(vm, slot0->region.guest_phys_addr + slot0->region.memory_size);

	vm_set_user_memory_region2(vm, slot, KVM_MEM_GUEST_MEMFD, gpa, GMEM_SIZE, NULL,
				   retrieved_gmem_fd, 0);

	for (size_t i = 0; i < GMEM_SIZE; i += page_size)
		virt_pg_map(vm, gpa + i, gpa + i);

	vcpu = vm_vcpu_add(vm, 0, guest_code_phase2);
	kvm_arch_vm_finalize_vcpus(vm);

	vcpu_args_set(vcpu, 3, gpa, GMEM_SIZE, DATA_SIZE);

	printf("Resuming / Running VM in Phase 2...\n");
	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, NULL), UCALL_DONE);

	printf("\nSUCCESS: Phase 2 Complete! All 5MB complex data verified intact!\n");

	luo_session_finish(session_fd);
	close(session_fd);

	ksft_print_msg("[STAGE 2] Finalizing state session...\n");
	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	/* This will also close the vm_fd */
	kvm_vm_free(vm);
	close(retrieved_gmem_fd);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_GUEST_MEMFD));
	page_size = getpagesize();

	return luo_test(argc, argv, STATE_SESSION_NAME,
			run_stage_1, run_stage_2);
}
