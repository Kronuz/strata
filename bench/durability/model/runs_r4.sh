#!/bin/bash
# Revision 4's model runs (DM-R29, DM-R30): the first-turn marks of several groups with
# reclamation's slices held until the last mark turn is written (`--ride none`, D3 rule 2), against
# slices of marked groups only (`own`) and the rule as revision 3 wrote it (`any`, the control); and
# the owner's rule at five voters under every barrier D4 imposes (`lead5`, `heard5l`, `lead5_loss`).
# Each run is under run_bounded.sh's wall-clock cap, at nice 19, one thread, at most MAXJOBS
# (default 2) at once, and no launch starts under a strict hold.
# Usage: CHECK=path/to/forgetful_check runs_r4.sh OUTDIR GROUP...; groups below.
set -u
here=${MODEL_DIR:-$(cd "$(dirname "$0")" && pwd)}
out=$1; shift
R="$here/run_bounded.sh"; S="$here/schedules"
MG="/usr/local/bin/python3 -I $here/mark_groups.py"
PY="/usr/local/bin/python3 -I $here/forgetful_model.py"

jobs_wait() { while [ "$(jobs -rp | wc -l)" -ge "${MAXJOBS:-2}" ]; do sleep 5; done; }
# Before each launch: `quiet check` exits 1 under a strict hold, and the launch waits for it to end;
# 2 (light holds only) or 0 lets it start, at nice 19 as run_bounded.sh runs it.
hold_wait() { while true; do quiet check >/dev/null 2>&1; [ $? -ne 1 ] && return; quiet wait --light >/dev/null 2>&1 || sleep 30; done; }
run() { local name=$1 cap=$2; shift 2; jobs_wait; hold_wait; "$R" "$out/$name.out" "$cap" "$@" & }
both() {
  local mode=$1 args=$2 sched=$3
  echo "# $sched: $mode $args"
  "$C" $mode $args --schedule "$S/$sched.txt" | grep -v '^  *[0-9]*\. \(replicate\|sync\) '
  $PY $mode $args --schedule "$sched" | grep 'result\|not enabled'
}

for group in "$@"; do
case $group in
marks)
  # Two groups, one mark per turn, a clean close allowed between mark turns. The calibration at
  # one group reproduces mark_model.py's count (122,267 at depth 8).
  run mg-calib-g1-d8 600 $MG --groups 1 --ride none --mark-always --depth 8
  run mg-any-mpt1-d9 1800 $MG --groups 2 --mpt 1 --ride any --depth 9
  run mg-any-mpt1-close-d9 1800 $MG --groups 2 --mpt 1 --ride any --close-marking --depth 9
  run mg-none-mpt1-close-d9 1800 $MG --groups 2 --mpt 1 --ride none --close-marking --depth 9
  run mg-own-mpt1-close-d9 1800 $MG --groups 2 --mpt 1 --ride own --close-marking --depth 9
  run mg-none-mpt1-close-switch-d9 2400 $MG --groups 2 --mpt 1 --ride none --close-marking --switch-marking --depth 9 --limit 12000000
  run mg-none-mpt1-close-newest-d9 1800 $MG --groups 2 --mpt 1 --ride none --close-marking --copy-reading newest --depth 9
  ;;
marks3)
  run mg3-any-mpt1-close-d8 1800 $MG --groups 3 --mpt 1 --ride any --close-marking --depth 8
  run mg3-none-mpt1-close-d8 1800 $MG --groups 3 --mpt 1 --ride none --close-marking --depth 8
  run mg3-own-mpt1-close-d8 1800 $MG --groups 3 --mpt 1 --ride own --close-marking --depth 8
  run mg3-none-mpt1-close-switch-d8 2400 $MG --groups 3 --mpt 1 --ride none --close-marking --switch-marking --depth 8 --limit 12000000
  ;;
lead5)
  # The forced schedules under each barrier mode, both checkers: the owner's rule (`fplus`) and
  # revision 0's (`timeout`, `heard`, `fplus_heard`) lose v0; D6's rule (`round`, `round_heard`)
  # is not enabled at the election.
  C=${CHECK:?the checker binary}
  hold_wait
  {
    for b in none candidate all; do
      echo "##### barrier $b"
      for m in fplus timeout round; do both $m "--voters 5 --maxos 3 --maxt 4 --barrier $b" lead5; done
      for m in heard fplus_heard round round_heard; do both $m "--voters 5 --maxos 3 --maxt 4 --barrier $b" heard5l; done
      for m in fplus round; do both $m "--voters 5 --maxos 3 --maxt 4 --barrier $b" lead5_loss; done
    done
  } > "$out/schedules-lead5.out" 2>&1
  ;;
lead5search)
  # From lead5's step 9 (2 about to re-learn v0 from 0), the owner's rule finds the loss and D6's
  # does not, under the candidate's barrier and under every barrier.
  C=${CHECK:?the checker binary}
  for b in candidate all; do
    for m in fplus round; do
      run cc-lead5-9-$m-$b-d6 1800 "$C" $m --voters 5 --maxos 3 --maxt 4 --barrier $b --schedule "$S/lead5.txt" --from-step 9 --depth 6
    done
  done
  ;;
esac
done
wait
