#!/usr/bin/env bash
# strata's Linux gate: one script, configure + build + test, on a one-shot AWS machine or the local QEMU VM.
#   tools/vm/aws-linux.sh run TREE SCRIPT   ship TREE to ~/work, run SCRIPT there with $OUT set, results home
#   tools/vm/aws-linux.sh ssh [CMD...] | start | stop | status    the local VM (linux.sh)
# AWS only when STRATA_AWS_VM_ALLOW is set: `lease run` resumes or launches STRATA_AWS_VM (strata-ci-linux)
# of STRATA_AWS_VM_TYPE (c7a.8xlarge), stops it after STRATA_AWS_VM_TIMEOUT (30m default) and parks it. Without
# it, or when lease refuses (exit 1: caps, quota, name busy), the local VM runs the same script.
# SCRIPT's stdout and stderr print here; its $OUT files land in the directory printed last on stderr;
# the exit status is SCRIPT's (or lease's own failure, 2 and above).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
name="${STRATA_AWS_VM:-strata-ci-linux}"
type="${STRATA_AWS_VM_TYPE:-c7a.8xlarge}"
timeout="${STRATA_AWS_VM_TIMEOUT:-30m}"

local_vm() {  # up, then run the command under a light hold
	[ "$("$here/linux.sh" status)" = running ] || "$here/linux.sh" start >&2
	exec quiet light aws-linux-fallback 30 "local Linux VM" -- "$here/linux.sh" "$@"
}

run_local() {  # TREE SCRIPT: the same sync, run and results as lease run
	local tree=$1 script=$2 dst rc=0
	dst="$HOME/.scratch/aws-linux/local-$(date -u +%Y%m%dT%H%M%SZ)-$$"; mkdir -p "$dst"
	"$here/linux.sh" ssh 'rm -rf ~/work ~/out && mkdir -p ~/work ~/out'
	tar -C "$tree" -cf - . | "$here/linux.sh" ssh 'tar -xf - -C ~/work'
	"$here/linux.sh" ssh "cd ~/work && OUT=\$HOME/out bash -c $(printf %q "$(cat "$script")")" </dev/null || rc=$?
	"$here/linux.sh" ssh 'tar -C ~/out -cf - .' | tar -xf - -C "$dst"
	echo "aws-linux: results in $dst" >&2
	return "$rc"
}

case "${1:-}" in
run)
	tree="${2:?usage: aws-linux.sh run TREE SCRIPT}"; script="${3:?usage: aws-linux.sh run TREE SCRIPT}"
	if [ -z "${STRATA_AWS_VM_ALLOW:-}" ]; then
		echo "aws-linux: AWS not asked for (STRATA_AWS_VM_ALLOW unset); using the local VM" >&2
		exec quiet light aws-linux-fallback 30 "local Linux VM (AWS off)" -- "$0" run-local "$tree" "$script"
	fi
	mkdir -p "$HOME/.scratch"
	err="$(mktemp "$HOME/.scratch/aws-linux.XXXXXX")"
	# The script's own status goes in $OUT/rc and the unit exits 0, so lease's exit 1 means only that it refused.
	rc=0
	# shellcheck disable=SC2016  # expands on the machine
	lease run "$name" "$type" --timeout "$timeout" --sync "$tree" --setup "$here/aws-linux-setup.sh" -- \
		bash -c 'bash -c "$1"; echo $? >"$OUT/rc"' _ "$(cat "$script")" 2>"$err" || rc=$?
	cat "$err" >&2
	dst="$(sed -n 's/^lease: results in \(.*\) (exit .*)$/\1/p' "$err" | tail -1)"; rm -f "$err"
	if [ "$rc" = 1 ] && [ -z "$dst" ]; then
		echo "aws-linux: lease refused; using the local VM" >&2
		exec quiet light aws-linux-fallback 30 "local Linux VM (no lease)" -- "$0" run-local "$tree" "$script"
	fi
	for f in setup.log run.log; do [ -n "$dst" ] && [ -f "$dst/$f" ] && cat "$dst/$f"; done
	[ "$rc" = 0 ] || exit "$rc"
	exit "$(cat "$dst/rc")"
	;;
run-local) shift; run_local "$@" ;;
ssh) shift; local_vm ssh "$@" ;;
start) "$here/linux.sh" start ;;
stop|status) exec "$here/linux.sh" "$1" ;;
*) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
