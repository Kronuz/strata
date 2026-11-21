#!/usr/bin/env bash
# Warm-heap exploration (docs/perf/warm-heap.md).
#   run.sh build OUT COUNT        catalog_tiers(_count) and a 4-segment fixture d1000s4 of COUNT clones
#   run.sh pieces OUT TIER [COUNT] live heap per index by allocation site, atos-named
set -euo pipefail
cmd=$1 out=$2
here="$(cd "$(dirname "$0")/../.." && pwd)"
case "$cmd" in
build)
	count=$3
	mkdir -p "$out/bin"
	cmake -G Ninja -S "$here/bench/catalog_tiers" -B "$out/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build "$out/build" -j4 --target catalog_tiers catalog_tiers_count tiers_idle_index >/dev/null
	cp "$out/build/catalog_tiers" "$out/build/catalog_tiers_count" "$out/bin/"
	cp "$out/build/tiers_idle_index" "$out/bin/idle_index"
	git -C "$here" rev-parse HEAD >"$out/bin/commit"
	for f in d1000s4:1000:4 d1000s10:1000:10 d1000s1:1000:1; do
		IFS=: read -r name docs segs <<<"$f"
		rm -rf "${out:?}/$name"
		"$out/bin/idle_index" build "$out/$name" "$count" "$docs" "$segs" | grep -E "^template" || true
	done
	;;
pieces)
	tier=$3 count=${4:-1000} fx=${5:-d1000s4}
	bin="$out/bin/catalog_tiers_count"
	echo "commit $(cat "$out/bin/commit") $fx $tier $count"
	"$bin" tier "$out/$fx" "$count" "$tier" | grep -E "^heap live"
	"$bin" sites "$out/$fx" "$count" "$tier" >"$out/sites-$tier.txt"
	grep -vE "^SITE" "$out/sites-$tier.txt"
	slide=$(sed -n 's/^slide //p' "$out/sites-$tier.txt")
	load=$(printf '0x%x' $((0x100000000 + ${slide:-0})))
	grep -E "^SITE" "$out/sites-$tier.txt" | head -40 | while read -r line; do
		echo "$line" | cut -d: -f1
		# shellcheck disable=SC2046
		atos -o "$bin" -l "$load" $(echo "$line" | cut -d: -f2) | head -6 | sed 's/^/    /'
	done
	;;
esac
