#!/bin/bash
set -euo pipefail

# Run MM host tests: memblock, radix-tree, vma

cd "$(dirname "$0")/../../"

# Array format: "test_dir:executable_name"
TESTS=(
    "tools/testing/memblock:main"
    "tools/testing/radix-tree:main"
    "tools/testing/vma:vma"
)

passed=0
failed=0
skipped=0
failed_tests=()

for test_spec in "${TESTS[@]}"; do
    test_dir="${test_spec%:*}"
    test_exe="${test_spec#*:}"
    test_name=$(basename "$test_dir")
    
    echo "Running $test_name tests..."
    
    if [ ! -d "$test_dir" ]; then
        echo "SKIP: $test_name (directory not found)"
        skipped=$((skipped + 1))
        continue
    fi
    
    cd "$test_dir"
    
    # Build and run, capturing output
    if make -s > /tmp/${test_name}.log 2>&1 && ./$test_exe >> /tmp/${test_name}.log 2>&1; then
        echo "PASS: $test_name"
        passed=$((passed + 1))
    else
        echo "FAIL: $test_name"
        cat /tmp/${test_name}.log
        failed=$((failed + 1))
        failed_tests+=("$test_name")
    fi
    
    make -s clean 2>/dev/null || true
    cd - > /dev/null
done

echo ""
echo "=========================================="
echo "Test Summary:"
echo "  Passed:  $passed"
echo "  Failed:  $failed"
echo "  Skipped: $skipped"
echo "=========================================="

if [ $failed -gt 0 ]; then
    echo "Failed tests:"
    for test in "${failed_tests[@]}"; do
        echo "  - $test"
    done
    exit 1
fi

exit 0
