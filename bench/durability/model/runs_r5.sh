#!/bin/bash
# Revision 5's runs (DM-R33, DM-R34, DMS-15, DMS-16): check 3 over a merge output's every ordinal
# (check3_model.py), the `Vouch` rule and its controls (vouch_model.py), and check 5's per-term rule
# (term_value_check.cc, built against strata's include/ and the pinned headers in DEPS). One at a
# time, at nice 19 under run_bounded.sh's cap, each launch only when `quiet check` exits 0 or 2.
# Usage: DEPS=path/to/deps runs_r5.sh OUTDIR
set -u
here=$(cd "$(dirname "$0")" && pwd)
out=$1
R="$here/run_bounded.sh"
PY="/usr/local/bin/python3 -I"
hold_wait() { while true; do quiet check >/dev/null 2>&1; [ $? -ne 1 ] && return; quiet wait --light >/dev/null 2>&1 || sleep 30; done; }
run() { local name=$1 cap=$2; shift 2; hold_wait; /usr/bin/time -l -o "$out/$name.time" "$R" "$out/$name.out" "$cap" "$@"; }

run check3-in2-out3 300 $PY "$here/check3_model.py" --maxin 2 --maxout 3
run check3-in2-out4 900 $PY "$here/check3_model.py" --maxin 2 --maxout 4
run check3-in3-out3 900 $PY "$here/check3_model.py" --maxin 3 --maxout 3
for rule in position entry_ungrounded position_grounded; do
  run vouch-$rule-e3-d13 300 $PY "$here/vouch_model.py" --rule $rule --maxe 3 --depth 13 --cap 1500000
done
run vouch-entry-e2-all 900 $PY "$here/vouch_model.py" --rule entry --maxe 2 --depth 60 --cap 1500000
run vouch-entry-e3-d13 900 $PY "$here/vouch_model.py" --rule entry --maxe 3 --depth 13 --cap 1500000
if [ -n "${DEPS:-}" ]; then
  root=$(cd "$here/../../.." && pwd)
  mkdir -p "$root/.scratch/r5"
  hold_wait
  nice -n 19 /usr/local/opt/llvm/bin/clang++ -std=c++20 -O1 -Wall -Wextra -I"$root/include" -I"$DEPS/termdict-src" -I"$DEPS/fst-src" \
    -I"$DEPS/varint-src" -I"$DEPS/hashes-src" "$root/bench/durability/term_value_check.cc" -o "$root/.scratch/r5/term_value_check" 2> "$out/term_value_check.build"
  run term_value_check 60 "$root/.scratch/r5/term_value_check"
fi
