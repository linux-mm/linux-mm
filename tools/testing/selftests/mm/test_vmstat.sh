#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# This is a test script for the kernel test module to measure performance of VM
# statistics.  It simply loads the kernel module.

TEST_NAME="vmstat"
DRIVER="test_${TEST_NAME}"

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

check_test_requirements()
{
	uid=$(id -u)
	if [ $uid -ne 0 ]; then
		echo "$0: Must be run as root"
		exit $ksft_skip
	fi

	if ! which modprobe > /dev/null 2>&1; then
		echo "$0: You need modprobe installed"
		exit $ksft_skip
	fi

	if ! modinfo $DRIVER > /dev/null 2>&1; then
		echo "$0: You must have the following enabled in your kernel:"
		echo "CONFIG_TEST_VMSTAT=m"
		exit $ksft_skip
	fi
}

run_performance_check()
{
	echo "Run performance tests to evaluate how fast vmstat is."

	modprobe $DRIVER > /dev/null 2>&1
	echo "Done."
	echo "Check the kernel message buffer to see the summary."
}

usage()
{
	echo -n "Usage: $0 performance"
	echo
	echo "Example usage:"
	echo
	echo "# Shows help message"
	echo "./${DRIVER}.sh"
	echo
	echo "# Performance analysis"
	echo "./${DRIVER}.sh performance"
	echo
	exit 0
}

function run_test()
{
	if [ $# -eq 0 ]; then
		usage
	else
		if [[ "$1" = "performance" ]]; then
			run_performance_check
		fi
	fi
}

check_test_requirements
run_test $@

exit 0
