#!/bin/bash
set -euo pipefail

# Build kernel and run MM selftests in VM using vng

cd "$(dirname "$0")/../../"

KCONFIG_FRAGMENT=".github/kconfigs/mm-selftests.config"

echo "Building kernel with MM selftests configuration..."

# Build kernel with config fragment (silent mode - only errors)
if ! vng -v --build --config "$KCONFIG_FRAGMENT" --configitem CONFIG_DEBUG_INFO=n > /tmp/build.log 2>&1; then
    echo "Kernel build failed:"
    tail -100 /tmp/build.log
    exit 1
fi

echo "Kernel build completed successfully"

echo "Building MM selftests..."
if ! make -C tools/testing/selftests/mm > /tmp/selftests-build.log 2>&1; then
    echo "Selftests build failed:"
    cat /tmp/selftests-build.log
    exit 1
fi

echo "Running MM selftests in VM..."

# Run tests in VM with NUMA enabled (2 nodes)
# The kernel source is mounted at /host inside the VM
# Redirect both stdout and stderr to the log file
vng --cpus 4 --memory 4G --numa 2 -- bash -c 'cd /host/tools/testing/selftests/mm && sudo ./run_vmtests.sh 2>&1' > /tmp/vmtests.log 2>&1
exit_code=$?

if [ $exit_code -ne 0 ]; then
    echo "VM tests execution failed with exit code $exit_code"
    cat /tmp/vmtests.log
    exit 1
fi

# Parse and display results
echo ""
echo "=========================================="
echo "VM Test Results:"
echo "=========================================="

# Display the full output
cat /tmp/vmtests.log

# Check for test results summary
if grep -q "# SUMMARY:" /tmp/vmtests.log; then
    echo ""
    echo "=========================================="
    grep "# SUMMARY:" /tmp/vmtests.log -A 10 || true
    echo "=========================================="
fi

# Count passed, failed, skipped tests using TAP format
passed=$(grep -c "^ok " /tmp/vmtests.log 2>/dev/null || true)
failed=$(grep -c "^not ok " /tmp/vmtests.log 2>/dev/null || true)
skipped=$(grep -c "# SKIP" /tmp/vmtests.log 2>/dev/null || true)

# Default to 0 if empty
passed=${passed:-0}
failed=${failed:-0}
skipped=${skipped:-0}

echo ""
echo "Test Summary:"
echo "  Passed:  $passed"
echo "  Failed:  $failed"
echo "  Skipped: $skipped"

if [ "$failed" -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    grep "^not ok " /tmp/vmtests.log || true
    exit 1
fi

echo ""
echo "All VM tests passed"
exit 0
