#!/bin/bash
# From the architect's full design review of revision 6 (r6/run_capped.sh), unchanged but this line.
# Runs one model configuration at nice 19 under a wall-clock cap and a resident-memory cap, and
# records its start, end, exit, duration, and peak resident size beside its output (OUT and
# OUT.time, from /usr/bin/time -l). Usage: run_capped.sh OUT MAX_SECONDS MAX_RSS_MB cmd args...
out=$1; max=$2; cap_mb=$3; shift 3
{
  echo "# start $(date -u +%Y-%m-%dT%H:%M:%SZ) cap ${max}s ${cap_mb} MB: $*"
  start=$(date +%s)
  /usr/bin/time -l -o "$out.time" nice -n 19 "$@" &
  tpid=$!
  stopped=""
  peak=0
  while kill -0 "$tpid" 2>/dev/null; do
    child=$(pgrep -P "$tpid" | head -1)
    if [ -n "$child" ]; then
      kb=$(ps -o rss= -p "$child" 2>/dev/null | tr -d ' ')
      if [ -n "$kb" ]; then
        [ "$kb" -gt "$peak" ] && peak=$kb
        if [ $((kb / 1024)) -gt "$cap_mb" ]; then
          kill -TERM "$child" 2>/dev/null; stopped="# stopped at the ${cap_mb} MB cap ($((kb / 1024)) MB)"; break
        fi
      fi
    fi
    if [ $(( $(date +%s) - start )) -gt "$max" ]; then
      [ -n "$child" ] && kill -TERM "$child" 2>/dev/null; stopped="# stopped at the ${max}s cap"; break
    fi
    sleep 2
  done
  wait "$tpid"; rc=$?
  [ -n "$stopped" ] && echo "$stopped"
  echo "# end $(date -u +%Y-%m-%dT%H:%M:%SZ) exit $rc after $(( $(date +%s) - start ))s; sampled peak $((peak / 1024)) MB"
} > "$out" 2>&1
