#!/bin/bash
# Mutants of the inline write buffer record (CR1, CR2, ruling (c)): each patches a header, rebuilds
# the tests that should catch it, and must be killed. Exits nonzero if the tree fails or one survives.
# Usage: test/wbuf_mutants.sh [OUT] (default $JOB_OUT/wbuf-mutants); run from the repository root of a disposable checkout.
set -u
OUT=${1:-${JOB_OUT:-.scratch}/wbuf-mutants}
mkdir -p "$OUT"
LLVM=${LLVM:-/usr/local/opt/llvm/bin}
IX=include/strata/core/index.hh WR=include/strata/core/writer.hh WB=include/strata/core/write_buffer.hh
for f in $IX $WR $WB; do cp "$f" "$OUT/$(basename "$f").good"; done
restore() { for f in $IX $WR $WB; do cp "$OUT/$(basename "$f").good" "$f"; done; }
TESTS="write_buffer_test write_buffer_reads_test fields_test background_merge_test defer_io_test"
cmake -S . -B "$OUT/rel" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$OUT/cmake-rel.log" 2>&1 || { echo "cmake rel failed"; exit 1; }
cmake -S . -B "$OUT/asan" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=$LLVM/clang -DCMAKE_CXX_COMPILER=$LLVM/clang++ \
	"-DCMAKE_CXX_FLAGS=-O1 -g -DNDEBUG -fsanitize=address,undefined" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" > "$OUT/cmake-asan.log" 2>&1 || { echo "cmake asan failed"; exit 1; }
fail=0
# run NAME DIR TEST: builds TEST in DIR, runs it; leaves its output in $OUT/NAME-TEST.log, returns its exit code (255 = no build).
run() {
	local log="$OUT/$1-$3.log"
	ninja -C "$OUT/$2" -j4 "$3" > "$log" 2>&1 || return 255
	"$OUT/$2/$3" >> "$log" 2>&1
}
# killed NAME DIR TEST PATTERN: the mutant fails TEST (or its build) with PATTERN in its log.
killed() {
	run "$1" "$2" "$3"; local rc=$?
	if [ $rc -ne 0 ] && grep -q -E "$4" "$OUT/$1-$3.log"; then echo "$1 $3: killed (rc=$rc)"; else echo "$1 $3: SURVIVED (rc=$rc)"; fail=1; fi
}
patch() { python3 - "$@" <<'P'
import sys
p, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read(); assert s.count(a) == 1, (p, a); open(p, 'w').write(s.replace(a, b))
P
}
for t in $TESTS; do run base rel $t && echo "base $t: pass" || { echo "base $t: FAIL"; fail=1; }; done
run base asan write_buffer_test && echo "base asan write_buffer_test: pass" || { echo "base asan write_buffer_test: FAIL"; fail=1; }

# m1 (CR1): emplace inlines past a chunk; append_n's bound throws in Release.
patch $IX "if (inline_ok && n <= wbuf::min_chunk) {" "if (inline_ok) {"
killed m1 rel write_buffer_test "long key [0-9]+: strata: a write buffer record past a chunk"
killed m1 rel write_buffer_reads_test "a write buffer record past a chunk"
# m2 (CR1): m1 without append_n's bound, the pre-fix Release behaviour: ASan sees the overflow.
patch $WB "if (bytes % 8 != 0 || bytes < stride || bytes > min_chunk) [[unlikely]]" "if (bytes % 8 != 0 || bytes < stride) [[unlikely]]"
killed m2 asan write_buffer_test "heap-buffer-overflow"
restore
# m4 (CR2): every layout inline, static_asserts kept: fields_test does not compile.
patch $IX "return opts_in || (std::is_same_v<typename E::part, stored_part> && basic_segment_writer<Layout>::template views_stored<E>());" "return true;"
killed m4 rel fields_test "add_stored_view would skip a plugin's own add"
# m5 (CR2): m4 with both static_asserts removed: own_builder_parts sees no builder add.
patch $WR 'static_assert((views_stored<Entries>() && ...), "strata: add_stored_view would skip a plugin'"'"'s own add");' ""
patch $IX 'static_assert(!opts_in || basic_segment_writer<Layout>::template views_stored<E>(), "strata: a plugin with inline_part derives from stored_plugin and keeps its builder");' ""
killed m5 rel fields_test "fields_test.cc:[0-9]+: CHECK\(\(extra_plugin::builder::seen"
restore

# R (ruling (c)): the doubling chunks restored in append_n and room_for.
patch $WB "const std::size_t n = next_chunk(bytes_);" "const std::size_t n = count_ ? std::min(max_chunk, 2 * entry(count_ - 1).size) : min_chunk;"
patch $WB "		std::size_t have = 0, i = count_;
		std::uint64_t total = bytes_;
		if (count_) {
			const chunk& t = entry(count_ - 1);
			have = (t.size - t.used) / bytes;
		}
		while (have < n) {
			if (i >= limit_) { return false; }
			const std::size_t size = next_chunk(total);" "		std::size_t have = 0, i = count_, size = 0;
		std::uint64_t total = bytes_;
		if (count_) {
			const chunk& t = entry(count_ - 1);
			have = (t.size - t.used) / bytes;
			size = t.size;
		}
		while (have < n) {
			if (i >= limit_) { return false; }
			size = i ? std::min(max_chunk, 2 * size) : min_chunk;"
killed R rel write_buffer_test "write_buffer_test.cc:[0-9]+: CHECK\(exact && opened > 24\)"
killed R rel write_buffer_reads_test "write_buffer_reads_test.cc:[0-9]+: CHECK\(peak"
killed R rel background_merge_test "background_merge_test.cc:[0-9]+: CHECK\(gated"
killed R rel defer_io_test "defer_io_test.cc:[0-9]+: CHECK\(ix.digest\(\) == 0x6cc0ff118bf776d3ull\)"
restore
# M1: a chunk a 16th of the lane.
patch $WB "total / 32 / min_chunk" "total / 16 / min_chunk"
killed M1 rel write_buffer_test "write_buffer_test.cc:[0-9]+: CHECK\(bounded"
restore
# M2: room_for simulates doubling while append_n uses next_chunk.
patch $WB "const std::size_t size = next_chunk(total);" "const std::size_t size = i ? std::min<std::size_t>(max_chunk, 2 * next_chunk(total - 1)) : min_chunk;"
killed M2 rel write_buffer_test "write_buffer_test.cc:[0-9]+: CHECK\(exact\)"
restore
# M3: chunks stay 1 KiB: the lane's directory fills.
patch $WB "const std::size_t n = next_chunk(bytes_);" "const std::size_t n = min_chunk;"
killed M3 rel write_buffer_test "chunk directory is full|Abort|abort"
restore

for f in $IX $WR $WB; do cmp -s "$f" "$OUT/$(basename "$f").good" || { echo "not restored: $f"; fail=1; }; done
[ $fail -eq 0 ] && echo "all mutants killed" || echo "FAILED"
exit $fail
