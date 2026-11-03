#!/usr/bin/env bash
# strata's Linux gate VM on AWS: one shared Ubuntu 24.04 lease (`lease`, c7a.8xlarge by default: 32 cores, no SMT),
# resumed or created on first use, parked after its idle minutes, so Linux builds leave this host's CPUs free.
#   tools/vm/aws-linux.sh ssh [CMD...] | start | stop | status
# Same interface as linux.sh; preflight picks it with PREFLIGHT_LINUX_VM=aws-linux.sh.
# STRATA_AWS_VM_PHASE labels ssh runs for `lease phases` (build by default; test or bench).
set -euo pipefail

name="${STRATA_AWS_VM:-strata-ci-linux}"
type="${STRATA_AWS_VM_TYPE:-c7a.8xlarge}"
hours="${STRATA_AWS_VM_HOURS:-4}"
lock="$HOME/.scratch/leases/$name.lock"
packages="g++ clang lld cmake ninja-build git python3 python3-venv liburing-dev zlib1g-dev libzstd-dev liblz4-dev uuid-dev curl pkg-config"

running() { lease list | awk -F'\t' -v n="$name" '$1 == n && $4 != "parked" { found = 1 } END { exit !found }'; }

up() {
	running && return 0
	mkdir -p "$(dirname "$lock")"
	until mkdir "$lock" 2>/dev/null; do sleep 5; done   # one creator; the others wait for it
	trap 'rmdir "$lock" 2>/dev/null || true' EXIT
	if ! running; then
		# Resumes the parked lease (disk, toolchain and ccache intact) or launches one; without
		# capacity (quota or budget), the local VM.
		if ! lease up "$name" "$type" --hours "$hours" >/dev/null; then
			rmdir "$lock" 2>/dev/null || true
			echo "aws-linux: no lease; using the local VM" >&2
			[ "$("$(dirname "$0")/linux.sh" status)" = running ] || "$(dirname "$0")/linux.sh" start >&2
			# The local VM takes this host's CPUs, so the fallback runs under a light hold.
			exec quiet light aws-linux-fallback 30 "local Linux VM (no lease)" -- "$(dirname "$0")/linux.sh" "${args[@]}"
		fi
		lease ssh "$name" --phase setup "cloud-init status --wait >/dev/null; command -v cmake >/dev/null && command -v ninja >/dev/null && command -v clang++ >/dev/null || { sudo apt-get -qq update && sudo DEBIAN_FRONTEND=noninteractive apt-get -qq install -y $packages >/dev/null; }" >&2
	fi
	rmdir "$lock" 2>/dev/null || true
	trap - EXIT
}

args=("$@")
case "${1:-}" in
ssh) shift; up; exec lease ssh "$name" --phase "${STRATA_AWS_VM_PHASE:-build}" "$@" ;;   # preflight's build-and-test runs count as build
start) up; echo "up: $name" ;;
stop) lease down "$name" ;;
status) lease list | grep -w "$name" || echo "not running" ;;
*) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
