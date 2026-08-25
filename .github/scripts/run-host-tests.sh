#!/bin/bash
set -euo pipefail

tests_dir=$(dirname $(realpath $0))

source $tests_dir/common.sh

declare -A TESTS
TESTS["run-memblock-tests.sh"]="memblock"
TESTS["run-vma-tests.sh"]="VMA"
TESTS["run-datastructure-tests.sh"]="Data structure"

failed=()
passed=()

for test in "${!TESTS[@]}"; do
    test_name="${TESTS[$test]}"
    if ! $tests_dir/$test; then
        failed+=("$test_name")
    else
        passed+=("$test_name")
    fi
done

if [ ${#passed[@]} -gt 0 ]; then
    echo ""
    echo "Passed tests:"
    for test in "${passed[@]}"; do
        echo "  - $test"
    done
fi

if [ ${#failed[@]} -gt 0 ]; then
    echo "Failed tests:"
    for test in "${failed[@]}"; do
        echo "  - $test"
    done
    exit 1
fi
