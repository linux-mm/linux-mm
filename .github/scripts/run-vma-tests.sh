#!/bin/bash
set -euo pipefail

# Run VMA tests

cd "$(dirname "$0")/../../tools/testing/vma"

echo "Building VMA tests..."
if ! make -s > /tmp/vma-build.log 2>&1; then
    echo "Build failed:"
    cat /tmp/vma-build.log
    exit 1
fi

echo "Running VMA tests..."
if ! ./vma > /tmp/vma-run.log 2>&1; then
    echo "Tests failed:"
    cat /tmp/vma-run.log
    
    # Extract failure details if available
    if grep -q "failed" /tmp/vma-run.log; then
        echo ""
        echo "Failure summary:"
        grep "FAILED\|failed" /tmp/vma-run.log
    fi
    exit 1
fi

# Extract and display test results
result=$(grep "tests run" /tmp/vma-run.log)
echo "✓ VMA tests: $result"

# Verify no failures
if grep -q "0 failed" /tmp/vma-run.log; then
    exit 0
else
    echo "Some tests failed"
    cat /tmp/vma-run.log
    exit 1
fi
