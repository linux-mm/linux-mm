#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Run CI scripts outside github workflows

set -euo pipefail

linux_dir=""

function run_tests() {
	local script_dir=$(dirname $(realpath $0))
	local ci_dir="$script_dir/../.github/scripts"

	export LINUX_DIR=$linux_dir
	"$ci_dir/run-host-tests.sh"
	"$ci_dir/build-kernel.sh"
	"$ci_dir/run-mm-selftests.sh"
	"$ci_dir/run-kunit-tests.sh"
}

function usage() {
    cat << EOF
Usage: $0 -l <linux-dir>

Options:
  -l, --linux-dir     	Linux kernel tree
  -h, --help		display this help message
EOF

  exit 1
}

function parse_opts() {
    OPTS=$(getopt -o l:h -l linux-dir:,help -- $@)
    eval set -- $OPTS

    while true; do
	case "$1" in
	    -l | --linux-dir)
		linux_dir=$2
		shift 2
		;;
	    -h | --help)
		usage
		;;
	    --)
		shift
		break
		;;
	    *)
		echo "Internal error!"
		exit 1
		;;
	esac
    done

    if [ -z $linux_dir ]; then
	    usage
	    exit 1
    fi
}

parse_opts $@
run_tests
