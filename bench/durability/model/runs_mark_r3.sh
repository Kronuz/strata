#!/bin/bash
# DM-R24's journal-level runs (mark_model.py): revision 3's rules against the architect's 6-step
# copy-forward loss, the open record's lifetime, and the controls. Each run is under run_bounded.sh's
# cap, at nice 19, one thread, one at a time (the Python tables hold every state in memory).
# Usage: runs_mark_r3.sh OUTDIR GROUP...
set -u
here=${MODEL_DIR:-$(cd "$(dirname "$0")" && pwd)}
out=$1; shift
R="$here/run_bounded.sh"
M="/usr/local/bin/python3 -I $here/mark_model.py"
run() { local name=$1 cap=$2; shift 2; "$R" "$out/$name.out" "$cap" "$@"; }

for group in "$@"; do
case $group in
controls)
  # Revision 2's rule (c) as written reproduces the architect's loss; "newest" by journal order
  # loses too; the open record reclaimable within its epoch costs precision (unclean read) or,
  # read as the same boot, safety.
  run mark-literal-copy-d10 1800 $M --variant literal --open-life copy --depth 10
  run mark-jorder-copy-d10 1800 $M --variant firstturn_jorder --open-life copy --depth 10
  run mark-firstturn-none-d10 1800 $M --variant firstturn --open-life none --depth 10
  run mark-firstturn-same-d10 1800 $M --variant firstturn --open-life same --depth 10
  ;;
design)
  # Revision 3's rules: the first-turn mark and the open record copied forward.
  run mark-firstturn-copy-d10 1800 $M --variant firstturn --open-life copy --depth 10
  run mark-firstturn-copy-v3-d9 1800 $M --variant firstturn --open-life copy --depth 9 --maxval 3
  run mark-firstturn-copy-s3-d10 3600 $M --variant firstturn --open-life copy --depth 10 --switches 3
  run mark-orig-copy-d10 1800 $M --variant orig --open-life copy --depth 10
  ;;
deep)
  # Depth 11 with copy-forward on, as the re-check asks; 64-bit state keys to fit the host.
  run mark-firstturn-copy-d11 5400 $M --variant firstturn --open-life copy --depth 11 --limit 12000000 --key64
  run mark-orig-copy-d11 5400 $M --variant orig --open-life copy --depth 11 --limit 12000000 --key64
  ;;
esac
done
