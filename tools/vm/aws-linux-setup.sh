#!/usr/bin/env bash
# Toolchain for strata's Linux gate on an AWS lease; idempotent (a resumed lease keeps its disk).
# Run by `lease run --setup` through aws-linux.sh.
set -euo pipefail
cloud-init status --wait >/dev/null || true
if command -v cmake >/dev/null && command -v ninja >/dev/null && command -v clang++ >/dev/null; then exit 0; fi
sudo apt-get -qq update
sudo DEBIAN_FRONTEND=noninteractive apt-get -qq install -y g++ clang lld cmake ninja-build git python3 \
	python3-venv liburing-dev zlib1g-dev libzstd-dev liblz4-dev uuid-dev curl pkg-config >/dev/null
