#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/common.sh"

cd $linux_dir

echo "Building kernel with MM selftests configuration..."
# Remove any stale .config so vng regenerates it from the fragments below
rm -f $linux_dir/.config
KCONFIG_OPTS=(
	--config "$mm_ci_dir/.github/kconfigs/mm-selftests.config"
	--config "$linux_dir/tools/testing/selftests/mm/config"
)
vng --build "${KCONFIG_OPTS[@]}" &> $log.noisy && pass "Kernel build passed"

# remove config "warning: override:" from the log
grep -v "warning: override:" $log.noisy > $log || true
fail "Kernel build failed"
