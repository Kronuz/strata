#!/bin/sh
# usage: repeat_load.sh BUILD_DIR WAVES PROCS CMD...   runs PROCS copies of CMD at once, WAVES times;
# prints the failure count and exits 1 when any run failed
dir=$1 waves=$2 procs=$3; shift 3
fail=0 total=0
w=0
while [ $w -lt $waves ]; do
	pids=""
	i=0
	while [ $i -lt $procs ]; do
		(cd "$dir" && "$@" >/dev/null 2>"$dir/.flake.$w.$i") &
		pids="$pids $!"
		i=$((i + 1))
	done
	i=0
	for p in $pids; do
		total=$((total + 1))
		if ! wait $p; then fail=$((fail + 1)); echo "--- failure wave $w proc $i"; cat "$dir/.flake.$w.$i"; fi
		rm -f "$dir/.flake.$w.$i"
		i=$((i + 1))
	done
	w=$((w + 1))
done
echo "flake result: $fail failures of $total runs"
[ $fail -eq 0 ]
