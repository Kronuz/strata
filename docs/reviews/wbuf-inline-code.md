# Code review: perf/wbuf-inline (inline write buffer records)

> Status: CURRENT (2026-11-04) code reviewer, one review of strata:6f26aa2 (off main 9e344d8;
> `HEAD` equals `origin/perf/wbuf-inline`, nothing unpushed). Ruling: **REJECT** until CR1 and
> CR2 are fixed; one fix round, then a rechecker against the probes below.

Read: `docs/status/wbuf-inline.md`, `docs/perf/wbuf-inline.md`, `docs/mechanisms/write-buffer.md`,
`docs/design/write-buffer.md` D3, `docs/reviews/calibration.md`, and the diff of `index.hh`,
`writer.hh` and the tests.

## Blocking

### CR1. A tombstone's or an inverter's record is inline with no chunk check: heap overflow

`push_record` (`include/strata/core/index.hh:4043`) appends a tombstone, and every record of an
inverting layout, with `emplace_inline(..., key, {})` and no bound. The document path checks
`inline_stride(...) <= wbuf::min_chunk` and falls back to whole (`:4056`); this path does not.
Keys are valid up to `format::max_term_bytes` (fst's `uint32_t` max), so a key past about 980 B
makes a record past the chunk `append_n` opens. `append_n` asserts `bytes <= min_chunk`
(`write_buffer.hh:388`), which Release compiles out, and then constructs the record past the
chunk's end. On main these records were whole, a fixed 80 B stride, with the key on the heap.

`lane_fits` (`index.hh:4366`) caps the widest stride at a chunk "since a record past a chunk is
held whole", which is false for these records, so the CR9 room check also under-counts them.

Evidence, ASan+UBSan RelWithDebInfo (NDEBUG, as Release):

| Probe | main 9e344d8 | branch |
|---|---|---|
| `probe_long_key 500 erase` | ok | ok |
| `probe_long_key 2000 erase` (delete as a fresh lane's first record) | ok | heap-buffer-overflow, WRITE of size 2000 in `record::record` from `push_record` |
| `probe_long_key 5000 erase` | ok | heap-buffer-overflow, WRITE of size 5000 |
| `probe_long_key 1500 cap` (`lane_chunks_cap = 2`, three long-key deletes) | ok | heap-buffer-overflow |
| prospect `churn` with one 2000 B id (`churn-longkey.ops`, inverting layout) | `C 1`, `Q 1`, exit 0 | heap-buffer-overflow, WRITE of size 2001, abort |
| prospect's own `index` and `index_mapped` tests (a 1 MiB + 1 key) | not run against main | heap-buffer-overflow, WRITE of size 1048577 in `record::record` from `push_record`, abort |

So on prospect's default (inverting) path a single document with a 2 KiB key corrupts the heap.
No test has a key past about 300 B in a tombstone or an inverting record
(`inline_records` uses at most 160 B of key padding and erases only `key3`/`key9`).

Fix: give these records the same fallback as documents (a whole key-only `document` when the
inline stride passes a chunk), so the `lane_fits` cap becomes true; or bound `lane_fits` by the
real stride and let `append_n` open a chunk large enough. A regression test: long-key deletes as a
fresh lane's first record and under a low `lane_chunks_cap`, plus a mutant that drops the check.

### CR2. The inline default drops a stored-derived part's own members and skips its builder

`inline_parts` (`index.hh:2319-2330`) inlines any part derived from `stored_plugin::part` unless
its plugin declares `inline_part` and that says no. The inline flush then calls
`add_stored_view` (`writer.hh:517`), which reaches only plugins derived from `stored_plugin`, and
only through `stored_builder::add_view`, never the plugin's own `add(ord, fields, part)`.
`materialize()` (used by the digest) keeps only key and stored bytes. So a plugin that extends
the stored part and forgets `inline_part` loses its members silently at the flush, and a plugin
whose part derives from the stored part without deriving from `stored_plugin` is skipped. Both
were correct on main (every record whole, every plugin's `add` called).
`test/fields_test.cc:44` is exactly this pattern and passes only because the branch added
`inline_part` to it.

Evidence: `probe_extra_part` (a plugin whose part adds `extra` and whose builder records it in
`add`): main 3 of 3 `add()` calls with each `extra`; branch 0 of 3.

Fix (one line in spirit): inline only when every part is exactly `stored_plugin::part`, or the
plugin opts in with `inline_part`, and require such plugins to derive from `stored_plugin`; a
plugin's `add` that `add_view` would bypass should fail to compile rather than be skipped.

## Non-blocking notes

1. The flush threshold still charges `sizeof(document) + 96 + part_bytes` per document
   (`doc_bytes`), above an inline record's real stride, so flush points match main (prospect
   churn: identical segment counts at every commit) and the saving shows as RSS headroom, not as
   more records per flush. I found no new under-count: every inline record is at most main's
   whole record plus its heap strings (within 8 B when both strings were short enough for libc++'s inline buffer), and a chunk's tail waste
   is under one record. The long-key tombstone under-count (estimate 80 B, real 40 B + key) is the
   same on main, where the key sat on the uncharged heap. Charging real strides is the lead's
   call (it moves flush cadence; measure under sustained load).
2. Design D3 has varint lengths at 28/32 and a 24-byte header; the code has a u32 pair at 32/40.
   The mechanism note records it; the architect amends D3 or asks for varints.
3. The 1 KiB inline bound keeps values past about 900 B whole: a second shape for a common size.
   The lane already doubles chunks; a record could take a chunk sized to it up to `max_chunk`,
   leaving whole records for parts only. Architect question, with a bytes/record number for
   1-4 KiB values.
4. A rejected transaction's inline records no longer free heap strings, so ASan stops seeing a
   reader that reads a truncated record; the design forbids that read (WB-9). Detection, not a bug.
5. `quiet light` exited 1 (`quiet: line 432: =: command not found`, `line 433: r: unbound
   variable`) after ninja finished the prospect build; the build was complete. For the dotfiles owner.

## The developer's open question (ledger "Blocked on" 1)

Ruling: (a), keep such documents whole in this branch, with CR2's safer default. No layout that
ships has a stored-derived part with data of its own: prospect's `search_layout` uses the plain
`stored_plugin`, and its whole records come from `inverted_plugin`'s part in the
`flush_threads > 1` mode, a different case. A per-plugin serializer (b) for a test-only shape
would add a mechanism nobody uses. The one-shape rule applies to prospect's whole-document mode,
where the gap is measurable (main's 255 B against 198 B per record here suggests about 22%):
measure prospect's `flush_threads > 1` bytes per record first, then the architect picks (b) or
another way. Option (c), dropping that mode, narrows user-visible behavior and is the owner's call.

## What was run

1. strata ASan+UBSan RelWithDebInfo build (`.scratch/review/asan`), ctest 69/69 passed.
2. Probes above, each against the branch and main 9e344d8 (`strata-main/` from `git archive`).
3. prospect main (prospect:0744e51) built with `-DFETCHCONTENT_SOURCE_DIR_STRATA` against the
   branch (`p-branch`, ASan+UBSan) and against strata main (`p-main`, `prospect_engine` only):
   compiles unchanged. prospect ctest against the branch, ASan+UBSan: 49/51; `index` and
   `index_mapped` abort on CR1 (`p-branch-ctest.log`). prospect's tests were not run against
   strata main.
4. Dump identity: `prospect_engine churn` on 30,000 upserts and deletes with 12 commits and COUNT
   and TOP_10_COUNT queries (`churn-plain.ops`), then `dump` of six queries at k = 10, 100, 1000
   under both schemes: churn results and segment counts (132 lines) and dump (36 lines)
   byte-identical between main and branch. This covers the inverting path (one flush thread).

Probes and inputs, all in `.scratch/review/` on the branch worktree: `probe_long_key.cc`,
`probe_extra_part.cc`, `build_probe.sh`, `build_probe_main.sh`, `gen_churn.py`,
`churn-plain.ops`, `churn-longkey.ops`, `dump-queries.jsonl`, `churn/` (outputs), logs
`asan-ctest.log`, `p-branch-ctest.log`.

## Verdict

**REJECT.** Blocking: CR1 (heap overflow on a long-key delete or any long-key inverting record),
CR2 (silent loss of a stored-derived part's members at the flush). Conditions for landing after
the fix round: regression tests and dying mutants for both; prospect ctest and the churn identity
rerun on the fixed branch; CI green on every platform, Linux TSan included (calibration
2026-10-29).

Not run: TSan (the branch adds no synchronization; the developer's macOS TSan 69/69 stands, and
Linux CI TSan is the authority); Linux, FreeBSD and arm64 builds (CI); prospect's
`flush_threads > 1` churn identity (its tests cover the mode); throughput and bytes per record
(no benchmarks by the reviewer); fuzzing.

## Re-check

> Status: CURRENT (2026-11-04) rechecker, strata:0b760d8 (fixes), strata:3858928 (measurements),
> strata:7791520 (ledger; HEAD, equals `origin/perf/wbuf-inline`). Scope: CR1 and CR2 only.

| Finding | Verdict | Evidence |
|---|---|---|
| CR1 | **CLOSED** | `record::emplace` (`index.hh:2669`) is the one append path: inline only when `inline_ok && n <= wbuf::min_chunk`, else a whole record; tombstones and inverting records go through it (`index.hh:4073`). `append_n` throws `std::logic_error` in every build (`write_buffer.hh:388`). `lane_fits` (`index.hh:4396`) counts the real stride. |
| CR2 | **CLOSED** | Inline is opt-in: `inline_kind` / `inline_layout` (`index.hh:2322`, `:2331`) accept the stored plugin's exact part and builder, or `inline_part` with a `static_assert` that the plugin derives from `stored_plugin` and keeps `stored_builder`; `add_stored_view` (`writer.hh:529`) static-asserts `views_stored` for every entry, and the flush calls it only under `if constexpr (inline_layout())`. |

Probes, rebuilt from `.scratch/review/` against the fixed headers (ASan+UBSan, RelWithDebInfo,
`-O2 -DNDEBUG`, so Release asserts are compiled out; outputs in `.scratch/review/recheck/`):

1. `probe_long_key` erase 500, 2000, 5000, 70000 B: ok, exit 0 (branch before: heap-buffer-overflow
   at 2000 and 5000).
2. `probe_long_key` cap (`lane_chunks_cap = 2`, three deletes) at 1500, 2000, 5000, 70000 B: verdict
   accepted, exit 0 (before: heap-buffer-overflow).
3. prospect `churn` on `churn-longkey.ops` (one 2000 B id, inverting layout), `p-branch` rebuilt:
   exit 0, `C 1`, `Q 1` rows as on main (before: heap-buffer-overflow, abort).
4. `probe_extra_part`: 3 of 3 builder `add()` calls with `extra0`, `extra1`, `extra2` (before: 0 of 3).
5. prospect's `index` and `index_mapped` no longer abort in `record::record`; they get past the
   1 MiB + 1 key case and fail later (NEW-1).
6. strata ASan+UBSan: `write_buffer`, `write_buffer_reads`, `fields`, `index`, `index_mapped`,
   `defer_io`, `defer_io_mapped`, `flush_memory`, `flush_memory_mapped`, `key_index`, `index_prereq`,
   `fuzz_fields_replay`: all pass (12 of 12 by ctest, plus the three test binaries run directly).

Mutants, each built against a copy of the headers (`recheck/mut.py`, `recheck/mutbuild.sh`),
tests unchanged:

| Mutant | Test | Result |
|---|---|---|
| `emplace` without the `n <= min_chunk` bound (m1) | `long_key_records` alone | fails: `long key 1000/2000/5000: strata: a write buffer record past a chunk`, and the cap case; so the Release size check throws |
| m1 | `record_strides` (`write_buffer_reads_test`) | aborts on the same `logic_error` |
| m1 plus `append_n`'s check removed (the pre-fix Release behaviour) (m2) | `long_key_records` alone | heap-buffer-overflow in `record::record` from `apply` |
| `lane_fits` ignoring key and stored sizes (m3) | `long_key_records` alone | survives (the long keys are held whole, so `lane_fits` is not what bounds them) |
| m3 | `write_buffer_test`, whole | `chunk_limit_sized_transactions` dies (`length_error: chunk directory is full`), so the existing test covers the room check |
| every layout inline, `static_assert`s kept (m4) | `fields_test` | does not compile: `add_stored_view would skip a plugin's own add` |
| m4 with both `static_assert`s removed (the pre-fix runtime behaviour) (m5) | `own_builder_parts` | fails at `fields_test.cc:708` (`seen` empty) |

## New findings

NEW-1 (downstream, not a defect of the fix). prospect main (prospect:0744e51) `test/index_test.cc:1469`,
`CHECK(a.segments().size() > 2 && b.segments().size() > 2)`, fails on the fixed branch for both
`index` and `index_mapped`: it is the same budget case that strata's own `index_test` needed 16000
records for (real-stride charging moves the flush points). prospect's pin of strata needs the same
change to that test (more records, or a smaller budget) before it can take this branch. Not
re-checked past that line: the test stops there.

The four changes the prompt asked about, read in the touched hunks and covered by the runs above:
one append path with a whole-record fallback (`emplace`), the opt-in inline part, real-stride
charging (`index.hh:4103`; a whole record still charges `doc_bytes`, which includes its key), and
the varint lengths (`varint_bytes`, `index.hh:2622`; `lengths()` decodes at most two bytes per
length since an inline record fits a chunk). I found no new defect in them.

Not run: the full strata ctest (a full ASan build is about 89 targets; only the targets above
were built), prospect's full ctest, the prospect churn dump identity (the flush points moved, so
segment counts change by design), TSan, Linux, FreeBSD and arm64 (CI), benchmarks, and
fuzzing. The measurements in strata:3858928 were not re-measured.
