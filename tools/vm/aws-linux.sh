#!/usr/bin/env bash
# strata's Linux gate: one script, configure + build + test, on the local QEMU VM (AWS is retired).
#   tools/vm/aws-linux.sh run TREE SCRIPT   ship TREE to a fresh per-run directory on the VM, run SCRIPT
#                                           there with $OUT set, results home
#   tools/vm/aws-linux.sh ssh [CMD...] | start | stop | status    the local VM (linux.sh)
# Each run gets its own ~/runs/ID/{work,out} and removes only that, so runs from several agents can
# share the VM. SCRIPT's stdout and stderr print here; its $OUT files land in the directory printed
# last on stderr; the exit status is SCRIPT's.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"

local_vm() {  # up, then run the command under a light hold
	[ "$("$here/linux.sh" status)" = running ] || "$here/linux.sh" start >&2
	exec quiet light linux-vm 30 "local Linux VM" -- "$here/linux.sh" "$@"
}

run_local() {  # TREE SCRIPT
	local tree=$1 script=$2 id dst rc=0
	id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
	dst="$HOME/.scratch/aws-linux/local-$id"; mkdir -p "$dst"
	[ "$("$here/linux.sh" status)" = running ] || "$here/linux.sh" start >&2
	"$here/linux.sh" ssh "mkdir -p ~/runs/$id/work ~/runs/$id/out"
	tar -C "$tree" -cf - . | "$here/linux.sh" ssh "tar -xf - -C ~/runs/$id/work"
	"$here/linux.sh" ssh "cd ~/runs/$id/work && OUT=\$HOME/runs/$id/out bash -c $(printf %q "$(cat "$script")")" </dev/null || rc=$?
	"$here/linux.sh" ssh "tar -C ~/runs/$id/out -cf - ." | tar -xf - -C "$dst"
	"$here/linux.sh" ssh "rm -rf ~/runs/$id"
	echo "aws-linux: results in $dst" >&2
	return "$rc"
}

case "${1:-}" in
run)
	tree="${2:?usage: aws-linux.sh run TREE SCRIPT}"; script="${3:?usage: aws-linux.sh run TREE SCRIPT}"
	exec quiet light linux-vm 30 "local Linux VM" -- "$0" run-local "$tree" "$script"
	;;
run-local) shift; run_local "$@" ;;
ssh) shift; local_vm ssh "$@" ;;
start) "$here/linux.sh" start ;;
stop|status) exec "$here/linux.sh" "$1" ;;
*) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
