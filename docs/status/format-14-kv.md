# format-14-kv: the document layout as a key-value store (format 14)

- Task: design format 14's key-value layout for strata's core (the document layout), so the core
  can beat RocksDB as a key-value store: key-ordered value blocks, a shared decoded-block cache, a
  key-ordered scan, the buffer's sorted view, key-ordered merges. Design only to the first
  buildable step; no review ladder (PLAN, "Formats stay open until the first release"); the owner
  settles the shape from the design's "Shape for the owner", then a developer builds.
- Design: `docs/design/format-14-kv.md`, revision 0; raw prototype results and medians in
  `docs/design/format-14-kv/`; prototypes in `bench/kv/proto/`.
- Branch / worktree: `design/format-14-kv` from main `426cc59`, at
  `/Users/kronuz/code/Kronuz/strata-wt/format-14-kv`.

## State

Designed: ready for the owner to settle the shape (Q1, Q2). No review ladder for the format
(PLAN); the cache's step (D2) takes the concurrency tier's one design review before it is built.

Q1 and Q2 measured (`docs/design/format-14-kv/q1-q2.md`, decision tables at its end): split
recommended, conditioned on termdict's `cursor::seek` and a speed pass on its sequential decode;
LZ4 recommended as the default with zstd 1 on merged output as the disk preset. The owner decides.

## Done

- `0dde9d1`: ledger started.
- `6abf075`: the four micro-prototypes (`bench/kv/proto/`: blocks, cache, buffer, bufmem), their
  CMake targets outside `all`, the driver `run.py`, and the raw results.
- `bb78b18`: the design (D1 to D5, T1 to T8, Q1 and Q2, step 1 with proposed done criteria
  K1 to K7, the shape summary), the split layout's zstd rows added to the block results.
- `8c33162`: the prototypes build only with `-DSTRATA_KV_PROTO=ON`; their RSS probes read Linux
  too. Prototype builds and scratch data removed from the worktree after the runs.
- `785bf19`: Q1 and Q2's prototype `kv_proto_cached` (both layouts through the CLOCK cache at
  calibrated 60/90/99% hit rates; split with `td_cursor`, a target-starting termdict seek; codecs
  LZ4, zstd 1, mixed; 4-way merges), the layouts and cache moved to `blocks.hh` and
  `clock_cache.hh`, and the plan written before the runs.
- (this commit): the results on the Mac and the Linux gate VM (three interleaved rounds, A/A within
  4%), raw lines `q12-mac.jsonl.gz` and `q12-vm.jsonl.gz`, the two decision tables.

Measured choices (medians, three interleaved rounds, the development Mac):

- Block 4 KiB, LZ4: a get's block miss 1.5 µs against 36 µs for today's 64 KiB zstd frame; zstd 1
  at 4 KiB 4.8 µs, 16% fewer bytes than LZ4.
- Split (keys termdict + key-ordered value blocks) against inline (RocksDB's block table): gets
  within 12%, bytes within 3%, seeks 3× slower until termdict's `cursor::seek`.
- Cache: 16-way set-associative CLOCK, 1.35× an LRU's hits on one thread, 4.5× on sixteen, same
  hit rate.
- Buffer: arena log + 8-byte-slot version map + two-run sorted view, 177 B per record (today's
  document-layout buffer 494 B; RocksDB 153 B); inserts 4.0× and gets 8.4× a skiplist's.

## Next

1. The owner answers Q1 (split or inline) and Q2 (codec per tier) from `q1-q2.md`'s tables, and
   whether T7 keeps its "within 10% of format 13" clause under an LZ4 default.
2. If split: termdict's additive `cursor::seek` (the shape of `td_cursor` in
   `bench/kv/proto/cached.cc`) lands in Kronuz/termdict before step 1's reader needs it; a
   termdict sequential-decode speed pass (32-36 ns per key today, target about 15) before step 2's
   T5 scan rows.
3. The lead sets step 1's done criteria in this ledger (the design proposes K1 to K7) and starts a
   developer on step 1.
4. Later steps, each its own branch: the core scan and sorted view (step 2), the cache after its
   design review with Q2's preset and rank ordinals (step 3), the arena buffer (step 4).

## Blocked on

The owner: Q1 and Q2 (step 1 as written assumes both recommendations).
