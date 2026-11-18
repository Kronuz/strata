#!/bin/sh
# Builds idle_index at this checkout (after) and at $JOB_REF_MAIN (before) into OUT, both from this
# checkout's bench/idle_index/idle_index.cc (the harness is one; the engine differs), then COUNT
# indexes of DOCS documents under OUT/root for run.sh (docs/perf/open-sync.md).
# Usage: build.sh OUT [COUNT [DOCS]], from the checkout's root, as a light job.
set -eu
out=$1; count=${2:-1000}; docs=${3:-100}
mkdir -p "$out"
cp bench/idle_index/idle_index.cc "$JOB_REF_MAIN/bench/idle_index/idle_index.cc"
for side in after before; do
	src=.; [ "$side" = before ] && src=$JOB_REF_MAIN
	cmake -S "$src" -B "$src/build-open-sync" -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build "$src/build-open-sync" --target idle_index -j4 >/dev/null
	cp "$src/build-open-sync/idle_index" "$out/idle_index.$side"
done
rm -rf "$out/root"
"$out/idle_index.after" build "$out/root" "$count" "$docs"
