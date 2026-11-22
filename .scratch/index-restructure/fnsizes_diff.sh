#!/usr/bin/env bash
# Builds strata's test binaries Release+LTO at the base ($JOB_REF_BASE) and here, prints each
# binary's fnsizes table diff (sorted by name), and fails when any function's size differs.
set -euo pipefail
here=$PWD
base=${JOB_REF_BASE:?}
fn=/Users/kronuz/code/Kronuz/cpp-libraries/tools/fnsizes.sh
out=$here/.scratch/index-restructure/fnsizes
rm -rf "$out" && mkdir -p "$out"
cfg=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DFETCHCONTENT_BASE_DIR="$here/.scratch/index-restructure/fc")
for side in base head; do
	src=$here
	[ "$side" = base ] && src=$base
	cmake -S "$src" -B "$here/.scratch/index-restructure/build-$side" "${cfg[@]}" >"$out/$side.configure.log"
	cmake --build "$here/.scratch/index-restructure/build-$side" -j4 >"$out/$side.build.log"
done
changed=0 n=0
for bin in $(cd "$here/.scratch/index-restructure/build-head" && find . -type f -perm -u+x -name '*_test' | sort); do
	b="$here/.scratch/index-restructure/build-base/$bin" h="$here/.scratch/index-restructure/build-head/$bin"
	[ -f "$b" ] || { echo "only at head: $bin"; changed=1; continue; }
	n=$((n + 1))
	name=$(basename "$bin")
	"$fn" "$b" | sort -t$'\t' -k3 >"$out/$name.base"
	"$fn" "$h" | sort -t$'\t' -k3 >"$out/$name.head"
	if ! diff "$out/$name.base" "$out/$name.head" >"$out/$name.diff"; then
		changed=1
		echo "== $name: $(grep -c '^<' "$out/$name.diff") rows differ"
		head -40 "$out/$name.diff"
	fi
done
echo "binaries compared: $n"
[ "$changed" = 0 ] && echo "no function changed size"
exit "$changed"
