#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root"
    exit 1
fi

MNT_PATH="/tmp/mnt_hugetlb_shared_leak"
FILE_PATH="$MNT_PATH/test_file"

# Save original values
orig_nr=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)

cleanup() {
    echo "Cleaning up..."
    rm -f "$FILE_PATH"
    umount "$MNT_PATH" 2>/dev/null
    rmdir "$MNT_PATH" 2>/dev/null
    echo "$orig_nr" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    echo "Cleanup done."
}
trap cleanup EXIT

# 1. Set nr_hugepages to 2
echo 2 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 2. Mount hugetlbfs with min_size=2M (1 page)
mkdir -p "$MNT_PATH"
if ! mount -t hugetlbfs -o min_size=2M none "$MNT_PATH"; then
    echo "Failed to mount hugetlbfs"
    exit 1
fi

# Check resv_hugepages after mount (should be 1)
initial_resv=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/resv_hugepages)
echo "Initial resv_hugepages (after mount): $initial_resv"
if [ "$initial_resv" -ne 1 ]; then
    echo "ERROR: Initial resv_hugepages is not 1!"
    exit 1
fi

# Verify reproducer binary exists
if [ ! -x ./subpool_shared_leak ]; then
    echo "reproducer binary './subpool_shared_leak' not found or not executable."
    echo "Please compile it first: gcc -static -o subpool_shared_leak subpool_shared_leak.c"
    exit 1
fi

# 3. Run helper to map 4MB, allocate 2MB, and close.
# This creates 2 reservations, consumes 1 (by allocating Page 0).
# The unallocated Page 1 reservation remains active in the inode's resv_map.
echo "Running helper..."
./subpool_shared_leak "$FILE_PATH"

resv_after_helper=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/resv_hugepages)
echo "resv_hugepages after helper (should be 1): $resv_after_helper"
# Page 0 is allocated (no longer reserved). Page 1 is reserved.
# So resv_hugepages should be 1.
if [ "$resv_after_helper" -ne 1 ]; then
    echo "ERROR: resv_hugepages is not 1 after helper run!"
    exit 1
fi

# 4. Truncate file to 2MB (releases Page 1 reservation)
echo "Truncating file to 2MB (releasing 1 page reservation)..."
truncate -s 2M "$FILE_PATH"

# Check resv_hugepages after truncate.
# Since Page 0 is still allocated (and in page cache), and satisfies the
# min_size=2M guarantee, we should have 0 reservations remaining.
# If the bug is present, the truncate path will incorrectly restore the
# reservation to the subpool and skip releasing it globally, leaving
# resv_hugepages at 1.
final_resv=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/resv_hugepages)
echo "Final resv_hugepages (after 2MB truncate): $final_resv"

if [ "$final_resv" -eq 1 ]; then
    echo "RESULT: LEAK DETECTED (FAIL)"
    exit 1
elif [ "$final_resv" -eq 0 ]; then
    echo "RESULT: NO LEAK (PASS)"
    exit 0
else
    echo "RESULT: UNEXPECTED STATE ($final_resv)"
    exit 2
fi
