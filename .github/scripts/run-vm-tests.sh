#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/common.sh"

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
	cp -a tools/testing/selftests/mm/* $guest_ext4_mnt/mm-selftests

	sudo umount $guest_ext4_mnt
}

function prepare_guest_script() {
	local ext4_img=${guest_ext4_img/$guest_dir/\/mnt}
	local ext4_mnt=${guest_ext4_mnt/$guest_dir/\/mnt}

	cat > $guest_dir/run_vmtests.sh <<EOF
#!/bin/bash
set -euo pipefail
set -x

sudo mkswap /dev/vda
sudo swapon /dev/vda

# run_vmtests.sh allocates 2M hugepages, but it does not allocate 1G pages
# force 4 1G hugepages here
# FIXME: update for different non-default hugepage sizes
echo 4 | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages

mount -o loop $ext4_img $ext4_mnt
mount -t tmpfs tmpfs /tmp

cd $ext4_mnt/mm-selftests
sudo ./run_vmtests.sh 2>&1
EOF
}

echo "Building kernel with MM selftests configuration..."
KCONFIG_FRAGMENT=".github/kconfigs/mm-selftests.config"
vng -v --build --config "$KCONFIG_FRAGMENT" > $log 2>&1 || \
	fail "Kernel build failed"

echo "Building MM selftests..."
make -C tools/testing/selftests/mm > $log 2>&1 || fail "Selftests build failed"

echo "Preparing guest test environment"
prepare_guest_env &> /dev/null

echo "Preparing guest test script"
prepare_guest_script

TEST_SCRIPT="/mnt/run_vmtests.sh"
QEMU_OPTS="-drive file=$guest_swap,if=virtio"
KERNEL_OPTS="hugepagesz=2M hugepagesz=1G"

echo "Running MM selftests"
vng --cpus 4 --memory 16G --numa 8G --numa 8G --rwdir=/mnt=$guest_dir \
    --qemu-opts="$QEMU_OPTS" -- bash $TEST_SCRIPT &> $log || true

# Parse and display results
echo "=========================================="
echo "Raw results:"
echo "=========================================="

# Display the full output
cat $log

# Check for failed tests and exit with error if there are any
failed=$(grep -c "^not ok " $log 2>/dev/null || true)
failed=${failed:-0}

if [ "$failed" -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    grep "^not ok " $log || true
    exit 1
fi
