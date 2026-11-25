#!/usr/bin/env bash
# ab.sh row, heap and identity refuse binaries built at other commits than the job asks for, and
# identity prints the sides line. Fake binaries and commit stamps stand in for a build:
#   test_guard.sh      exits 0 when every case behaves
set -u
here="$(cd "$(dirname "$0")/../.." && pwd)"
ab="$here/bench/eager_open/ab.sh"
head=$(git -C "$here" rev-parse HEAD)
other=0000000000000000000000000000000000000000
work=$(mktemp -d "${TMPDIR:-/tmp}/ab-guard.XXXXXX")
trap 'rm -rf "$work"' EXIT
fails=0

stamp() {  # SIDE STRATA PROSPECT
	mkdir -p "$work/$1"
	printf '%s\n%s\n' "$2" "$3" >"$work/$1/commit"
	for b in eager_open eager_open_count; do
		printf '#!/bin/sh\necho ran >>"%s/ran"\n' "$work" >"$work/$1/$b"
		chmod +x "$work/$1/$b"
	done
}
fresh() {  # all sides stamped as the job asks
	rm -rf "${work:?}"/*
	for s in base proto aa; do stamp $s "$head" "$head"; done
}
attempt() {  # NAME EXPECT(ok|refuse) CMD...
	local name=$1 want=$2
	shift 2
	rm -f "$work/ran" "$work/log"
	JOB_REF_BASE=$here JOB_REF_PROSPECT=$here SIDES='base proto aa' "$ab" "$@" >"$work/log" 2>&1
	local rc=$?
	if [ "$want" = ok ] && { [ $rc -ne 0 ] || [ ! -e "$work/ran" ]; }; then
		echo "FAIL $name: exit $rc, expected a run"; cat "$work/log"; fails=$((fails + 1))
	elif [ "$want" = refuse ] && { [ $rc -eq 0 ] || [ -e "$work/ran" ]; }; then
		echo "FAIL $name: exit $rc, ran=$([ -e "$work/ran" ] && echo yes || echo no), expected a refusal before any run"; fails=$((fails + 1))
	fi
}
cmds=("row $work f 1 1 a" "heap $work f 1 a" "identity $work 1")

fresh
for c in "${cmds[@]}"; do attempt "matching ${c%% *}" ok $c; done
fresh
JOB_REF_BASE=$here JOB_REF_PROSPECT=$here SIDES='base proto aa' "$ab" identity "$work" 1 2>&1 | grep -q '^sides: ' || { echo "FAIL identity prints no sides line"; fails=$((fails + 1)); }

fresh; stamp proto "$other" "$head"      # a leftover proto build
for c in "${cmds[@]}"; do attempt "stale proto ${c%% *}" refuse $c; done
fresh; stamp base "$other" "$head"       # a leftover base build
for c in "${cmds[@]}"; do attempt "stale base ${c%% *}" refuse $c; done
fresh; stamp proto "$head" "$other"      # one side over another prospect
for c in "${cmds[@]}"; do attempt "other prospect ${c%% *}" refuse $c; done

[ $fails -eq 0 ] && echo "ab_guard: ok"
exit $((fails != 0))
