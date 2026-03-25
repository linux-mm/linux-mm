#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/common.sh"
failures_log="$tmp_dir/$test_script-failures.log"
exitcode=0

declare -A TESTS
TESTS["ksft_compaction.sh"]="compaction"
TESTS["ksft_madv_guard.sh"]="madvise(MADV_GUARD)"
TESTS["ksft_madv_populate.sh"]="madvise(MADV_POPULATE)"
TESTS["ksft_mdwe.sh"]="prctl(PR_SET_MDWE)"
TESTS["ksft_mkdirty.sh"]="PTE/PMD mkdirty"
TESTS["ksft_pfnmap.sh"]="VM_PFNMAP"
TESTS["ksft_process_madv.sh"]="process_madvise(2)"
TESTS["ksft_process_mrelease.sh"]="process_mrelease(2)"
TESTS["ksft_rmap.sh"]="rmap"
TESTS["ksft_vma_merge.sh"]="VMA merge"
TESTS["ksft_mmap.sh"]="mmap(2)"
TESTS["ksft_mremap.sh"]="mremap(2)"
TESTS["ksft_page_frag.sh"]="page fragment allocator"
# ksm_numa is flaky, disable it for now
# TESTS["ksft_ksm_numa.sh"]="KSM NUMA"
TESTS["ksft_soft_dirty.sh"]="soft-dirty"
TESTS["ksft_cow.sh"]="CoW"
TESTS["ksft_pagemap.sh"]="pagemap_ioctl()"
TESTS["ksft_migration.sh"]="migration"
TESTS["ksft_mlock.sh"]="mlock(2)"
TESTS["ksft_hugevm.sh"]="very large virtual address space"
TESTS["ksft_pkey.sh"]="protection keyes"
TESTS["ksft_gup_test.sh"]="GUP"
TESTS["ksft_memfd_secret.sh"]="memfd_secret(2)"
TESTS["ksft_ksm.sh"]="KSM"
TESTS["ksft_memory_failure.sh"]="memory failure"
TESTS["ksft_thp.sh"]="THP"
TESTS["ksft_userfaultfd.sh"]="userfaultfd(2)"
TESTS["ksft_hugetlb.sh"]="hugetlb"
TESTS["ksft_hmm.sh"]="HMM"
TESTS["ksft_vmalloc.sh"]="vmalloc"

tests=($(printf '%s\n' "${!TESTS[@]}" | sort))
test_names=($(printf '%s\n' "${TESTS[@]}" | sort))

cd $linux_dir

guest_dir=$(mktemp -d -p $(pwd))
guest_ext4_img=$guest_dir/ext4.img
guest_ext4_mnt=$guest_dir/ext4_mnt
guest_swap=$guest_dir/swap.qcow2

function vm_test_cleanup() {
	sudo umount -f $guest_ext4_mnt &> /dev/null || true
	sudo rm -fr $guest_dir
	cleanup
}
trap vm_test_cleanup EXIT

function prepare_guest_env() {
	# a lot of tests require swap, they are skipped otherwise
	qemu-img create -f qcow2 $guest_swap 2G

	# tests that use local tmpfiles and won't happy with virtiofs and
	# plan9, create an ext4 filesystem for them and populate this
	# filesystem with a copy of mm selftests

	truncate -s 2G $guest_ext4_img
	/usr/sbin/mkfs.ext4 $guest_ext4_img

	mkdir $guest_ext4_mnt
	sudo mount -o loop $guest_ext4_img $guest_ext4_mnt
	sudo mkdir $guest_ext4_mnt/mm-selftests
	sudo chown $USER $guest_ext4_mnt/mm-selftests
	cp -a tools/testing/selftests/* $guest_ext4_mnt/mm-selftests/

	sudo umount $guest_ext4_mnt
}

function prepare_guest_script() {
	local ksft_script=$1
	local ext4_img=${guest_ext4_img/$guest_dir/\/mnt}
	local ext4_mnt=${guest_ext4_mnt/$guest_dir/\/mnt}

	cat > $guest_dir/run_vmtests.sh <<EOF
#!/bin/bash
set -euo pipefail

sudo mkswap /dev/vda &>/dev/null
sudo swapon /dev/vda &>/dev/null

# run_vmtests.sh allocates 2M hugepages, but it does not allocate 1G pages
# force 4 1G hugepages here
# FIXME: update for different non-default hugepage sizes
echo 4 | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages

# disable address space randomization
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

mount -o loop $ext4_img $ext4_mnt
mount -t tmpfs tmpfs /tmp

cd $ext4_mnt/mm-selftests/mm/
sudo ./$ksft_script -n 2>&1
EOF
}

TEST_SCRIPT="/mnt/run_vmtests.sh"
QEMU_OPTS="-drive file=$guest_swap,if=virtio"
KERNEL_OPTS="hugepagesz=2M hugepagesz=1G"

function run_test() {
    local test=$1
    local test_name=$2
    local err=0

    echo "Running $test_name test"
    prepare_guest_script "$test" &> /dev/null

    vng --cpus 4 --memory 16G --numa 8G --numa 8G --rwdir=/mnt=$guest_dir \
        --qemu-opts="$QEMU_OPTS" -- bash $TEST_SCRIPT &> $log || err=$?
    if  [ $err -eq 0 ] || [ $err -eq $ksft_skip ]; then
            grep Totals $log || true
            grep SUMMARY $log || true
            echo "✓ $test_name tests passed"
            return 0
    fi

    exitcode=1

    # For TAP test log failures, for non-TAP test dump the raw log
    echo "$test_name test failed:" >>$failures_log
    # Record failures tests if they are in TAP format
    if ! grep "^# not ok " $log 2>/dev/null >>$failures_log; then
	    cat $log >>$failures_log
    fi
    echo "------------------------------------------" >>$failures_log
    echo "✗ $test_name tests failed"
}

echo "Building kernel with MM selftests configuration..."
KCONFIG_FRAGMENT=".github/kconfigs/mm-selftests.config"
vng -v --build --config "$KCONFIG_FRAGMENT" > $log 2>&1 || \
	fail "Kernel build failed"

echo "Building MM selftests..."
make -C tools/testing/selftests/mm > $log 2>&1 || fail "Selftests build failed"

echo "Preparing guest test environment"
prepare_guest_env &> /dev/null

echo "Running MM selftests"

for test in "${tests[@]}"; do
    test_name="${TESTS[$test]}"
    run_test "$test" "$test_name"
done

# Display results summary
echo "=========================================="
if [ $exitcode -eq 0 ]; then
    echo "✓ All tests passed"
else
    cat $failures_log
fi

exit $exitcode
