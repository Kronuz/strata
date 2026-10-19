#!/bin/bash
# Runs one model configuration at nice 19 under a wall-clock cap, recording its start, end, exit,
# and duration beside its output. Usage: run_bounded.sh OUT MAX_SECONDS cmd args...
out=$1; max=$2; shift 2
{
  echo "# start $(date -u +%Y-%m-%dT%H:%M:%SZ) cap ${max}s: $*"
  start=$(date +%s)
  nice -n 19 "$@" &
  pid=$!
  ( sleep "$max"; kill -TERM $pid 2>/dev/null && echo "# stopped at the ${max}s cap" ) &
  watcher=$!
  wait $pid; rc=$?
  kill $watcher 2>/dev/null; wait $watcher 2>/dev/null
  echo "# end $(date -u +%Y-%m-%dT%H:%M:%SZ) exit $rc after $(( $(date +%s) - start ))s"
} > "$out" 2>&1
