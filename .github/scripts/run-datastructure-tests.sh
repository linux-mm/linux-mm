#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/common.sh"

test_dir=$linux_dir/tools/testing/radix-tree

echo "Building data structure tests..."
make -C $test_dir -s &> $log || fail "Build of data structure tests failed"

declare -A TESTS
TESTS["main"]="Radix tree and IDA"
TESTS["idr-test"]="IDR"
TESTS["multiorder"]="Multiorder XArray"
TESTS["xarray"]="XArray"
TESTS["maple"]="Maple tree"

FLAKY_TESTS="maple"

failed=()

for test in "${!TESTS[@]}"; do
    test_name="${TESTS[$test]}"
    echo "Running $test_name tests"
    if ! $test_dir/$test &> $log; then
        cat $log
        if echo $FLAKY_TESTS | grep $test &>/dev/null; then
             echo "? flaky $test_name tests failed"
             continue
        fi
        failed+=("$test_name")
        echo "✗ $test_name tests failed"
    else
        echo "✓ $test_name tests passed"
    fi
done

if [ ${#failed[@]} -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for test in "${failed[@]}"; do
        echo "  - $test"
    done
    exit 1
fi

exit 0
