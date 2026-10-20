#!/bin/bash
# Revision 8's runs (DM-R48 to DM-R50, DM-R52's model label): the architect's rewind and cycle
# probes from the re-check of revision 7, ported (vouch_model_rewind.py, vouch_model_cycle.py),
# each of the architect's configurations reproduced, then this revision's additions (a repair that
# restores an image's plants and cuts its standing back; a group born under sampling whose one
# switch grounds only images not resting on the leader alone), and switch_model.py's relabeled
# failures with the architect's two mutants of the computation at p. Each run at nice 19 under
# run_capped.sh's wall-clock and resident-memory caps, at most JOBS at once (default 2, never above
# 4), and each launch only when `quiet check` exits 0 or 2; exit 1 waits.
# Usage: runs_r8.sh OUTDIR
set -u
here=$(cd "$(dirname "$0")" && pwd)
out=$1
mkdir -p "$out"
jobs_max=${JOBS:-2}
[ "$jobs_max" -gt 4 ] && jobs_max=4
C="$here/run_capped.sh"
PY="/usr/local/bin/python3 -I"
RW="$here/vouch_model_rewind.py"
CY="$here/vouch_model_cycle.py"
SW="$here/switch_model.py"
hold_wait() {
  while true; do
    quiet check >/dev/null 2>&1; rc=$?
    echo "$(date -u +%H:%M:%S) quiet check $rc" >> "$out/quiet.log"
    [ "$rc" -ne 1 ] && return
    quiet wait --light >/dev/null 2>&1 || sleep 60
  done
}
run() {
  local name=$1 secs=$2 mb=$3; shift 3
  while [ "$(jobs -rp | wc -l)" -ge "$jobs_max" ]; do sleep 2; done
  hold_wait
  "$C" "$out/$name.out" "$secs" "$mb" "$@" &
}

# DM-R48, the architect's configurations: the interval kept across D6's replacement loses; cut back
# to the repaired image, it holds.
run rw-kept-e3-d13 600 1800 $PY "$RW" --interval kept --maxe 3 --depth 13
run rw-clipped-e2-d12 1200 1800 $PY "$RW" --interval clipped --maxe 2 --depth 12
run rw-clipped-e3-d11 1200 1800 $PY "$RW" --interval clipped --maxe 3 --depth 11
# With `clean` entries, so a repair can restore a plant its image held: the interval cut back but the
# standing kept loses; the interval kept loses as before; both cut back hold.
run rw-clean-standing_kept-e3-d13 1200 1800 $PY "$RW" --interval clipped --clean --standing kept --maxe 3 --depth 13
run rw-clean-interval_kept-e3-d13 1200 1800 $PY "$RW" --interval kept --clean --standing clipped --maxe 3 --depth 13
run rw-clean-fix-e2-d11 1800 1800 $PY "$RW" --interval clipped --clean --standing clipped --maxe 2 --depth 11
run rw-clean-fix-e3-d9 1800 1800 $PY "$RW" --interval clipped --clean --standing clipped --maxe 3 --depth 9

# DM-R49, the architect's configurations (a group born under `install`, the leader writing `down`
# and `up` at will).
run cy-up_written-yes_written-e3-d13 600 1800 $PY "$CY" --cycle --up-rule written --yes-rule written --maxe 3 --depth 13
run cy-up_written-yes_written-nomiss-e3-d13 600 1800 $PY "$CY" --cycle --up-rule written --yes-rule written --nomiss --maxe 3 --depth 13
run cy-up_first-yes_written-e3-d13 600 1800 $PY "$CY" --cycle --up-rule first --yes-rule written --maxe 3 --depth 13
run cy-up_first-yes_checked-e3-d8 1200 1800 $PY "$CY" --cycle --up-rule first --yes-rule checked --maxe 3 --depth 8
# DM-R49 and DM-R50 in a group born under `sample`: the fix, then each rule removed as its mutant.
run cyb-fix-e3-d9 1800 1800 $PY "$CY" --cycle --born sample --up-rule first --yes-rule checked --maxe 3 --depth 9
run cyb-switch_ground_all-e3-d13 600 1800 $PY "$CY" --cycle --born sample --up-rule first --yes-rule checked --switch-ground all --maxe 3 --depth 13
run cyb-up_written-e4-d9 1200 1800 $PY "$CY" --cycle --born sample --up-rule written --yes-rule checked --maxe 4 --depth 9 --cap 1000000
run cyb-yes_written-e3-d13 600 1800 $PY "$CY" --cycle --born sample --up-rule first --yes-rule written --maxe 3 --depth 13

# DM-R52's label: the strict readings unchanged, each mutant labeled, the computation's mutants.
run sw-switch-o6-k2 3600 500 $PY "$SW" --scenario switch --ops 6 --keys 2 --maxsegs 3
run sw-switch-o5-k3 3600 500 $PY "$SW" --scenario switch --ops 5 --keys 3 --maxsegs 3
run sw-switch-o5-k2-s4 3600 500 $PY "$SW" --scenario switch --ops 5 --keys 2 --maxsegs 4
run sw-catchup-o6-k2 3600 500 $PY "$SW" --scenario catchup --ops 6 --keys 2 --maxsegs 3
run sw-catchup-o5-k3 3600 500 $PY "$SW" --scenario catchup --ops 5 --keys 3 --maxsegs 3
for m in inclusive inflight-open no-expunge expunge-held-only merge-held no-ceiling; do
  run sw-switch-o5-$m 600 500 $PY "$SW" --scenario switch --ops 5 --keys 2 --maxsegs 3 --$m
done
for m in merge-held no-ceiling; do
  run sw-catchup-o5-$m 600 500 $PY "$SW" --scenario catchup --ops 5 --keys 2 --maxsegs 3 --$m
done
for m in nosub noback; do
  run sw-switch-o5-atp_$m 600 500 $PY "$SW" --scenario switch --ops 5 --keys 2 --maxsegs 3 --atp $m
done
wait

# Last and alone: the two configurations whose every reachable state with two entries is the
# largest (the architect's took 1.9 GB).
jobs_max=1
run cyb-fix-e2-all 2400 2200 $PY "$CY" --cycle --born sample --up-rule first --yes-rule checked --maxe 2 --depth 60 --cap 2500000
wait
run cy-up_first-yes_checked-e2-all 2400 2200 $PY "$CY" --cycle --up-rule first --yes-rule checked --maxe 2 --depth 60 --cap 3000000
wait

# Added after the first pass: the born-sample fix at four entries, the bound at which the later-`up`
# mutant first loses (8 steps), to depth 8 (depth 9 passes the 1.5-million-state cap).
run cyb-fix-e4-d8 2400 1800 $PY "$CY" --cycle --born sample --up-rule first --yes-rule checked --maxe 4 --depth 8 --cap 1500000
wait
