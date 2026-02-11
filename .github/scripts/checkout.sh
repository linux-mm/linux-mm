#!/bin/bash
set -euo pipefail
set -x

# Shallow clone the kernel repository
# Uses local mirror on self-hosted runners if available

REPO_URL="https://x-access-token:${GITHUB_TOKEN}@${GITHUB_SERVER_URL#https://}/$GITHUB_REPOSITORY"
MIRROR_PATH="/mirror/linux.git"

if [ -d "$MIRROR_PATH" ]; then
    echo "Using local mirror at $MIRROR_PATH"
    git clone --depth 2 --reference "$MIRROR_PATH" "$REPO_URL" linux
else
    echo "Cloning from $REPO_URL"
    git clone --depth 2 "$REPO_URL" linux
fi

cd linux
git fetch origin ${LINUX_TREE_REF}
git checkout "${LINUX_TREE_REF}"
echo "Checked out $(git rev-parse HEAD)"
