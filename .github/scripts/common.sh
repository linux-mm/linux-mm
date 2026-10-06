#!/bin/bash
set -euo pipefail

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

test_script=$(basename $(realpath $0))
linux_dir=${LINUX_DIR:-$(dirname $(realpath $0))"/../.."}
mm_ci_dir=$(dirname $(realpath $0))"/../.."

tmp_dir=$(mktemp -d)
log=$tmp_dir/$test_script.log

function log_checkout_info() {
	local server=${GITHUB_SERVER_URL:-https://github.com}
	local repo=${GITHUB_REPOSITORY:-linux-mm/linux-mm}
	local commit

	# The tested tree is the merge commit GitHub creates for the pull
	# request, so it only ever exists in the GitHub repository.
	commit=$(git -C "$linux_dir" rev-parse HEAD 2>/dev/null) ||
		commit=${GITHUB_SHA:-}
	[ -n "$commit" ] || return

	cat <<EOF
To check out the tree the tests ran on, you may use the following commands:
git fetch $server/$repo.git $commit
git checkout FETCH_HEAD
EOF
}

function cleanup() {
    local rc=$?

    rm -fr "$tmp_dir"

    # Repeat on failure, so the information is also right next to the failure
    # output rather than only at the top of a long log.
    [ $rc -eq 0 ] || log_checkout_info
}
trap cleanup EXIT

log_checkout_info

function fail() {
	local msg=${1:-"✗ Test failed"}

	cat $log
	echo "✗ $msg"
	exit 1
}

function pass() {
	local msg=${1:-"✗ Test passed"}

	echo "✓ $msg"
	exit 0
}
