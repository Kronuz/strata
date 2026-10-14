# Phase 3 lifecycle rows: power loss, digests, disk full, robustness

PLAN.md, Phase 3R: Phase 3R starts once Phase 3's lifecycle rows pass (power loss,
digest, disk full, fuzzing, sanitizers, coverage), with C1–C4 passed; Phase 3's ratchet
rows run in parallel and gate Phase 4. This report covers the lifecycle rows of
`docs/design/phase-3.md` (T1, T2, T5, T6) at strata `bc10295`, for the re-check of
`docs/reviews/phase-3-lifecycle.md` (L1–L4). The ratchet rows (T3, T4, T7, T8) and T9's
performance conditions are reported separately.

Tests run on macOS x86_64 with Homebrew LLVM 23 (unit, sanitizer, and coverage builds);
the fuzz hours on the bare-metal M1 Pro (arm64) with Homebrew LLVM.

## Results

| Row | Target | Result | Status |
|---|---|---|---|
| T1 Power loss | ≥ 10,000 crash points, zero losses, zero inconsistent opens, over adds, replaces, and deletes by key and docid, rejected transactions, metadata, refreshes, budget flushes, merges, commits, `checkpoint`, `install`, and `recover --to`; crashes during recovery | `lifecycle_test`: 10,000 crash points over 31 seeded workloads applied at log positions, releases of pinned checkpoints included, every mutation of each, sector-granular power loss (`faultfs`); each recovery's (digest, applied position) is a commit point's, and replaying the whole log after it reaches the uninterrupted digest (condition 1); the index then merges in full, commits, and reopens to the same digest; half the workloads under a 32 KiB memory budget with a 4 KiB buffer share, so the buffer flushes as it fills. Every third crash point arms a second power loss 0–6 mutations into the recovery that follows, and at every 50th the crashed state is rebuilt once per mutation of its recovery with a second loss after that mutation: 533 points inside recoveries (condition 3). Merges run on the writer's thread, so no delete arrives during one. `install` at every mutation, on checkpoints of several segments with live docs and metadata, with a second loss during the recovery at every third point (L1). `create` at every mutation: the directory opens as the empty index or as `index_not_created`, and `create` runs again over it (9 of 10 points; condition 2). `recover --to` at every mutation of its own open. `index_test` cuts power at each of the 28 mutations of a commit that drops a segment and of its stepped reclamation. Zero losses, zero inconsistent opens | Pass; condition 7 carries |
| T1, injected corruption | Fails closed, tested apart | `index_test`: a missing or changed file fails the open and changes nothing; `index::check()` finds a changed byte the open does not read; `install` refuses five damaged checkpoints (live docs past their segment, a live count or a document count unlike its entry, a damaged postings region, a missing file) with the target unchanged, unfenced, and clean under ASan (L1); an intact file from another generation or another index under a name the manifest gives, or a manifest resealed with entries its files do not match, fails the open with that check's own error and changes nothing, one case per binding check (condition 4) | Pass |
| T2 Digest | Two replays and schedules (a flush after every transaction, none); merges; a refresh between a delete and a re-add; a merge dropping a deleted copy before its key or docid returns; replay after recovery and after `install`: equal digests and verdicts | `lifecycle_test`: 40 seeded workloads under three schedules (the workload's commit points, a flush after every transaction, no flush until the end, then reopened), merges included, the digest compared after every transaction on the first ten seeds (D12); the named sequences under four schedules (none, refresh after each, merge after each delete, merge after every step), every transaction accepted; replay of a whole log after a commit-and-reopen and after `install`, the applied half skipped and the rest equal to the uninterrupted run's verdicts and digest | Pass; merges losing documents while they run wait for background merges (D18) |
| T5 Disk full | Storage admission reserves a commit's bytes; merges reserve their output; standalone admission rejects new transactions while space is short; ENOSPC at write time recovers in place, at fsync time fences; capacity filled during flushes, merges, and every file class | `byte_budget` as storage (L4): an index holds its files' bytes, a standing allowance for its next commit's own files (a manifest with one more segment entry, a live-docs file per segment with deletes since its last one, a metadata file when it changed), each transaction's estimated bytes plus a segment's fixed bytes for the transaction that starts a buffer, and a merge bound that holds for docids and versions; `verdict::no_space` otherwise, changing nothing. Queued obsolete files are reclaimed before admission refuses. On a 64 KiB disk under a 64 KiB budget, one-document commits run until admission answers `no_space` (47 of them) and never hit ENOSPC; a merge of segments with docids 2^40 apart is refused by admission or fits. Writes cut to 97 bytes with the capacity at every 97-byte step from 0 to 6,000, so ENOSPC strikes mid-file: the commit fails without fencing, the partial file is removed, and the retry equals a reference commit before and after reopening; ENOSPC during flush and merge leaves the buffer, segments, and digest as they were; a failed sync fences and reopening recovers | Pass |
| T6 Fuzzing | One CPU-hour per new or changed decoder, at the commit reviewed | At strata `d41c957` with termdict `0297799` (no decoder, fuzz target, or pin changed through `bc10295`), ASan and UBSan with `-fno-sanitize-recover`, 10 s hang timeout, 8 workers for 482 s each (1.07 CPU-hours per target): `fuzz_segment` 251,632 runs, 5,073 PCs, corpus 1,257; termdict's coded dictionary 10,550,311 runs, 915 PCs, corpus 881. Zero findings. The first M1 run found termdict's pointer overflow (fixed in `0297799`; the input replays from `a3b6f18`). `fuzz_files` stands (`files.hh` unchanged since `5a8b5e3`) | Pass |
| T6 Fixtures | A fixture per new format | `small.sx` (format 12), `manifest.bin`, `live.bin`, `meta.bin`; termdict's `small.td` | Pass |
| T6 Sanitizers | ASan, UBSan, TSan over every test | See "Sanitizers" | Pass; TSan over flush and merge threads waits for D18 (none exist yet) |
| T6 Coverage (C16) | Decoders 98% lines and 95% branches; scorers 95/90; everything else 90/80 | See "Coverage": every file at its targets | Pass |
| T6 Corruption checks (C13) | Every corruption check of the new decoders reached by a resealed unit test or the replayed corpus | Every check reached but five guards, listed under "Coverage" with their reasons; the wrapped position-index test and `or_bitset`'s byte and count checks reached (L2) | Five open, with reasons |
| T6 Parity (R8) | Phase 2 and Phase 3 rows of the test-parity inventory | `docs/testing/parity.md`: Lucene's index-writer, merge-policy, delete, crash, disk-full, and corruption tests; Tantivy's indexer, merger, and delete tests | Pass; owed rows listed there |

## Power loss and digests (T1, T2)

`faultfs` models a crash per sector (old, new, or zeros for each unsynced sector) and per
namespace operation (each unsynced create, rename, or removal survives or not), with
faults before or after their effect, capacity, and short writes. `lifecycle_test`
counts a workload's mutations in a dry run, then replays it with power lost after each
one and checks that reopening gives the digest of a commit point from the last
acknowledged to the last attempted.

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
merge reserves its sources' bytes plus 18 bytes a document, 16 a block, and a segment's
fixed bytes, which bound its output when docids and versions lose their compact forms; a
full merge of an index that fills most of its disk therefore waits, as Lucene's does,
for room for its output beside its sources. Obsolete files count until they are gone,
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

At `bc10295`: every test under ASan with UBSan (`-fno-sanitize-recover=undefined`), T1
at 10,000 crash points; every test under TSan. ASan found a use of freed memory in three
new tests (a segment over a temporary string; `segment(std::string&&)` is now deleted so
the mistake cannot compile) and a budget destroyed before its index in `index_test`.

## Coverage

`tools/coverage.sh` at `bc10295`: every test (T1 at 2,000 crash points), `parity` against
the oracle's ranks on 10,000 documents, and `verify` on a 10,000-document index over the
gate's 1,162 queries (0 mismatches), with both fuzz corpora replayed:

| File | Target (lines / branches) | Lines | Branches |
|---|---|---:|---:|
| `reader.hh` | 98 / 95 | 99.20% | 95.20% |
| `format.hh` | 98 / 95 | 100% | 98.85% |
| `files.hh` | 98 / 95 | 100% | 99.18% |
| `search.hh` | 95 / 90 | 98.54% | 93.66% |
| `searcher.hh` | 95 / 90 | 97.70% | 92.65% |
| `writer.hh` | 90 / 80 | 99.84% | 90.94% |
| `index.hh` | 90 / 80 | 99.50% | 89.66% |
| `merge.hh` | 90 / 80 | 100% | 97.14% |
| `merge_policy.hh` | 90 / 80 | 97.28% | 87.88% |
| `check.hh` | 90 / 80 | 100% | 87.50% |
| `budget.hh` | 90 / 80 | 100% | 90.00% |
| `faultfs.hh` | 90 / 80 | 96.03% | 87.80% |

Corruption checks (C13): `segment_test` reaches the open-time checks with sections
replaced at any length and the file re-laid out (footer bounds, stats, contiguous and
monotone docids, versions, the key filter, each sub-condition), crafted bytes for the
term-info and impact decoders (each impact count's sub-conditions included), dictionaries
rebuilt with values the postings cannot hold, a sweep that changes every byte of a small
segment's postings and positions (bitset, FOR, and wdf blocks over two skip groups;
stepped position blocks), reseals it, and reads everything, the wrapped position-index
test (a group's first position near 2^64, wrapping in the group decode and inside a
block: every read fences), and a window count that reaches `or_bitset`'s byte and count
checks. Counted per sub-condition over every instantiation, five guards stay unreached:

- `or_bitset`'s span check (`reader.hh:687`): `fill` passes only blocks after one it
  consumed at or past the window's base, so `first < base` cannot come from data.
- A FOR block whose ordinals use every byte, leaving none for wdfs (`reader.hh:858`),
  and a decoded first ordinal of `no_more_docs` (`reader.hh:865`): each needs two
  coordinated changes, to skip data and to block bytes.
- A position block's offset past the section while stepping, and a block whose size
  cannot be read there (`reader.hh:1135, 1138`): the sweep steps position blocks but
  reaches neither, since each needs a block-length change that keeps every earlier block
  readable.

## Changes for the architect

- L1: `load_files` checks every file a manifest names against its entry, for `recover`
  and `install` alike; `install` verifies the whole checkpoint before anything changes,
  outside the fencing guard (`c266040`).
- L2: `decode_group` and `position_index` reject a position sum that wraps (`28e2362`).
- L3: the fuzz hours above, at the fixed commits.
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
- Conditions 1–4 are met (`ccb4fb1`, `914db86`, `1616e52`), and condition 9's items are
  done (the retention barrier, the per-transaction digest, these corrections, and the
  merge-policy note). Conditions 5–8 carry to their stated points.
- The bare-metal machine is a MacBook Pro M1 Pro (arm64, 32 GB; BENCHMARKS rule 4).
  strata, Lucene, and Xapian 2.1.0 are built there; its ssh service stopped accepting
  connections before Tantivy built, so the cross-engine trials wait for it, with the gate
  VM as the fallback.
- TSan over flush and merge threads and T2's merges racing deletes, once D18's
  background merges exist.
