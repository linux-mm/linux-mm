#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/common.sh"

test_dir=$linux_dir/tools/testing/memblock

echo "Building memblock tests..."
make -C $test_dir >$log 2>&1  || fail "build of memblock tests failed"

echo "Running memblock tests..."
$test_dir/main -v >$log 2>&1 || fail "memblock tests failed"

# memblock tests use assert() which will abort on failure
# If we reach here, all tests passed
echo "✓ memblock tests passed"
exit 0
