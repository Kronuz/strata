# Phase 3 lifecycle rows: power loss, digests, disk full, robustness

PLAN.md, Phase 3R: Phase 3R starts once Phase 3's lifecycle rows pass (power loss,
digest, disk full, fuzzing, sanitizers, coverage), with C1–C4 passed; Phase 3's ratchet
rows run in parallel and gate Phase 4. This report covers the lifecycle rows of
`docs/design/phase-3.md` (T1, T2, T5, T6) at strata `85b3cdc`, for the fourth re-check of
`docs/reviews/phase-3-lifecycle.md` (L8's remainder, conditions 15–17, and finding 48's
corrections). The ratchet rows (T3, T4, T7, T8) and T9's
performance conditions are reported separately.

Tests, sanitizer, coverage, and fuzz builds run on macOS x86_64 with Homebrew LLVM 23;
the bare-metal M1 is unreachable, so its fuzz hours are run again here.

## Results

| Row | Target | Result | Status |
|---|---|---|---|
| T1 Power loss | ≥ 10,000 crash points, zero losses, zero inconsistent opens, over adds, replaces, and deletes by key and docid, rejected transactions, metadata, refreshes, budget flushes, merges, commits, `checkpoint`, `install`, and `recover --to`; crashes during recovery | `lifecycle_test`: 10,000 crash points over 31 seeded workloads applied at log positions, releases of pinned checkpoints included, every mutation of each, sector-granular power loss (`faultfs`); each recovery's (digest, applied position) is a commit point's, and replaying the whole log after it reaches the uninterrupted digest (condition 1); the index then merges in full, commits, and reopens to the same digest; half the workloads under a 32 KiB memory budget with a 4 KiB buffer share, so the buffer flushes as it fills. Every third crash point arms a second power loss 0–6 mutations into the recovery that follows, and at every 50th the crashed state is rebuilt once per mutation of its recovery with a second loss after that mutation: 555 points inside recoveries (condition 3). Merges run on the writer's thread, so no delete arrives during one. `install` at every mutation, on checkpoints of several segments with live docs and metadata, with a second loss during the recovery at every third point (L1). `create` at every mutation under 50 crash seeds each (550 crashes): the directory opens as the empty index or as `index_not_created`, and `create` runs again over it; `create` refuses a committed index without its owner lock, and an index whose documents were all deleted, whose metadata changed, and which committed past its retained predecessors does not read as uncreated when its manifests are lost; one whose metadata never changed does, D3's stated residual (condition 2). `recover --to` at every mutation of its own open. `index_test` cuts power at each of the 30 mutations of a commit that drops a segment and of its stepped reclamation; `reclaim_test` cuts power across reclamation in 12 workloads with pins, releases, merges, two predecessors, and 256-byte steps (3,635 points, 16,274 manifests checked, 2,997 truncated orphans): every manifest on disk names intact files, every pin survives, and every file is named after `reclaim()`; without the directory barrier, 93 manifests name missing files (condition 9). Zero losses, zero inconsistent opens | Pass; condition 7 carries |
| T1, injected corruption | Fails closed, tested apart | `index_test`: a missing or changed file fails the open and changes nothing; `index::check()` finds a changed byte the open does not read; `install` refuses eight damaged checkpoints (live docs past their segment, a live count unlike its entry with the manifest's totals in step, live docs whose delta wraps back onto an ordinal, live docs whose bitset size wraps near 2^64 (L7), a manifest under another generation's name, a document count unlike its entry, a damaged postings region, a missing file) with the target unchanged, unfenced, and clean under ASan (L1); the live-docs decoder refuses a delta that wraps 2^64, so ordinals strictly ascend, and a segment's dead count is its bitmap's popcount (`86b50a5`); an intact file from another generation or another index under a name the manifest gives, or a manifest resealed with entries its files do not match, fails the open with that check's own error and changes nothing, one case per binding check, now including the live docs' segment, live count, and live length with the totals in step, and a wrapping delta (condition 4, finding 25) | Pass |
| T2 Digest | Two replays and schedules (a flush after every transaction, none); merges; a refresh between a delete and a re-add; a merge dropping a deleted copy before its key or docid returns; replay after recovery and after `install`: equal digests and verdicts | `lifecycle_test`: 40 seeded workloads under three schedules (the workload's commit points, a flush after every transaction, no flush until the end, then reopened), merges included, the digest compared after every transaction on the first ten seeds (D12); the named sequences under four schedules (none, refresh after each, merge after each delete, merge after every step), every transaction accepted; replay of a whole log after a commit-and-reopen and after `install`, the applied half skipped and the rest equal to the uninterrupted run's verdicts and digest | Pass; merges losing documents while they run wait for background merges (D18) |
| T5 Disk full | Storage admission reserves a commit's bytes; merges reserve their output; standalone admission rejects new transactions while space is short; ENOSPC at write time recovers in place, at fsync time fences; capacity filled during flushes, merges, and every file class | `byte_budget` as storage (L4): an index holds its files' bytes, a standing allowance for its next commit's own files (a manifest with one more segment entry, a live-docs file per segment with deletes since its last one, a metadata file when it changed), each transaction's estimated bytes plus a segment's fixed bytes for the transaction that starts a buffer, and a merge's estimate, grown to the output's real size before it is written (postings can grow when merged: L5); `install` reserves the copy and its commit's metadata file and manifest; `verdict::no_space` otherwise, changing nothing. Queued obsolete files are reclaimed before admission refuses. On a 64 KiB disk under a 64 KiB budget, one-document commits run until admission answers `no_space` (47 of them) and never hit ENOSPC; on a disk of the budget's size, merges of segments with docids 2^40 apart (room in 200-byte steps across admission's threshold) and of growing postings (1,000 terms on A's first document and B's 127, 1,000-byte steps), and an `install` with 32 KiB of metadata (1,000-byte steps), complete or are refused by admission, and the disk never refuses; without the growth, 20 merges meet ENOSPC on the disk, and without the install's commit bytes, 33 installs do. The growing-postings merges with a 20,000-, 50,000-, or 300,000-byte obsolete segment queued (rooms 158,000–228,000 bytes in 2,000-byte steps) never meet a disk refusal, and at the merged segment's create the budget covers the output: the merge's reservation is held through the drain (L5; without it the budget falls short of the output), and a removed file's bytes count as free once the directory sync makes its removal durable (L8; without it the disk refuses admitted merges and a transaction admitted by draining fails every commit). Writes cut to 97 bytes with the capacity at every 211-byte step from 0 to 3,000, so ENOSPC strikes mid-file: the commit fails without fencing, the partial file is removed, and the retry equals a reference commit before and after reopening (a second loop runs every 97-byte step to 6,000 without short writes); ENOSPC during flush and merge leaves the buffer, segments, and digest as they were; a failed sync fences and reopening recovers; a failed truncate, unlink, or directory sync after the commit point fails neither the commit nor a merge (condition 10). The budget counts file bytes; on a real filesystem, capacity leaves a block per file of headroom (condition 11) | Pass |
| Media faults (beyond the design's rows) | Unreliable reads heal or fail closed; no answer is ever wrong (`docs/design/media-faults.md`) | `media_test`: EIO or one flipped bit at 782 offsets across every file, transient and sticky: transient faults all heal (529 by the open's retry, 104 by `heal()`); sticky ones fail the open (529), are reported damaged by `heal()` (104), or land in bytes nothing reads (149); the digest never differs from the undamaged one. A merged segment damaged for good is recovered with `recover --to`. termdict's own corrupt-block error now reports as `corrupt_segment` | Pass |
| T6 Fuzzing | One CPU-hour per new or changed decoder, at the commit reviewed | ASan and UBSan with `-fno-sanitize-recover=all`, 10 s hang timeout, 4 workers, on this Mac (x86_64, Homebrew LLVM 23). `fuzz_files` at L7's decoder (`a10df59`, unchanged through `b493296`), its generator drawing document counts near 2^64: 910 s, 74,383,545 runs, 736 PCs, corpus 249 (`.scratch/fuzz-l7/`). `fuzz_segment` at `86b50a5` (since then the segment decoders change only in which exception a damaged term block raises, `05df531`, and in a dictionary scan that reports it, `85b3cdc`; the pin is unchanged): 901 s, 156,376 runs, 4,906 PCs, corpus 1,218; its first job reports at 60 s, after merging the 1,089 seed files in one process, so a strict count is 0.95 CPU-hours, and the M1's run at `d41c957` adds 1.08 CPU-hours over the same code (`.scratch/fuzz-m1/fuzz-seg-l3b.log`). termdict's coded dictionary at the pin `0297799`: 910 s, 12,863,232 runs, 862 PCs, corpus 807, started from no seed since termdict checks in no corpus (`.scratch/fuzz-l3c/`). Zero findings | Pass |
| T6 Fixtures | A fixture per new format | `small.sx` (format 12), `manifest.bin`, `live.bin`, `meta.bin`; termdict's `small.td` | Pass |
| T6 Sanitizers | ASan, UBSan, TSan over every test | See "Sanitizers" | Pass; TSan over flush and merge threads waits for D18 (none exist yet) |
| T6 Coverage (C16) | Decoders 98% lines and 95% branches; scorers 95/90; everything else 90/80 | See "Coverage": every file at its targets | Pass |
| T6 Corruption checks (C13) | Every corruption check of the new decoders reached by a resealed unit test or the replayed corpus | Every check reached but five guards, listed under "Coverage" with their reasons; the wrapped position-index test, `or_bitset`'s byte and count checks (L2), and `read_group`'s order check reached | Five open, with reasons |
| T6 Parity (R8) | Phase 2 and Phase 3 rows of the test-parity inventory | `docs/testing/parity.md`: Lucene's index-writer, merge-policy, delete, crash, disk-full, and corruption tests; Tantivy's indexer, merger, and delete tests | Pass; owed rows listed there |

## Power loss and digests (T1, T2)

`faultfs` models a crash per sector (old, new, or zeros for each unsynced sector) and per
namespace operation (each unsynced create, rename, or removal survives or not), with
faults before or after their effect, capacity, and short writes. `lifecycle_test`
counts a workload's mutations in a dry run, then replays it with power lost after each
one and checks that reopening gives the digest of a commit point from the last
acknowledged to the last attempted.

With background merges (condition 7, R2 of `docs/reviews/background-merges.md`), a
second pass runs the same enumeration with merges on a stepping executor
(`test/stepping_executor.hh`): one piece of merge work runs at a time, on a helper
thread only while the test thread waits, so a run replays exactly; a piece ends at each
4 KiB write chunk and each rate-limiter wait. After every workload step the executor
runs 0–3 pieces at seeded points, merges follow every flush by small tiers, and deletes
and replaces land between a merge's scheduling and its publication. Every mutation of
the writer and of the merges (output chunks, the positions spill, removals, directory
syncs) is a crash point, with the same second losses during recovery, and a dry run's
commit points must equal an inline-merging twin's. CI runs 2,000 such points
(`STRATA_BG_CRASH_POINTS`, default 2,000) to bound sanitizer time; the gate's count is

```sh
STRATA_BG_CRASH_POINTS=10000 ./build/lifecycle_test
```

Result at the gate's count, at `e8accae` (RelWithDebInfo `-O2 -g`, asserts on, macOS
arm64, 2 min 35 s): 10,000 crash points over 25 workloads with background merges, 696
inside recoveries; the inline pass, 10,000 over 31, 555 inside recoveries. Zero losses,
zero inconsistent opens.

Found by the new rows and fixed:

- **`recover --to` was not atomic** (`c48fada`). It removed every manifest above the
  chosen one and synced the directory; unsynced removals survive a crash independently,
  so a crash between them could leave a manifest between the old head and the target
  as the newest. The chosen manifest's content is now committed as a new generation
  through the commit protocol. Design D3 updated.

## Disk full (T5)

Storage admission uses the same `byte_budget` as the indexing memory budget (one
implementation per mechanism): an index's permit holds the bytes of the files it owns,
counted at open, on every write, and on every removal, plus what admission reserved. A
transaction's reservation is an overestimate of its on-disk bytes; the flush that writes
it settles the permit to the files' real size. Each index also holds a standing allowance
for its next commit's own files, so a commit that frees space can always be written. A
merge reserves an estimate (its sources' bytes plus 18 bytes a document, 16 a block, and
a segment's fixed bytes), builds its output in memory, and grows the reservation to the
output's real size before writing it, or is refused with nothing written: the estimate
is not a bound, since a term on one early document of A and many of B is a tail of small
deltas in B and one block in the merge, as wide as the jump across A. A full merge of an
index that fills most of its disk therefore waits, as Lucene's does, for room for its
output beside its sources. `install` reserves the checkpoint's files and its commit's
metadata file and manifest. Obsolete files count until they are gone,
and admission reclaims the queued ones before it refuses.

## Merges

The merge policy (`405fbc0`) meets Lucene 10.4.0's TieredMergePolicy contract as its
tests state it: the segment-count budget after every flush and merge in simulated
append-only (20M documents) and update (1M documents, 90% updates) workloads, the deleted
fraction, equal flushes merging into equal segments, maximum-size segments left out,
floor-sized merges, singleton merges reclaiming deletes, and forced merges to an exact
count within 1.5× the size limit. Every merge of two or more segments is balanced (the
largest at most two thirds), which Lucene's write-amplification bound alone does not
check: an unbalanced policy measured 18.6 against the bound of 21.0, the balanced one 9.5.
A segment past the size limit is rewritten alone once its deletes pass the allowed
fraction (`90b23aa`, finding 22). With `auto_merge`, commits merge as refreshes do
(`6d52f79`), and a segment whose documents are all deleted leaves at the commit without
a merge (`41204b6`, `docs/perf/reclamation.md`).

## Sanitizers

At `85b3cdc` (`31e0e1f` adds one test and the coverage tool): every test (16) under ASan with UBSan (`-fno-sanitize-recover=undefined`), T1
at 10,000 crash points; every test under TSan. ASan found a use of freed memory in three
new tests (a segment over a temporary string; `segment(std::string&&)` is now deleted so
the mistake cannot compile) and a budget destroyed before its index in `index_test`.

## Coverage

`tools/coverage.sh` at `35989ce`, unioned over the test binaries (`tools/coverage_union.py`: `llvm-cov report` keeps one binary's counters for an inline function several instantiate, which hid checks only one test reaches; lines are llvm-cov's own lcov records, branches the JSON export's outcomes): every test (T1 at 2,000 crash points), `parity` against
the oracle's ranks on 10,000 documents, and `verify` on a 10,000-document index over the
gate's 1,162 queries (0 mismatches), with both fuzz corpora replayed.

Counts are on evidence (condition 19). A count at or above 2^62 comes from a counter
expression gone negative when an exception unwound past an increment, and is no
evidence. Such an outcome counts only when the same procedure in a single-byte build
(`COV_SINGLE_BYTE=1`, a flag per region) took it (`COV_CONFIRM`). Two `reader.hh`
outcomes and four `search.hh` outcomes rest on saturated counts that the single-byte
build does not confirm; they count as untaken.

| File | Target (lines / branches) | Lines | Branches (evidenced) |
|---|---|---:|---:|
| `reader.hh` | 98 / 95 | 99.88% | 95.76% (655 of 684) |
| `format.hh` | 98 / 95 | 100% | 99.43% (173 of 174) |
| `files.hh` | 98 / 95 | 100% | 99.18% (121 of 122) |
| `search.hh` | 95 / 90 | 98.72% | 92.77% (629 of 678) |
| `searcher.hh` | 95 / 90 | 97.67% | 92.65% (63 of 68) |
| `writer.hh` | 90 / 80 | 99.83% | 90.94% (251 of 276) |
| `index.hh` | 90 / 80 | 99.22% | 92.57% (635 of 686) |
| `merge.hh` | 90 / 80 | 100% | 98.57% (69 of 70) |
| `merge_policy.hh` | 90 / 80 | 97.21% | 87.88% (116 of 132) |
| `check.hh` | 90 / 80 | 100% | 87.50% (7 of 8) |
| `budget.hh` | 90 / 80 | 100% | 90.00% (18 of 20) |
| `faultfs.hh` | 90 / 80 | 94.92% | 87.36% (159 of 182) |

Every uncovered line, with its reason (`COV_LIST=1`):

- **`reader.hh:748`:** the lazy last-document decode of a postings tail when the
  document frequency is a multiple of 128.
- **`index.hh`:**
  - `215`: opening a directory that holds index files but no owner lock.
  - `423`: `File::size()` failing after a failed truncation round.
  - `469, 473-474`: two of `heal()`'s three catch branches, for a segment whose file
    also fails a second read.
  - `496`: `schema_version()`, for the host.
  - `863-864`: `drain()`'s failed directory sync.
  - `965`: the closing brace after a `break`, a region no execution reaches.
- **`search.hh`:**
  - `53`: an unused `statistics` constructor.
  - `137`, `165`: base-class defaults that every scorer overrides.
  - `496-497`: the aligned wrapper's two-phase path; no test wraps a two-phase child.
  - `760`: a required-or-optional scorer's `block_end`, which no pruning parent asks.
  - `820-822`: the counting window's fallback when the exclusion costs more than a
    block per included document.
  - `827-829`: starting the exclusion inside a window.
- **`searcher.hh:45-46`:** the live-docs wrapper's `max_score` and `block_end`, which no
  pruning parent calls through it.
- **`writer.hh:292`:** `doc_count()`, unused.
- **`merge_policy.hh:134-138`:** a forced merge where nothing fits under the cap and the
  next segment joins anyway.
- **`faultfs.hh`:**
  - `69-70`: rejecting a multi-component name, which no test passes.
  - `98-101`: `media_hits()`, a harness accessor no test reads.
  - `165-166`: the operation switch's default.
  - `404-410`: the journal's `entry_footprint` interface, unused by the index.

Corruption checks (C13): `segment_test` reaches the open-time checks with sections
replaced at any length and the file re-laid out (footer bounds, stats, contiguous and
monotone docids, versions, the key filter, each sub-condition), crafted bytes for the
term-info and impact decoders (each impact count's sub-conditions included), dictionaries
rebuilt with values the postings cannot hold, a sweep that changes every byte of a small
segment's postings and positions (bitset, FOR, and wdf blocks over two skip groups;
stepped position blocks), reseals it, and reads everything, the wrapped position-index
test (a group's first position near 2^64, wrapping in the group decode and inside a
block: every read fences), and a window count that reaches `or_bitset`'s byte and count
checks, and a lowered skip-group last that fences both a pruned query and a plain walk
(`reader.hh:789`, reached since `86b50a5`). Counted per sub-condition over every
instantiation, five guards stay unreached. The table is one run; earlier counted runs
varied by an outcome in `reader.hh` and `search.hh` (finding 51).

- `or_bitset`'s span check (`reader.hh:707`): `fill` passes only blocks after one it
  consumed at or past the window's base, so `first < base` cannot come from data.
- A FOR block whose ordinals use every byte, leaving none for wdfs (`reader.hh:878`),
  and a decoded first ordinal of `no_more_docs` (`reader.hh:885`): each needs two
  coordinated changes, to skip data and to block bytes.
- A position block's offset past the section while stepping, and a block whose size
  cannot be read there (`reader.hh:1155, 1158`): the sweep steps position blocks but
  reaches neither, since each needs a block-length change that keeps every earlier block
  readable.

## Changes for the architect

Since the third re-check, in `85b3cdc`:

- L8's remainder: reclaim's truncation steps count as free once one sync of the file per
  round makes the truncation durable (finding 43); a partial file removed after ENOSPC is
  followed by a directory sync (finding 44). Every admitted commit beside a segment being
  truncated in 4,096-byte steps on an 80,000-byte disk succeeds (4 fail without the file
  sync); a commit refused mid-file succeeds as space returns 1,000 bytes at a time (never
  without the directory sync).
- Condition 15: only an EIO that survives the retry reports as `corrupt_index`; EMFILE,
  ENFILE, ENOMEM, and EACCES propagate as themselves, and `read_named` no longer turns
  every open failure into a missing file (test over all five errors).
- Condition 16: the merge's key scan goes through `segment::scan_dictionary`;
  `media_test` merges after every sticky fault (42 escape as `termdict::corrupt` without
  it).
- Condition 17: `coverage_union.py` takes lines from llvm-cov's lcov records and, since
  `31e0e1f`, branches from its JSON export.

Since the second re-check (`34b21ef`):

- L7 (`a10df59`): the live-docs bitset is sized as count / 8 plus one for a remainder, so
  a document count within 7 of 2^64 no longer wraps to an empty body; refused by the
  decoder, by `open`, and by `install` (ASan reports the overflow without the fix).
- L5 and L8 (`6462402`): a merge's or install's reservation is a hold that
  `settle_storage()` counts, so a drain keeps it; a removed file's bytes stay counted
  until a directory sync makes the removal durable, and `drain()` syncs. The queued-file
  merge sweeps (20,000-, 50,000-, 300,000-byte records) never meet a disk refusal, and the
  budget covers the output at the merged segment's create (fails without L5); a
  transaction admitted by draining commits at once (fails without L8).
- Conditions 2, 13, 14 (`b493296`): the all-deleted create case commits past the
  retained predecessors and fails without `uncreated()`'s rule; `retention_blocked()`
  reports a kept manifest that never reads, and releasing its pin resumes retention;
  tests for the merge half of condition 10, the skip for a pinned generation with no
  manifest (fails without it), and a queued file gone before its truncation; retain's
  unreachable ENOENT branch goes (`IO::remove` treats a missing file as success).
- Beyond the review: media faults (`05df531`, `docs/design/media-faults.md`): one retry
  per read, `heal()`, and termdict's corrupt-block error reported as `corrupt_segment`;
  one batched sync per commit's data files (`d3dc7da`, `docs/perf/commit-barriers.md`).

In `86b50a5`, for the second re-check:

- L1: `decode_live_docs` refuses a delta at or past the room left below the document
  count, so a delta cannot wrap; `dead` is the bitmap's popcount. Finding 24's cases are
  refused by the decoder, by `open`, and by `install` with the target unchanged; finding
  25's sub-conditions each have a case with the totals in step; `fuzz_files` asserts
  strictly ascending ordinals. Without the decoder change, `files_test` fails 2 checks and
  `index_test` fails the install refusal, then aborts on the target's reopen.
- L5: the merge's reservation grows to its output's real size before the write, and
  `install` reserves its commit's metadata file and manifest; D5 states what the target
  holds should that commit still meet ENOSPC. The sweeps fail without each change.
- L6: a round of retention that cannot read a kept manifest removes nothing, and only
  successful reads are cached. With either error on the open's read and then on a
  commit's, the pinned checkpoint stays and installs; without the change, the test's
  three checks fail and the install throws ENOENT.
- L2's listing: the lowered group-last case also walks the postings, reaching
  `read_group`'s order check (`reader.hh:789`).
- Condition 2: `create` clears what an interrupted create left however it took the lock,
  and only `meta-1.kv` and `manifest-1.tmp` count as such (D3 states the residual).
  Without it, the 50-seed test ends in a fence on "File exists".
- Condition 9: `reclaim_test`, finding 29's invariants as a test; it fails without the
  barrier.
- Condition 10: retention and reclamation errors after the commit point keep the files
  queued (or the directory sync owed) and fail neither `commit()` nor a merge; without it,
  the test ends in an uncaught EIO.
- Conditions 11 and 12: T5 states the budget's units (design and this report); BENCHMARKS
  records the gate VM's discard setting.

Earlier, for the first re-check:

- L1: `load_files` checks every file a manifest names against its entry, for `recover`
  and `install` alike; `install` verifies the whole checkpoint before anything changes,
  outside the fencing guard (`c266040`).
- L2: `decode_group` and `position_index` reject a position sum that wraps (`28e2362`).
- L3: the fuzz hours above, at `86b50a5`, logs in `.scratch/fuzz-l3c/`.
- L4: the commit allowance, a segment's fixed bytes, and the merge bound (`48b2154`).
- Conditions 1 and 2 (`ccb4fb1`): T1 at log positions with a whole-log replay after
  every recovery; `index_not_created` for a directory an interrupted `create` left, and
  `create` over it (design D3). Condition 3 (`1616e52`): releases in the workload,
  recovery cut at every mutation at a sample of points, and work after each recovery.
  Condition 4 (`914db86`): one test per binding check.
- Finding 5: a directory sync separates retention's manifest removals from its data
  removals (`b4f8a3b`). Finding 23's corrections are in this report.
- Reclamation (`41204b6`): no directory scan per commit; obsolete data files are
  truncated in 16 MiB steps, then unlinked, about 64 MiB per commit, the rest by
  `reclaim()` or when admission is short. Measured on all four platforms in
  `docs/perf/reclamation.md`.
- Each segment counts its deletes, and a search gets no bitmap for a segment without
  any (`bc10295`): before, every query scanned a deletes-free segment's whole bitmap,
  6.5 µs per 1M documents.
- `recover --to` commits the chosen manifest as a new generation (D3).
- `memory_budget` is `byte_budget`; storage admission is its second instance (D17, D20,
  T5); `verdict::no_space`.
- The merge policy returns several merges per call, balanced, with singleton merges for
  deletes and forced merges (`find_forced_merges`, `find_forced_deletes_merges`);
  `index::force_merge_deletes()`.
- Format 12 (not yet frozen): verified regions of 4 KiB (P15); dictionary keys coded
  over the segment's alphabet, restarts every 32 in blocks of 32–64 (P12, 0.992× Lucene);
  the lengths section verified on first use.
- `check_segment` drops its df comparison, which the postings iterator makes impossible.

## Open

- C1–C4 (membership, multi-group, repair, flow control). Track C's split steps 1–3
  landed on 2026-10-13: `metrics`, `completion`, `journal`, and `consensus`, with the
  peer transport in `consensus::transport` (Detent `7487c5e`, LIBRARY_BOUNDARIES,
  "Progress").
- Conditions 1, 3, and 4 are met (`ccb4fb1`, `914db86`, `1616e52`); condition 2 and
  condition 9's barrier test are in `86b50a5`. Conditions 5–8 carry to their stated
  points, condition 6 now including `install`'s dropped pins, and finding 22's second
  bullet carries to T3.
- The bare-metal machine is a MacBook Pro M1 Pro (arm64, 32 GB; BENCHMARKS rule 4).
  strata, Lucene, and Xapian 2.1.0 are built there; its ssh service stopped accepting
  connections before Tantivy built, so the cross-engine trials wait for it, with the gate
  VM as the fallback.
- T2's merges racing deletes under TSan: CI's linux-tsan job runs the background pass
  (2,000 points) on every push since `5d324ab`.
