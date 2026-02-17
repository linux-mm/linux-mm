#!/bin/bash
set -euo pipefail

test_script=$(basename $(realpath $0))
linux_dir=$(dirname $(realpath $0))"/../.."

log_dir=$(mktemp -d)
log=$log_dir/$test_script.log

function cleanup() {
echo    rm -fr "$log_dir"
}
trap cleanup EXIT

function fail() {
	local msg=${1:-"X Test failed"}

	echo "$msg"
	cat $log
	exit 1
}
