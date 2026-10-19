#!/bin/bash
# Revision 2's model runs (DM-R19, DM-R20): each under run_bounded.sh's wall-clock cap, at nice 19,
# at most MAXJOBS (4) at once. Usage: runs_r2.sh OUTDIR [GROUP...]; groups below.
# Build first: c++ -std=c++20 -O2 -o forgetful_check forgetful_check.cc, and pass it as CHECK.
# Revision 3 made D4's vote barrier the default (DM-R28); --barrier none reproduces these runs.
set -u
here=${MODEL_DIR:-$(cd "$(dirname "$0")" && pwd)}
out=$1; shift
C=${CHECK:?the checker binary}; S="$here/schedules"; R="$here/run_bounded.sh"
PY="/usr/local/bin/python3 -I $here/forgetful_model.py --barrier none"
SIX="--voters 6 --base 0,1,2,3,4,5 --script remove:2 --fmax 2 --maxos 4 --maxt 5"
GROW="--voters 6 --base 0,1,2,3,4 --script add:5,remove:2 --fmax 2 --maxos 5 --maxt 5"
RF3="--voters 4 --base 0,1,2 --script add:3,remove:1 --fmax 1"

jobs_wait() { while [ "$(jobs -rp | wc -l)" -ge "${MAXJOBS:-4}" ]; do sleep 5; done; }
run() { local name=$1 cap=$2; shift 2; jobs_wait; "$R" "$out/$name.out" "$cap" "$@" & }

for group in "$@"; do
case $group in
schedules)
  # Forced schedules, both checkers (each a few seconds).
  {
    for s in restart3 restart3p; do for r in text persist; do $PY round --voters 3 --maxt 2 --restart $r --schedule $s; done; done
    for r in text persist; do $PY round --voters 3 --maxt 2 --restart $r --torn --maxr 2 --schedule torn3; done
    for count in f1 major; do
      $PY round $SIX --count $count --schedule six_loss --final
      $PY round $GROW --count $count --schedule grow6_loss --final
    done
  } > "$out/schedules-py.out" 2>&1
  {
    for s in restart3 restart3p; do for r in text persist; do echo "# $s --restart $r"; "$C" round --barrier none --voters 3 --maxt 2 --restart $r --schedule "$S/$s.txt"; done; done
    for r in text persist; do echo "# torn3 --restart $r"; "$C" round --barrier none --voters 3 --maxt 2 --restart $r --torn --maxr 2 --schedule "$S/torn3.txt"; done
    for count in f1 major; do
      for s in six six_loss; do echo "# $s --count $count"; "$C" round --barrier none $SIX --count $count --schedule "$S/$s.txt"; done
      for s in grow6 grow6_loss; do echo "# $s --count $count"; "$C" round --barrier none $GROW --count $count --schedule "$S/$s.txt"; done
    done
  } > "$out/schedules-cc.out" 2>&1
  ;;
restart)
  # DM-R19: revision 1's restart rule finds the loss; the persisted mark holds.
  run cc-restart-text-n3-d8 600 "$C" round --barrier none --voters 3 --maxt 2 --restart text --depth 8
  run cc-torn-text-n3-os0-d8 600 "$C" round --barrier none --voters 3 --maxos 0 --maxt 2 --restart text --torn --maxr 2 --depth 8
  run cc-torn-persist-n3-os0-d10 1800 "$C" round --barrier none --voters 3 --maxos 0 --maxt 3 --restart persist --torn --maxr 2 --depth 10
  run cc-persist-torn-retry-n3-p2-d11 3600 "$C" round --barrier none --voters 3 --maxos 3 --maxp 2 --maxt 4 --restart persist --torn --maxr 2 --retry --symmetry --key64 --table 28 --depth 11
  run cc-persist-torn-retry-n5-d7 3600 "$C" round --barrier none --voters 5 --maxos 3 --maxt 4 --restart persist --torn --maxr 1 --retry --symmetry --key64 --table 27 --depth 7
  ;;
restart-deep)
  run cc-persist-torn-retry-n3-p2-d12 5400 "$C" round --barrier none --voters 3 --maxos 3 --maxp 2 --maxt 4 --restart persist --torn --maxr 2 --retry --symmetry --key64 --table 28 --depth 12
  run cc-persist-torn-retry-n5-d8 5400 "$C" round --barrier none --voters 5 --maxos 3 --maxt 4 --restart persist --torn --maxr 1 --retry --symmetry --key64 --table 28 --depth 8
  ;;
members)
  # DM-R20: searches from the forced schedules' prefixes, both counts, no symmetry (the
  # prefixes are asymmetric), 64-bit keys. Calibrated pairs first: f + 1 must find the loss.
  for count in f1 major; do
    run cc-six38-$count-d5 2400 "$C" round --barrier none $SIX --count $count --schedule "$S/six.txt" --from-step 38 --depth 5 --key64 --table 27
    run cc-grow43-$count-d5 2400 "$C" round --barrier none $GROW --count $count --schedule "$S/grow6.txt" --from-step 43 --depth 5 --key64 --table 27
  done
  for count in f1 major; do
    run cc-six33-$count-d5 3600 "$C" round --barrier none $SIX --count $count --schedule "$S/six.txt" --from-step 33 --depth 5 --key64 --table 27
    run cc-grow38-$count-d5 3600 "$C" round --barrier none $GROW --count $count --schedule "$S/grow6.txt" --from-step 38 --depth 5 --key64 --table 27
  done
  ;;
members6)
  # The calibrated pair one step earlier: f + 1's loss is six steps from `six`'s step 37.
  for count in f1 major; do
    run cc-six37-$count-d6 3600 "$C" round --barrier none $SIX --count $count --schedule "$S/six.txt" --from-step 37 --depth 6 --key64 --table 28
  done
  ;;
rf3)
  # Track C's rf = 3 window (four voters): exhaustive from the founding state, both counts.
  for count in f1 major; do
    run cc-rf3win-$count-d10 3600 "$C" round --barrier none $RF3 --maxos 2 --maxt 3 --count $count --symmetry --key64 --table 27 --depth 10
  done
  ;;
rf3win)
  # The rf = 3 window from a prefix with the promotion synced everywhere and v0 acknowledged at
  # memory: both counts, as deep as the host's memory allows.
  for count in f1 major; do
    run cc-rf3win24-$count-d8 3600 "$C" round --barrier none $RF3 --maxos 2 --maxt 4 --count $count --schedule "$S/rf3win.txt" --from-step 24 --depth 8 --key64 --table 27
  done
  ;;
rf3win9)
  for count in f1 major; do
    run cc-rf3win24-$count-d9 5400 "$C" round --barrier none $RF3 --maxos 2 --maxt 4 --count $count --schedule "$S/rf3win.txt" --from-step 24 --depth 9 --key64 --table 29
  done
  ;;
walks)
  # Calibration only: kind-first walks from `six`'s step 33 (ten steps from f + 1's loss) find
  # nothing under f + 1 either, so walks are no evidence at six voters.
  run cal-walks-six33-f1 1800 "$C" round --barrier none $SIX --count f1 --schedule "$S/six.txt" --from-step 33 --walks 2000000 --by-kind --seed 7 --depth 14
  ;;
esac
done
wait
