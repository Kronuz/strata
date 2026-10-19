#!/bin/bash
# Revision 3's model runs (DM-R26, DM-R28): D4's vote barrier in the model, the window schedules
# and searches redone under it, and the every-voter rule for `manual`. Each run is under
# run_bounded.sh's wall-clock cap, at nice 19, one thread, at most MAXJOBS (default 2) at once.
# Usage: CHECK=path/to/forgetful_check runs_r3.sh OUTDIR GROUP...; groups below.
# Build first: c++ -std=c++20 -O2 -o forgetful_check forgetful_check.cc (seven nodes, four changes).
set -u
here=${MODEL_DIR:-$(cd "$(dirname "$0")" && pwd)}
out=$1; shift
C=${CHECK:?the checker binary}; S="$here/schedules"; R="$here/run_bounded.sh"
PY="/usr/local/bin/python3 -I $here/forgetful_model.py"
SIX="--voters 6 --base 0,1,2,3,4,5 --script remove:2 --fmax 2 --maxos 4 --maxt 5"
GROW="--voters 6 --base 0,1,2,3,4 --script add:5,remove:2 --fmax 2 --maxos 5 --maxt 5"
G67="--voters 7 --base 0,1,2,3,4 --script add:5,add:6 --fmax 2 --maxos 4 --maxt 5"
SHR="--voters 5 --base 0,1,2,3,4 --script remove:4,remove:3 --fmax 1 --maxos 3 --maxt 4"
RP2="--voters 5 --base 0,1,2 --script add:3,remove:0,add:4,remove:1 --fmax 1 --maxos 2 --maxt 5"
RF3="--voters 4 --base 0,1,2 --script add:3,remove:1 --fmax 1"

jobs_wait() { while [ "$(jobs -rp | wc -l)" -ge "${MAXJOBS:-2}" ]; do sleep 5; done; }
run() { local name=$1 cap=$2; shift 2; jobs_wait; "$R" "$out/$name.out" "$cap" "$@" & }
# A forced schedule in both checkers: the C++ trace (replicate and sync steps elided), then
# Python's result line. Usage: both MODE "ARGS" SCHEDULE.
both() {
  local mode=$1 args=$2 sched=$3
  echo "# $sched: $mode $args"
  "$C" $mode $args --schedule "$S/$sched.txt" | grep -v '^  *[0-9]*\. \(replicate\|sync\) '
  $PY $mode $args --schedule "$sched" | grep 'result\|not enabled'
}

for group in "$@"; do
case $group in
crosscheck)
  # Python against C++ on the vote barrier: equal state counts, each barrier mode, fixed and
  # changing membership (no symmetry, so the C++ count is the raw one).
  {
    for b in none candidate all; do
      $PY round --voters 3 --maxt 2 --restart persist --torn --maxr 2 --depth 7 --barrier $b | sed -n 2,3p
      "$C" round --voters 3 --maxt 2 --restart persist --torn --maxr 2 --depth 7 --barrier $b | sed -n 1,3p
      $PY round $RF3 --maxos 2 --maxt 3 --count major --depth 7 --barrier $b | sed -n 2,3p
      "$C" round $RF3 --maxos 2 --maxt 3 --count major --depth 7 --barrier $b | sed -n 1,3p
      $PY round $SIX --count f1 --schedule six --from-step 38 --depth 3 --barrier $b | sed -n 2,3p
      "$C" round $SIX --count f1 --schedule "$S/six.txt" --from-step 38 --depth 3 --barrier $b | sed -n 1,4p
      $PY round $G67 --count f1 --schedule grow67 --from-step 65 --depth 3 --barrier $b | sed -n 2,3p
      "$C" round $G67 --count f1 --schedule "$S/grow67.txt" --from-step 65 --depth 3 --barrier $b | sed -n 1,4p
    done
  } > "$out/crosscheck.out" 2>&1
  ;;
schedules)
  # Every forced schedule under each barrier mode, both checkers. grow6, grow6_loss, and the
  # shrink fence are the revision-2 model's artifacts: not enabled once the candidate syncs.
  {
    for b in none candidate all; do
      echo "##### barrier $b"
      for s in restart3 restart3p; do for r in text persist; do both round "--voters 3 --maxt 2 --restart $r --barrier $b" $s; done; done
      for r in text persist; do both round "--voters 3 --maxt 2 --restart $r --torn --maxr 2 --barrier $b" torn3; done
      for count in f1 major; do
        for s in six six_loss; do both round "$SIX --count $count --barrier $b" $s; done
        for s in grow6 grow6_loss; do both round "$GROW --count $count --barrier $b" $s; done
        for s in grow67 grow67_loss; do both round "$G67 --count $count --barrier $b" $s; done
        both round "$SHR --count $count --barrier $b" shrink_fence
      done
      # DM-R26: `manual`'s f + 1 admission with every voter's answer required (round_heard, no
      # timeout): the voters that hold v0 refuse X, so neither window decides.
      both round_heard "$SIX --count f1 --barrier $b" six_heard
      both round_heard "$G67 --count f1 --barrier $b" grow67_heard
      # Revision 1's schedules, in results/model-schedules.out's configurations.
      for m in timeout fplus round; do both $m "--voters 3 --maxt 2 --barrier $b" dmr1; done
      for s in fplus5 fplus5_vb; do for m in timeout fplus round; do both $m "--voters 5 --maxos 3 --maxt 4 --barrier $b" $s; done; done
      for s in heard5 heard5_vb; do for m in heard fplus_heard round round_heard; do both $m "--voters 5 --maxos 3 --maxt 4 --barrier $b" $s; done; done
      for m in fplus round; do both $m "--voters 5 --maxos 3 --maxt 5 --barrier $b" fplus5b; done
      for m in round round_heard; do both $m "--voters 3 --fmax 3 --maxos 3 --maxt 3 --barrier $b" live3; done
    done
  } > "$out/schedules.out" 2>&1
  ;;
six)
  # rf = 5's removal window: the calibrated pairs from `six`'s prefixes, both counts.
  for count in f1 major; do
    run cc-six38-$count-d5 2400 "$C" round $SIX --count $count --schedule "$S/six.txt" --from-step 38 --depth 5 --key64 --table 27
  done
  for count in f1 major; do
    run cc-six37-$count-d6 3600 "$C" round $SIX --count $count --schedule "$S/six.txt" --from-step 37 --depth 6 --key64 --table 28
  done
  ;;
six33)
  for count in f1 major; do
    run cc-six33-$count-d5 3600 "$C" round $SIX --count $count --schedule "$S/six.txt" --from-step 33 --depth 5 --key64 --table 27
  done
  ;;
grow67)
  # rf = 5 to 7: from `grow67`'s step 65 (the round's first two answers in, before W's election).
  for count in f1 major; do
    run cc-grow67-65-$count-d5 3600 "$C" round $G67 --count $count --schedule "$S/grow67.txt" --from-step 65 --depth 5 --key64 --table 28
  done
  ;;
grow6)
  # rf = 5's growth half under the barrier: from `grow6`'s step 42, X's round with two answers in,
  # the step before revision 2's model elected W under the founding five (the artifact).
  for count in f1 major; do
    run cc-grow42-$count-d6 3600 "$C" round $GROW --count $count --schedule "$S/grow6.txt" --from-step 42 --depth 6 --key64 --table 28
  done
  ;;
shapes)
  # rf = 5 to 3 (four voters between C5 and C3) and two replacements at rf = 3, the architect's
  # prefixes, within the model (FMAX 1, the least f in force).
  for count in f1 major; do
    run cc-shrink31-$count-d7 2400 "$C" round $SHR --count $count --schedule "$S/shrink31.txt" --from-step 31 --depth 7 --key64 --table 27
  done
  for count in f1 major; do
    run cc-replace2-64-$count-d8 2400 "$C" round $RP2 --count $count --schedule "$S/replace2.txt" --from-step 64 --depth 8 --key64 --table 27
  done
  ;;
rf3)
  # Track C's rf = 3 window: from its founding state, and from `rf3win`'s step 24.
  for count in f1 major; do
    run cc-rf3win-$count-d10 3600 "$C" round $RF3 --maxos 2 --maxt 3 --count $count --symmetry --key64 --table 27 --depth 10
  done
  for count in f1 major; do
    run cc-rf3win24-$count-d8 3600 "$C" round $RF3 --maxos 2 --maxt 4 --count $count --schedule "$S/rf3win.txt" --from-step 24 --depth 8 --key64 --table 27
  done
  ;;
heard)
  # DM-R26: `manual`'s f + 1 admission with every voter of the membership answering, no timeout,
  # from the calibrated point where `round` with f + 1 loses v0 within 6 steps.
  run cc-six37-heard-f1-d6 3600 "$C" round_heard $SIX --count f1 --schedule "$S/six.txt" --from-step 37 --depth 6 --key64 --table 28
  ;;
restart)
  # DM-R19's searches under the barrier: three voters with torn tails, and five voters.
  run cc-torn-persist-n3-os0-d10 1800 "$C" round --voters 3 --maxos 0 --maxt 3 --restart persist --torn --maxr 2 --depth 10
  run cc-persist-torn-retry-n5-d7 3600 "$C" round --voters 5 --maxos 3 --maxt 4 --restart persist --torn --maxr 1 --retry --symmetry --key64 --table 27 --depth 7
  ;;
restart11)
  run cc-persist-torn-retry-n3-p2-d11 3600 "$C" round --voters 3 --maxos 3 --maxp 2 --maxt 4 --restart persist --torn --maxr 2 --retry --symmetry --key64 --table 28 --depth 11
  ;;
esac
done
wait
