#!/bin/bash
set -euo pipefail

# Run radix-tree tests (multiple executables)

cd "$(dirname "$0")/../../tools/testing/radix-tree"

echo "Building radix-tree tests..."
if ! make -s > /tmp/radix-tree-build.log 2>&1; then
    echo "Build failed:"
    cat /tmp/radix-tree-build.log
    exit 1
fi

# List of test executables in order
TESTS=(
    "main"
    "idr-test"
    "multiorder"
    "xarray"
    "maple"
)

failed=0
failed_tests=()

for test in "${TESTS[@]}"; do
    echo "Running $test..."
    if ! ./$test > /tmp/radix-tree-${test}.log 2>&1; then
        echo "✗ $test failed"
        cat /tmp/radix-tree-${test}.log
        failed=$((failed + 1))
        failed_tests+=("$test")
    else
        # Check for test result summary in output
        if grep -q "tests passed" /tmp/radix-tree-${test}.log; then
            result=$(grep "tests passed" /tmp/radix-tree-${test}.log | tail -1)
            echo "✓ $test: $result"
        else
            echo "✓ $test passed"
        fi
    fi
done

echo ""
echo "=========================================="
echo "Radix-tree test summary:"
echo "  Total executables: ${#TESTS[@]}"
echo "  Passed: $((${#TESTS[@]} - failed))"
echo "  Failed: $failed"
echo "=========================================="

if [ $failed -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for test in "${failed_tests[@]}"; do
        echo "  - $test"
    done
    exit 1
fi

exit 0
