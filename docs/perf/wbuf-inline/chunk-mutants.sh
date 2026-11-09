#!/bin/bash
# Revert check and mutants for next_chunk: each patches write_buffer.hh, rebuilds the tests, runs them.
W=/Users/kronuz/code/Kronuz/strata-wt/wbuf-inline; H=$W/include/strata/core/write_buffer.hh; G=$W/.scratch/chunk/write_buffer.hh.good
run() {
  echo "== $1"
  ninja -C $W/.scratch/b-rel -j4 write_buffer_test write_buffer_reads_test background_merge_test defer_io_test > /dev/null || { echo build failed; return; }
  for t in write_buffer_test write_buffer_reads_test background_merge_test defer_io_test; do
    out=$($W/.scratch/b-rel/$t 2>&1); echo "$t rc=$? $(echo "$out" | grep -c 'CHECK(') checks failed"; echo "$out" | grep 'CHECK(' | sed 's/^.*test\//  /' | head -4
  done
}
# R: the doubling chunks (the branch before this change), in append and room_for.
python3 - $H <<'P'
import sys; p=sys.argv[1]; s=open(p).read()
s=s.replace("const std::size_t n = next_chunk(bytes_);","const std::size_t n = count_ ? std::min(max_chunk, 2 * entry(count_ - 1).size) : min_chunk;")
s=s.replace("""		std::size_t have = 0, i = count_;
		std::uint64_t total = bytes_;
		if (count_) {
			const chunk& t = entry(count_ - 1);
			have = (t.size - t.used) / bytes;
		}
		while (have < n) {
			if (i >= limit_) { return false; }
			const std::size_t size = next_chunk(total);""","""		std::size_t have = 0, i = count_, size = 0;
		std::uint64_t total = bytes_;
		if (count_) {
			const chunk& t = entry(count_ - 1);
			have = (t.size - t.used) / bytes;
			size = t.size;
		}
		while (have < n) {
			if (i >= limit_) { return false; }
			size = i ? std::min(max_chunk, 2 * size) : min_chunk;""")
open(p,'w').write(s)
P
run "R: doubling chunks"; cp $G $H
# M1: a chunk a quarter of the lane.
sed -i '' 's|total / 32 / min_chunk|total / 16 / min_chunk|' $H; run "M1: next_chunk total/16"; cp $G $H
# M2: room_for simulates doubling while append uses next_chunk.
python3 - $H <<'P'
import sys; p=sys.argv[1]; s=open(p).read()
a="const std::size_t size = next_chunk(total);"; assert a in s
s=s.replace(a,"const std::size_t size = i ? std::min<std::size_t>(max_chunk, 2 * next_chunk(total - 1)) : min_chunk;"); open(p,'w').write(s)
P
run "M2: room_for off the lane's sizes"; cp $G $H
# M3: append_n ignores what the lane holds (1 KiB chunks forever).
sed -i '' 's|const std::size_t n = next_chunk(bytes_);|const std::size_t n = min_chunk;|' $H; run "M3: chunks stay 1 KiB"; cp $G $H
cmp $G $H && echo restored
