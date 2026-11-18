#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/common.sh"

test_dir=$linux_dir/tools/testing/memblock

for numa in 0 1; do
    config="NUMA=$numa"
    echo "Building memblock tests ($config)..."
    make -C "$test_dir" clean >"$log" 2>&1
    make -C "$test_dir" NUMA="$numa" >"$log" 2>&1 ||
        fail "build of memblock tests failed ($config)"

    echo "Running memblock tests ($config)..."
    "$test_dir/main" -v >"$log" 2>&1 || fail "memblock tests failed ($config)"
    echo "Running memblock tests ($config movable-node)..."
    "$test_dir/main" -v -m >"$log" 2>&1 || fail "memblock tests failed ($config movable-node)"
done

# memblock tests use assert() which will abort on failure
# If we reach here, all tests passed
echo "✓ memblock tests passed"
exit 0
