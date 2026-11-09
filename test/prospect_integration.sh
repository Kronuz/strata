#!/bin/bash
# prospect against this strata checkout: prospect's ctest, then churn and dump output identical to
# prospect on strata at MAIN, at flush threads 1 and 2 (the µs column dropped).
# Usage: test/prospect_integration.sh PROSPECT MAIN [OUT]; run from this checkout's root.
# As a job: --ref prospect=../prospect@BRANCH --ref main=main, then "$JOB_REF_PROSPECT" "$JOB_REF_MAIN".
set -u
P=$1 M=$2 OUT=${3:-${JOB_OUT:-.scratch}/prospect}
S=$PWD
mkdir -p "$OUT"
fail=0
cfg() { cmake -S "$P" -B "$OUT/$1" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_STRATA="$2" > "$OUT/$1-config.log" 2>&1; }
cfg branch "$S" && ninja -C "$OUT/branch" -j4 > "$OUT/branch-build.log" 2>&1 || { echo "prospect build against $S failed"; exit 1; }
cfg main "$M" && ninja -C "$OUT/main" -j4 prospect_engine > "$OUT/main-build.log" 2>&1 || { echo "prospect build against $M failed"; exit 1; }
if ctest --test-dir "$OUT/branch" --output-on-failure -j4 --timeout 1800 > "$OUT/ctest.log" 2>&1; then echo "ctest: $(grep 'tests passed' "$OUT/ctest.log")"; else echo "ctest FAILED: $(grep 'tests passed\|tests failed' "$OUT/ctest.log")"; fail=1; fi

python3 "$S/test/prospect_churn_gen.py" plain > "$OUT/churn-plain.ops"
python3 "$S/test/prospect_churn_gen.py" longkey > "$OUT/churn-longkey.ops"
printf '%s\n' '{"query": "w1"}' '{"query": "+w2 +w3"}' '{"query": "w4 w5 -w6"}' '{"query": "\"w7 w8\""}' '{"query": "w9 w10 w11"}' '{"query": "w12 w13 w14 w15"}' > "$OUT/dump-queries.jsonl"
for th in 1 2; do
	for ops in plain longkey; do
		for arm in main branch; do
			d=$OUT/id/$arm-$ops-t$th; e=$OUT/$arm/prospect_engine
			rm -rf "$d"; mkdir -p "$d"
			PROSPECT_THREADS=$th "$e" churn "$d" < "$OUT/churn-$ops.ops" > "$d.out" 2> "$d.err" || { echo "$arm $ops t$th churn rc=$?"; fail=1; }
			cut -f1,3- "$d.out" > "$d.cmp"
			if [ $ops = plain ]; then "$e" dump "$d" < "$OUT/dump-queries.jsonl" > "$d.dump" 2>> "$d.err" || { echo "$arm $ops t$th dump rc=$?"; fail=1; }; fi
			rm -rf "$d"
		done
		for f in cmp dump; do
			a=$OUT/id/main-$ops-t$th.$f b=$OUT/id/branch-$ops-t$th.$f
			[ -f "$a" ] || continue
			if [ -s "$b" ] && cmp -s "$a" "$b"; then echo "t$th $ops $f identical ($(wc -l < "$b" | tr -d ' ') lines)"; else echo "t$th $ops $f DIFFER"; fail=1; fi
		done
	done
done
rm -rf "$OUT/branch" "$OUT/main"
[ $fail -eq 0 ] && echo "prospect integration: ok" || echo "prospect integration: FAILED"
exit $fail
