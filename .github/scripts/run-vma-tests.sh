#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/common.sh"

test_dir=$linux_dir/tools/testing/vma

echo "Building VMA tests..."
make -C $test_dir 2>&1 >$log  || fail "build of VMA tests failed"

echo "Running VMA tests..."
$test_dir/vma -v 2>&1 > $log || fail "VMA tests failed"

# Extract and display test results
echo "✓ VMA tests passed"
