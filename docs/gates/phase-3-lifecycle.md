# Phase 3 lifecycle rows: power loss, digests, disk full, robustness

PLAN.md, Phase 3R: Phase 3R starts once Phase 3's lifecycle rows pass (power loss,
digest, disk full, fuzzing, sanitizers, coverage), with C1–C4 passed; Phase 3's ratchet
rows run in parallel and gate Phase 4. This report covers the lifecycle rows of
`docs/design/phase-3.md` (T1, T2, T5, T6) at strata `8ddff6f`. The ratchet rows (T3, T4,
T7, T8) and T9's performance conditions are reported separately.

Tests run on macOS with Homebrew LLVM 23 (unit, sanitizer, coverage, and fuzz builds);
measurements on the gate VM described in `docs/gates/phase-2.md`.

## Results

| Row | Target | Result | Status |
|---|---|---|---|
| T1 Power loss | ≥ 10,000 crash points, zero losses, zero inconsistent opens, over adds, replaces, and deletes by key and docid, rejected transactions, metadata, refreshes, budget flushes, merges, commits, `checkpoint`, `install`, and `recover --to`; crashes during recovery | `lifecycle_test`: 10,000 crash points over 35 seeded workloads, every mutation of each, sector-granular power loss (`faultfs`); half the workloads under a 32 KiB memory budget with a 4 KiB buffer share, so the buffer flushes as it fills; every third crash point loses power again during the recovery that follows. `recover --to` at every mutation of its own open; `install` under power loss. Zero losses, zero inconsistent opens | Pass |
| T1, injected corruption | Fails closed, tested apart | `index_test`: a missing or changed file fails the open and changes nothing; `index::check()` finds a changed byte the open does not read | Pass |
| T2 Digest | Two replays and schedules (a flush after every transaction, none); merges; a refresh between a delete and a re-add; a merge dropping a deleted copy before its key or docid returns; replay after recovery and after `install`: equal digests and verdicts | `lifecycle_test`: 40 seeded workloads under three schedules (the workload's commit points, a flush after every transaction, no flush until the end, then reopened), merges included; the named sequences under four schedules (none, refresh after each, merge after each delete, merge after every step), every transaction accepted; replay of a whole log after a commit-and-reopen and after `install`, the applied half skipped and the rest equal to the uninterrupted run's verdicts and digest | Pass; merges losing documents while they run wait for background merges (D18) |
| T5 Disk full | Storage admission reserves a commit's bytes; merges reserve their output; standalone admission rejects new transactions while space is short; ENOSPC at write time recovers in place, at fsync time fences; capacity filled during flushes, merges, and every file class | `byte_budget` as storage: an index holds its files' bytes (equal to the files on disk after commits and reopens), reserves each transaction's estimated bytes at admission (`verdict::no_space` otherwise, changing nothing) and a merge's source bytes before writing. Admitted work commits without ENOSPC on a disk of the budget's size; merges defer; work resumes when another index on the storage is dropped. ENOSPC at every 97-byte offset of a commit's segment, live-docs, metadata, and manifest files fails the commit without fencing, and the retry equals a reference commit before and after reopening; ENOSPC during flush and merge leaves the buffer, segments, and digest as they were; a failed sync fences and reopening recovers | Pass |
| T6 Fuzzing | One CPU-hour per new or changed decoder | `fuzz_segment` at format 12 with a base holding monotone docids, versions, and keys (docids, versions, `pk_filter`, and the other format-12 sections): 20 minutes on one worker plus 40 minutes on six (4.3 CPU-hours, 477,563 runs, 11,406 PCs); `fuzz_files` (manifests, live docs, metadata) 30 minutes on six (3 CPU-hours, 229M runs); termdict's coded dictionary 10 minutes on six (1 CPU-hour, 3.9M runs). ASan and UBSan, 10 s hang timeout, zero findings. Corpora merged and replayed as tests (1,088 and 239 inputs) | Pass |
| T6 Fixtures | A fixture per new format | `small.sx` (format 12), `manifest.bin`, `live.bin`, `meta.bin`; termdict's `small.td` | Pass |
| T6 Sanitizers | ASan, UBSan, TSan over every test | See "Sanitizers" | Pass; TSan over flush and merge threads waits for D18 (none exist yet) |
| T6 Coverage (C16) | Decoders 98% lines and 95% branches; scorers 95/90; everything else 90/80 | See "Coverage": every file at its targets but `reader.hh` branches, 94.57% | Short by 0.43 points |
| T6 Corruption checks (C13) | Every corruption check of the new decoders reached by a resealed unit test or the replayed corpus | Every check reached, or removed with the reason, but seven guards listed under "Coverage" | Seven open, with reasons |
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
it settles the permit to the files' real size. A merge reserves its sources' bytes, which
bound its output; a full merge of an index that fills most of its disk therefore waits,
as Lucene's does, for room for its output beside its sources.

## Merges

The merge policy (`405fbc0`) meets Lucene 10.4.0's TieredMergePolicy contract as its
tests state it: the segment-count budget after every flush and merge in simulated
append-only (20M documents) and update (1M documents, 90% updates) workloads, the deleted
fraction, equal flushes merging into equal segments, maximum-size segments left out,
floor-sized merges, singleton merges reclaiming deletes, and forced merges to an exact
count within 1.5× the size limit. Every merge of two or more segments is balanced (the
largest at most two thirds), which Lucene's write-amplification bound alone does not
check: an unbalanced policy measured 18.6 against the bound of 21.0, the balanced one 9.5.

## Sanitizers

At `8ddff6f`: every test under ASan with UBSan (`-fno-sanitize-recover=undefined`), T1
at 10,000 crash points; every test under TSan. ASan found a use of freed memory in three
new tests (a segment over a temporary string); `segment(std::string&&)` is deleted so the
mistake cannot compile.

## Coverage

`llvm-cov` over every test and both replayed corpora:

| File | Target (lines / branches) | Lines | Branches |
|---|---|---:|---:|
| `reader.hh` | 98 / 95 | 98.85% | 94.57% |
| `format.hh` | 98 / 95 | 100% | 98.30% |
| `files.hh` | 98 / 95 | 100% | 99.18% |
| `search.hh` | 95 / 90 | 97.81% | 92.33% |
| `searcher.hh` | 95 / 90 | 97.70% | 91.18% |
| `writer.hh` | 90 / 80 | 98.69% | 88.43% |
| `index.hh` | 90 / 80 | 98.70% | 89.71% |
| `merge.hh` | 90 / 80 | 100% | 95.71% |
| `merge_policy.hh` | 90 / 80 | 93.75% | 87.70% |
| `check.hh` | 90 / 80 | 100% | 87.50% |
| `budget.hh` | 90 / 80 | 100% | 85.00% |
| `faultfs.hh` | 90 / 80 | 92.78% | 82.32% |

Corruption checks (C13): `segment_test` reaches the open-time checks with sections
replaced at any length and the file re-laid out (footer bounds, stats, contiguous and
monotone docids, versions, the key filter, each sub-condition), crafted bytes for the
term-info and impact decoders, dictionaries rebuilt with values the postings cannot
hold, and a sweep that changes every byte of a small segment's postings and positions
(bitset, FOR, and wdf blocks over two skip groups; stepped position blocks), reseals it,
and reads everything: 28,748 changes, 3,408 fenced, the rest answering. Three checks
that could never fire were removed with the reason in place. Open:

- `or_bitset`'s span, byte, and count checks (`first < base` guards the caller's window
  and cannot come from data; the other two repeat `load_bitset`'s, which are reached).
- A FOR block whose ordinals use every byte, leaving none for wdfs, and a decoded first
  ordinal of `no_more_docs`: each needs two coordinated changes to skip data and block
  bytes.
- A position block's offset past the section while stepping, and a block whose size
  cannot be read there: the sweep steps position blocks but reaches neither.

## Changes for the architect

- `recover --to` commits the chosen manifest as a new generation (D3), above.
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

- C1–C4 need Track C's split steps 1–3: steps 1 and 2 landed on 2026-10-13
  (`metrics`, `completion`, `journal`, `consensus`); step 3, the peer transport into
  `consensus`, is next.
- The bare-metal machine is a MacBook Pro M4 (arm64, macOS; BENCHMARKS rule 4); strata
  has not yet been built or measured there.
- `reader.hh` branches 0.43 points short of 95%; the seven guards above.
- TSan over flush and merge threads and T2's merges racing deletes, once D18's
  background merges exist.
