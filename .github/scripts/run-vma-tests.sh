#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/common.sh"

test_dir=$linux_dir/tools/testing/vma

echo "Building VMA tests..."
make -C $test_dir > $log 2>&1  || fail "build of VMA tests failed"

echo "Running VMA tests..."
$test_dir/vma >$log 2>&1 || fail "VMA tests failed"

# Extract and display test results
echo "✓ VMA tests passed"
