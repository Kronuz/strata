#!/bin/bash
# The manifest-log design's revision-1 probes, in one run under a strict hold:
#   quiet strict mlog2 20 "manifest log r1" -- bench/manifest-log/run_r1.sh
# tail_model.py (D6's tail rule, against journal's fixtures at journal:bfad97f), t8_model.py
# (fields-and-keys T8 arithmetic) and open_probe.cc (T7's open cost, 3 rounds). Output:
# bench/manifest-log/results/<date>-r1.txt; scratch under the worktree's .scratch/mlog2.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
deps=${STRATA_DEPS:-/Users/kronuz/code/Kronuz/strata/build/_deps}
journal=${JOURNAL_REPO:-/Users/kronuz/code/Kronuz/journal}
py=${PYTHON:-/usr/local/bin/python3}
scratch="$root/.scratch/mlog2/run-$(date +%Y%m%d-%H%M%S)"
out="$here/results/$(date +%Y-%m-%d)-r1.txt"
mkdir -p "$scratch/fixtures" "$scratch/open"
{
	echo "# started $(date '+%Y-%m-%d %H:%M:%S'), host $(uname -m) $(sw_vers -productVersion 2>/dev/null || uname -r), strata $(git -C "$root" rev-parse --short HEAD)"
	git -C "$journal" archive bfad97f test/fixtures/crc32c/turn-log | tar -x -C "$scratch/fixtures"
	echo "== tail_model.py (journal fixtures at journal:bfad97f)"
	"$py" -I "$here/tail_model.py" "$scratch/fixtures/test/fixtures/crc32c/turn-log"
	echo "tail exit=$?"
	echo "== t8_model.py"
	"$py" -I "$here/t8_model.py"
	echo "t8 exit=$?"
	echo "== open_probe.cc"
	c++ -std=c++20 -O2 -I"$root/include" -I"$deps/crc32c-src/include" -I"$deps/varint-src" -I"$deps/varint-src/include" \
		-I"$deps/hashes-src" -I"$deps/char_classify-src" -I"$deps/static_string-src" -I"$deps/termdict-src" -I"$deps/fst-src" \
		-I"$deps/fst-src/include" -o "$scratch/open_probe" "$here/open_probe.cc" && "$scratch/open_probe" "$scratch/open" 3
	echo "open exit=$?"
	echo "# ended $(date '+%Y-%m-%d %H:%M:%S')"
} > "$out" 2>&1
rm -rf "$scratch"
echo "$out"
