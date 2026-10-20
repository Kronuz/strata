#!/bin/bash
# Revision 7's runs (DM-R40, DM-R41, DM-R42; DMS-24 item 2): the vouch model widened (every member
# may catch up; a catch-up grounds only from a grounded base; the switch to `verify: install`), the
# architect's vouch_model_r6.py beside it, and the switch model ported from the architect's, its
# strict readings and every mutant. Each run at nice 19 under run_capped.sh's wall-clock and
# resident-memory caps, at most JOBS at once (default 2, never above 4), and each launch only when
# `quiet check` exits 0 or 2; exit 1 waits.
# Usage: ARCH=path/to/review-durability/r6/model runs_r7.sh OUTDIR
set -u
here=$(cd "$(dirname "$0")" && pwd)
out=$1
mkdir -p "$out"
jobs_max=${JOBS:-2}
[ "$jobs_max" -gt 4 ] && jobs_max=4
C="$here/run_capped.sh"
PY="/usr/local/bin/python3 -I"
VM="$here/vouch_model.py"
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

# DM-R40: revision 7 (catch-up grounds only from a grounded base), and revision 6 as its mutant.
run v7-base-e2-all 1800 2000 $PY "$VM" --catchup base --maxe 2 --depth 60
run v7-base-e3-d13 1800 2000 $PY "$VM" --catchup base --maxe 3 --depth 13
run v7-any-e3-d13 600 2000 $PY "$VM" --catchup any --maxe 3 --depth 13
# Revision 5's controls under revision 7's catch-up rule.
for a in position entry_ungrounded position_grounded; do
  run v7-ctl-$a-e3-d13 600 2000 $PY "$VM" --answer $a --maxe 3 --depth 13
done
# DM-R41: option (a), with and without a plant the samples missed; (b); (a) with revision 6's catch-up.
run v7-switch-a-plant0-e2-all 1800 2000 $PY "$VM" --switch grounded --plant0 --maxe 2 --depth 60
run v7-switch-a-plant0-e3-d12 1800 2000 $PY "$VM" --switch grounded --plant0 --maxe 3 --depth 12
run v7-switch-a-e2-all 1800 2000 $PY "$VM" --switch grounded --maxe 2 --depth 60
run v7-switch-b-plant0-e2-all 1800 2000 $PY "$VM" --switch unvouched --plant0 --maxe 2 --depth 60
run v7-switch-a-plant0-any-e3-d13 600 2000 $PY "$VM" --switch grounded --plant0 --catchup any --maxe 3 --depth 13
# The architect's model, unmodified (read from its review directory).
if [ -n "${ARCH:-}" ]; then
  run arch-vr6-entry-catchup-e3-d13 600 2000 $PY "$ARCH/vouch_model_r6.py" --rule entry --catchup-new --maxe 3 --depth 13 --cap 1500000
  run arch-vr6-basegrounded-catchup-e2-all 1800 2000 $PY "$ARCH/vouch_model_r6.py" --rule entry_basegrounded --catchup-new --maxe 2 --depth 60 --cap 1500000
  run arch-vr6-basegrounded-catchup-e3-d12 1800 2000 $PY "$ARCH/vouch_model_r6.py" --rule entry_basegrounded --catchup-new --maxe 3 --depth 12 --cap 1500000
  for m in inclusive inflight-open; do
    run arch-sw-switch-o5-$m 600 2000 $PY "$ARCH/switch_model.py" --scenario switch --ops 5 --keys 2 --maxsegs 3 --$m
  done
fi
# DM-R42: the strict readings, both scenarios; then each reading and mutant.
run sw-switch-o6-k2 3600 2000 $PY "$SW" --scenario switch --ops 6 --keys 2 --maxsegs 3
run sw-switch-o5-k3 3600 2000 $PY "$SW" --scenario switch --ops 5 --keys 3 --maxsegs 3
run sw-switch-o5-k2-s4 3600 2000 $PY "$SW" --scenario switch --ops 5 --keys 2 --maxsegs 4
run sw-catchup-o6-k2 3600 2000 $PY "$SW" --scenario catchup --ops 6 --keys 2 --maxsegs 3
run sw-catchup-o5-k3 3600 2000 $PY "$SW" --scenario catchup --ops 5 --keys 3 --maxsegs 3
for m in inclusive inflight-open no-expunge expunge-held-only merge-held no-ceiling; do
  run sw-switch-o5-$m 600 2000 $PY "$SW" --scenario switch --ops 5 --keys 2 --maxsegs 3 --$m
done
for m in merge-held no-ceiling; do
  run sw-catchup-o5-$m 600 2000 $PY "$SW" --scenario catchup --ops 5 --keys 2 --maxsegs 3 --$m
done
wait
