# Code review: perf/merge-stream (87be659)

Reviewer: code-reviewer, fresh context. Branch `perf/merge-stream` at 87be659 (HEAD equals origin; base 9ba0659,
main at 88a56e7 has no `include/` changes since). Reviewed: the diff of `include/strata/core/{writer,merge,keys}.hh`,
`test/merge_stream_test.cc`, `docs/perf/merge-stream.md`, `docs/status/merge-stream.md`, against calibration.md.

## Verdict: APPROVE WITH CONDITIONS

One blocker (CR1): the shared stored frame writer now keeps its frame index in builder members, so a
second `flush()` of the same writer (documented as writing the same bytes, `keys.hh:187`) silently writes a
corrupt segment. Nothing in-tree flushes a writer twice (the index and prospect build a fresh writer per
flush), so no shipped path is hit today; it is a one-line fix plus a test, in the shared API of the format
writer, so it lands fixed. Everything else the task named holds: bytes are identical to main on every shape I
could construct, a failure at any write or checkpoint of a streamed merge leaves nothing behind, and the
merge's charge is bounded by blocks plus per-key state.

Blocking: CR1. Conditions: fix CR1 and add the regression test; re-check by the rechecker (probe
`.scratch/review/diff_probe.cc ... twice`).

## What I ran

| Check | Result |
|---|---|
| Release ctest (`.scratch/review/rel`) | 38/38 |
| ASan+UBSan, Homebrew LLVM, RelWithDebInfo, full ctest (`.scratch/review/asan`, `asan.log`) | 38/38, no reports |
| TSan, Homebrew LLVM, full ctest (`.scratch/review/tsan`, `tsan.log`) | 38/38, 0 ThreadSanitizer warnings |
| CI on 87be659 (run 39703473900), 8 jobs: linux gcc/clang/arm64, asan-ubsan, tsan, freebsd, macos arm64/x86_64 | all success |
| `merge_stream_test` compiled against main's include tree (`bin/merge_stream_test.main`) | the five digests pass on main; `bounded()` fails on main (budget peak 34.6 MB vs 2.1 MB on the branch). The pinned digests are genuinely main's, and the bound test fails without the code. |

## Findings

### CR1 (blocker): a second flush of the same writer writes a corrupt stored section

`include/strata/core/writer.hh:312-364`, `:414-417`. `encode_stored` (the non-streaming, flush path) now
writes through `put_frame`, which appends to the members `index_`, `offset_` and `frames_` (`:360-364`);
nothing resets them at the start or end of `encode_stored`. The old `encode_stored` kept `out`, `index` and
`frames` local, so `finish()` was repeatable. `keys.hh:187` documents the contract ("the entries are sorted in
place, so a second flush writes the same bytes"), and the flush-memory failure design depends on a flush that
fails (ENOSPC, cancel) being re-run.

Probe: `.scratch/review/diff_probe.cc` mode `twice` (`bin/diff_probe.branch 1 twice` vs `bin/diff_probe.main
1 twice`; output in this review's run). On main all 12 cases print SAME (copied and borrowed writers). On the
branch all 12 print DIFFERENT, and `check_segment` on the second file fails with `corrupt segment: stored frame
bounds`: the index holds the first flush's entries again, offsets continue from the first run, and the frame
count doubles. It is silent unless the caller verifies.

Reachability today: `flush_documents` (`index.hh:2554`) and prospect's `inverter::flush` construct a
writer per flush, so the engine does not hit it. The risk is the public `basic_segment_writer::flush` and a
failed-then-retried flush on a retained writer.

Fix (verified by `mut/fix1_reset`, 3 statements): at the top of `encode_stored`, `index_.clear(), offset_ = 0,
frames_ = 0;` (or make them locals threaded through `put_frame`). With it all 12 twice cases print SAME and the
diff probe is unchanged. Add a test: flush one writer twice, copied and borrowed, assert equal bytes and
`check_segment` ok; also a flush that throws mid-way (a cancel token set after the first frame) followed by a
flush.

### CR2 (non-blocking, follow-up): the pinned test is weaker than its comments say

`test/merge_stream_test.cc:108-116`. In `shapes()` the `block` and `codec` fields configure the input
segments' writers only; the merge's output block is the layout's policy block (4 KiB for the document layout,
`merge.hh:443`) and its codec is zstd(3) in every pinned case (`merge()` default). So:

- The merged file's lz4 and raw output frames are not pinned (lz4 is only compared "valid and different from zstd", `:150`).
- The comment on shape 5 ("the count cap closes blocks of empty records") is not what it tests; the count cap of
  `gather()` (`writer.hh:338`, `pending_count_ >= block_`) is reached only by 4096 consecutive empty records.
- Mutants (`.scratch/review/mut/`, `mutants.sh`; output in the review run): `m2_count_gt` (`>=` to `>` on the count cap) survives
  `merge_stream_test`, and survived my 120-seed random grid; it is killed by the edge probe below. `m6_dupcheck`
  (remove the duplicate-docid check in the ordered plan, `merge.hh:388-391`) survives `merge_stream_test` and
  the random grid, killed by the edge probe. `m7_ordinal` (off-by-one in `plan.ordinals`) survives
  `merge_stream_test` but is killed by the grid. `m1` (byte rule `>` to `>=`), `m3` (frame length), `m4`
  (first ordinal), `m5` (drop the last block) are killed by both.
- The plan rewrite's other branches (docid-order merge, explicit `order`, `records` spill, single permuted
  source; `merge.hh:389-433`) have no pinned digest; the test only drives the key-ordered branch.

Nothing is wrong in the code: the branch equals main on all of these (below). Ask: fold the edge cases
(`diff_probe.cc` `edges()`: 4097+ empty records, duplicate docid across sources, the five invalid orders) and one
digest each for lz4 and raw merge output and the explicit-order branch into `merge_stream_test`, and correct the
shape-5 comment.

### CR3 (non-blocking): charge lags or misses a few transients

All of these are small and in the safe direction overall (the budget peak was at or above the process's
dirty-page footprint in every probe row, below), but "charges what it holds" is not exact:

- `stored_builder::close_block` charges after the block is closed and compressed (`writer.hh:355`): up to one
  block plus one record in `pending_` and `block_raw_`, and `packed_`, are allocated before they are charged.
  With a 64 MiB record (`max_stored_record`) that is about 3 x 64 MiB charged after the fact. The old code
  charged after the fact too, so this is not a regression.
- The 16-byte-per-frame `index_` (`writer.hh:360`) and the section stream's 64 KiB gather buffer are not charged.
- The ordered plan's `sorted` copy of the docids (`merge.hh:388`, 8 B a document) is allocated and freed without a charge.
- `sorted_encoder::reserve` (`keys.hh:83`) allocates 8 B for every ordered document, keyless ones included,
  and charges only when the first keyed document is added (`keys.hh:97`); an all-keyless key-ordered merge
  allocates the reserve and never charges it. Untouched reserve pages are not resident, so this is accounting only.

### CR4 (non-blocking, for the architect): "nothing grows with the segment" is true of the stored section, not of the keys

Probe `mem_probe` (below): the stored section stays flat (3 x 100 MiB and 3 x 200 MiB of 4 KiB records, or 64 KiB
records), but the keys builders' state grows per document: 1.4M documents of 200 B cost a 137.8 MiB budget peak
(about 97 B a key: the filter's `64 n + 1 MiB` bound plus hashes and versions), against 1.4 MiB for 4,320
documents. The write-up says so (`docs/perf/merge-stream.md`, "What remains"); the many-indexes target needs
the partitioned filter (a format change) to bound it. Recorded so the design reads "bounded by blocks plus 97 B
a key", not "bounded".

### CR5 (nits)

- `docs/perf/merge-stream.md` Harness line says "macOS arm64"; the results header and the ledger say x86_64. This machine is x86_64.
- The ordered plan now validates the order before it checks for two live copies of a docid (`merge.hh:370-391`);
  main checked the docids first. An input wrong in both ways reports `invalid_argument` instead of `logic_error`.
  All single-fault inputs report the same exception and message as main (edge probe, 9 error cases identical).

## Task items

### 1. Byte identity: holds

`.scratch/review/diff_probe.cc` built twice (`bin/diff_probe.branch`, `bin/diff_probe.main`, main's include tree
from `git archive 9ba0659 include`), same inputs, output compared line for line:

- 400 random parameter sets, 9 operations each (3,735 lines, 3,393 compared bytes-for-bytes, the rest are
  the same `stored block size` rejection on both): segments 1-4 of 0-2,500 documents (empty and single-record
  segments included), keyed fraction 0/50/90/100 percent (keyless documents), deleted 0-100 percent (all
  deleted), zstd 1/3/19, lz4, raw, six record-size mixes (0 to 70,000 bytes), block targets 1 to 65,536, with
  and without a memory charge. Operations: key-ordered merge, keys+stored merge in docid order, the same with
  an explicit shuffled order, stored-only with order and the `records` scratch, a single permuted source with
  `records`, and flushes (copied, borrowed, reserved, charged). Result: `out.diff.main.txt` equals
  `out.diff.branch.txt` byte for byte (`cmp`: IDENTICAL), each segment also passes `check_segment`.
- Edge probe (`diff_probe 1 edge`, 149 cases): runs of empty records at block targets 1, 3, 64, 4096, 4097
  with 63 to 2,500 documents per segment, merges and flushes, duplicate docids, and the invalid orders:
  branch equals main.
- At scale: `mem_probe` merges 3 x 100 MiB inputs (1.4M documents at 200 B, 4 KiB and 64 KiB records, keyless
  stored-only, zstd) into a hashing sink; the 150 MB outputs hash identically on branch and main in all five
  configurations (`out.mem2.txt`).
- Search layout: the shared frame writer touches only the stored section; prospect is a separate repository
  pinned to an older strata, and its layout (64 KiB blocks) is not exercised here (see Not run).

### 2. Failure atomicity while streaming: holds

`.scratch/review/failure_probe.cc` runs a stored-heavy background merge (6 segments, ~5.8 MiB of incompressible
records, 16 KiB write chunks, 134 writes and 134 checkpoints per clean merge) on faultfs with a 64 MiB budget, and
injects: EIO and ENOSPC at the nth write, before and after the effect (nth 1-11, then every fifth); ENOSPC by
capacity at 25 sizes; cancellation at checkpoints 1-11, then every third. 212 trials (72 fenced, 134 dropped
and retried, the rest finished before the fault), 0 failures, also clean under ASan+UBSan (`bin/failure_probe.asan`).
After each dropped merge: sources and digest unchanged, segment files equal to before (no partial segment),
no `.spill`/`.rec`/`.fwd`/`.tmp`, `memory.used()` back to its value before the merge, a retry succeeds with
the same contents, and after commit no unnamed segment file remains. After each EIO: the index is fenced,
`commit()` throws, reopening recovers the last commit with the same digest, no partial file. The same probe
passes on main (200 trials), so the streaming does not change which cleanup runs; the partial output is removed by
`run_merge`'s existing catch (`index.hh:1793-1798`). The stream's open section has no destructor effect: an exception
between blocks leaves `section_sink::open_` set and the sink is discarded with the output.
Reclaim and retain: `index.hh` is not in the diff and the existing reclaim/retention/lifecycle tests pass in all
three ctest runs; the probe adds no view-lifetime case beyond them.

### 3. Memory accounting: holds, with CR3 and CR4

`.scratch/review/mem_probe.cc` (macOS x86_64; inputs mapped from files, output discarded into a hashing sink,
budget peak from `byte_budget`, dirty footprint from `phys_footprint` sampled every 2 ms, above the pre-merge level;
branch / main):

| Inputs (10 percent deleted) | Budget peak MiB | Footprint peak MiB |
|---|---|---|
| 3 x 100 MiB, 4 KiB records, keyed, lz4 | 7.7 / 780.0 | 2.9 / 539.4 |
| 3 x 200 MiB, 4 KiB records, keyed, lz4 | 14.4 / 1560.0 | 4.7 / 1094.8 |
| 3 x 100 MiB, 64 KiB records, keyed | 1.4 / 770.9 | 0.1 / 537.0 |
| 3 x 100 MiB, 200 B records, keyed (1.4M docs) | 137.8 / 808.1 | 96.7 / 473.7 |
| 3 x 100 MiB, 4 KiB records, keyless (stored only) | 2.4 / 778.3 | 0.1 / 539.8 |
| 3 x 100 MiB, 4 KiB records, keyed, zstd | 7.7 / 788.0 | 1.8 / 540.8 |

The stored section does not grow with the input (keyless: 2.4 MiB at 150 MB of output; 64 KiB records: 1.4 MiB);
the keyed rows grow with the document count only (CR4). The budget peak is above the footprint in every row
(conservative), and the charge held at the end is returned (`job.memory.shrink(held)`, `index.hh:1785`; the
failure probe checks `used()` returns). Doubling the input doubles the keyed row, so per-key, not per-byte.
The merge's output buffer (`merge_output`, up to the 1 MiB chunk) is outside both numbers; the probe used a
counting sink.

### 4. The flush path: holds, except CR1

The flush uses the same `append_frame`/`put_frame`, writes each frame through the section stream, and drops the
encoded-section string; `flush_memory_test` (borrow/lease rules, reserve, pinned digests) and the flush cases of
the differential probe pass (copied, borrowed, reserve, charged flushes equal main). `encode_stored` still reads
`views_` through the builder's lease, which the builder keeps until it is destroyed, so the lease rule is
unchanged. A flush no longer charges the encoded section (it no longer exists), so a flush's charge is lower
than main's by the section's size, as the ledger reports. Failure cleanup of the flush is the caller's
(`flush_documents`' removal of every run's file, `flush_active`'s ENOSPC removal); the stored section now reaches
the file before the later sections, as every other streamed section already did. The one defect is the repeat
flush, CR1.

## Probes and artifacts (all in `.scratch/review/`, kept)

- `diff_probe.cc` (differential, modes: default, `1 edge`, `1 twice`), `failure_probe.cc`, `mem_probe.cc`.
- `build_probe.sh` (compile a probe against branch, main, or a mutant include tree), `step1.sh` to `step10.sh`, `san.sh`, `mutants.sh`.
- `mut/m1..m7`, `mut/fix1_reset` (mutant and fix-sketch include trees), `main/include` (main's headers).
- Outputs: `out.diff.{main,branch,asan}.txt`, `out.edge.*.txt`, `out.fail.*.txt`, `out.mem.txt`, `out.mem2.txt`, `asan.log`, `tsan.log`, `step*.log`.
- Builds: `rel/`, `asan/`, `tsan/`; `data/` (1.1 GB of generated inputs, safe to delete).

## Not run

Local Linux and FreeBSD builds (CI's eight jobs on 87be659 are green instead); LeakSanitizer (unsupported on
macOS; ASan/UBSan on the probes found no errors, but leaks are not checked); prospect or the search layout
(separate repository on an older strata pin; its 64 KiB block and inverted sections are untested here); the
kv_strata RSS/throughput A/Bs and any timing (told no timing; the write-up's numbers are not reproduced);
the merge through the real `merge_output` and file path at 100 MiB scale (the memory probe used a counting
sink); real-disk ENOSPC (faultfs only); index-level multi-run flush beyond ctest (the writer-level borrowed
flush is covered); a view-lifetime case beyond the existing reclaim/retention tests.
