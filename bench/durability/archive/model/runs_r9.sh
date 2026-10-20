#!/bin/bash
# Revision 9's runs (DMS-25): the security reviewer's deferred first-switch grounding, ported into
# vouch_model_cycle.py (--switch-ground deferred), its configurations reproduced, its mutants, a
# regression pass over revision 8's default (eligible) rule, and switch_consumed.py's schedules under
# both rules. Each run at nice 19 under run_capped.sh's wall-clock and resident-memory caps, at most
# JOBS at once (default 2, never above 2 here), and each launch only when `quiet check` exits 0 or 2;
# exit 1 waits.
# Usage: runs_r9.sh OUTDIR
set -u
here=$(cd "$(dirname "$0")" && pwd)
out=$1
mkdir -p "$out"
jobs_max=${JOBS:-2}
[ "$jobs_max" -gt 2 ] && jobs_max=2
C="$here/run_capped.sh"
PY="/usr/local/bin/python3 -I"
CY="$here/vouch_model_cycle.py"
SC="$here/switch_consumed.py"
BS="--cycle --born sample --up-rule first --yes-rule checked"
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

# The schedules (small): revision 8's rule spends the switch, the deferred rule grounds at the return.
run sc-eligible 600 800 $PY "$SC" --switch-ground eligible --maxe 4 --depth 10
run sc-deferred 600 800 $PY "$SC" --switch-ground deferred --maxe 4 --depth 10
wait

# The fix: every reachable state with two entries, then depth 9 with three and 8 with four.
run cyb-deferred-e3-d9 1800 1800 $PY "$CY" $BS --switch-ground deferred --maxe 3 --depth 9
run cyb-deferred-e4-d8 2400 1800 $PY "$CY" $BS --switch-ground deferred --maxe 4 --depth 8 --cap 1500000
run cyb-deferred_any-e3-d9 1800 1800 $PY "$CY" $BS --switch-ground deferred_any --maxe 3 --depth 9
wait
# The mutants: `all` (the switch grounds a leader-alone image), the deferral without the leader-alone
# exclusion, and the other two rules removed under the deferred rule.
run cyb-all-e3-d13 600 1800 $PY "$CY" $BS --switch-ground all --maxe 3 --depth 13
run cyb-deferred_lw-e3-d13 600 1800 $PY "$CY" $BS --switch-ground deferred_lw --maxe 3 --depth 13
wait
run cyb-deferred-up_written-e4-d9 1200 1800 $PY "$CY" --cycle --born sample --up-rule written --yes-rule checked --switch-ground deferred --maxe 4 --depth 9 --cap 1000000
run cyb-deferred-yes_written-e3-d13 600 1800 $PY "$CY" --cycle --born sample --up-rule first --yes-rule written --switch-ground deferred --maxe 3 --depth 13
wait
# Regression: revision 8's default rule is unchanged by the port (results/r8 equals these).
run cyb-eligible-e3-d9 1800 1800 $PY "$CY" $BS --maxe 3 --depth 9
run cy-born_install-e3-d8 1200 1800 $PY "$CY" --cycle --up-rule first --yes-rule checked --maxe 3 --depth 8
wait

# Last and alone: every reachable state with two entries (the reviewer's run took 891 MB).
jobs_max=1
run cyb-deferred-e2-all 2400 2200 $PY "$CY" $BS --switch-ground deferred --maxe 2 --depth 60 --cap 2500000
wait
run cyb-eligible-e2-all 2400 2200 $PY "$CY" $BS --maxe 2 --depth 60 --cap 2500000
wait
