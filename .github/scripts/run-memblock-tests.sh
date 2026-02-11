#!/bin/bash
set -euo pipefail

# Run memblock tests

cd "$(dirname "$0")/../../tools/testing/memblock"

echo "Building memblock tests..."
if ! make -s > /tmp/memblock-build.log 2>&1; then
    echo "Build failed:"
    cat /tmp/memblock-build.log
    exit 1
fi

echo "Running memblock tests..."
if ! ./main > /tmp/memblock-run.log 2>&1; then
    echo "Tests failed:"
    cat /tmp/memblock-run.log
    exit 1
fi

# memblock tests use assert() which will abort on failure
# If we reach here, all tests passed
echo "✓ All memblock tests passed"
exit 0
