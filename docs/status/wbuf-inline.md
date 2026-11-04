> Status: CURRENT (2026-11-04) workstream perf/wbuf-inline, developer; lane charge applied, ready for the lead to land.

# wbuf-inline: the write buffer's records hold their key and stored bytes inline, for every layout

Task: finish explore/wbuf-bytes (strata:bec2c09, `docs/perf/wbuf-bytes.md`) as one shape: no switch, no
`document_layout`-only path. Branch perf/wbuf-inline off main 9e344d8. Results: `docs/perf/wbuf-inline.md`;
how it works: `docs/mechanisms/write-buffer.md`.

## State

Review blockers closed by the re-check (`docs/reviews/wbuf-inline-code.md`, strata:f52c654). The
performance engineer's two patches are applied (strata:8b1de41) with proof, sanitizers, Linux and
measurements (log, landing round). prospect's companion test change is pushed on prospect's
`test/wbuf-inline-flushes` (prospect:c0c4260), not merged. Not landed: the lead lands strata, then
repins and lands prospect. CI not watched.

## Done

1. strata:55773d2, 58d40e5, 48fd9f1, 6f26aa2: the inline record for every layout, tests, notes, first
   measurements (log below, 2026-11-04 first round).
2. strata:0b760d8 (CR1, CR2, the estimate, D3): `record::emplace` is the only way into the lane and
   bounds every record by the chunk, with the whole-record fallback for tombstones and inverting
   records too; `wbuf::lane::append_n` throws `logic_error` past a chunk in every build; `lane_fits`
   counts a record past a chunk at its whole stride. Inline is opt-in (`inline_kind`,
   `inline_layout()`), `writer::add_stored_view` refuses at compile time a layout whose builder it
   would skip. Varint lengths after the fields.
3. strata:3858928: bytes per record, records per flush, the `wbufw` A/B set.
4. Proof of the fix round (log 2026-11-04 fix round).
5. strata:8b1de41: `charge-lane.patch` and `file-sink-reserve.patch`: the buffer charges the lane's
   chunks for every layout plus what whole records hold beyond the lane; `holds_rejected` looks at
   the lane; `file_sink` reserves its gather once. The underflow in `doc_bytes - stride` is proved
   impossible by a `static_assert` (widest whole stride 40 + `sizeof(document)` within the smallest
   `doc_bytes`, `sizeof(document)` + 96) and an `assert` at the push; `lane_charged` tests it.
   `defer_io_test`'s identity, `background_merge_test`'s pressure bound and
   `rejection_beside_readers`' peak bounds updated, each explained in the commit.
6. prospect:c0c4260 on `test/wbuf-inline-flushes`: `test/index_test.cc:1457` writes 16,000 records.

## Next

1. The lead: CI for strata:8b1de41 and the ledger commit after it (pushed; jobs at stop below), land
   strata, then repin prospect to it and land prospect's branch.
2. The lead or architect: Blocked on 1 and 2.

## Blocked on

1. Architect: values past about 990 B (a record past the 1 KiB first chunk) stay whole, a second
   shape at a common size (review note 3). Option: a record takes a chunk sized to it, up to
   `max_chunk`, leaving whole records for opted-out parts only. Measured cost of the current shape:
   1500 B values 1660.7-1666.3 B/record resident, the same as main (no saving).
2. Lead / architect (new, from the landing round): the lane's chunks double from 1 KiB and are
   charged whole when taken, so at small thresholds a flush can hold about half the records the
   share allows. `background_merge_test`'s pressure flush (64 KiB effective threshold, 200 B
   values, 2000 writes) makes 15 segments with the charge against 8 on the branch before it and 11
   on main: there the branch flushes 1.36x more often than main. At shares of 1 MiB and above the
   per-record saving holds (log). It matters for the many-indexes workload's small buffers.
   Options: (a) accept: the charge is what the lane holds; (b) charge the open chunk by its used
   bytes, which leaves up to one chunk uncharged again; (c) size the first chunks from the share
   (for example a first chunk of share/8), so a small buffer does not double through its share.
   Not decided here.
3. Owner/architect, unchanged from round one: layouts whose parts carry more than the stored bytes
   (prospect's `flush_threads > 1` whole-document mode) keep whole records; the review ruled (a)
   for this branch.

## Design vs code

See "Design vs code" in `docs/mechanisms/write-buffer.md`: the design's 143 B omits the 3 B of
padding its own layout has (code: 146 B, 152 B in the lane); the ordinal field; parts beyond key and
stored kept whole unless opted in; values past about 990 B whole (open, above). The lengths are the
design's varints since strata:0b760d8.

## Log

### 2026-11-04 landing round (lane charge)

- **Revert check and mutants** (`.scratch/land-mutants.sh`, log `.scratch/land-mutants.log`,
  `write_buffer_reads_test`): reverting `buffered_bytes()` to the branch's form fails `lane_charged`
  (3 checks) and `rejection_beside_readers`; M1 (`b.bytes += doc_bytes`, no stride) fails
  `lane_charged`'s whole-record check; M2 (`holds_rejected` without the lane) fails the rejected-only
  buffer's peak bound. Restored: 0 failures.
- **ctest, macOS**: Release 69/69, ASan+UBSan 69/69, TSan 69/69 (`.scratch/land-ctest-*.log`).
  First Release run: `background_merge` and `background_merge_mapped` failed the pressure-flush
  bound (15 segments against at most 14), `defer_io_test` its identity, `write_buffer_reads_test`
  its rejection bounds (peak 523,776 B against 327,680 B: the 256 KiB chunk taken past the share);
  each updated and explained in strata:8b1de41.
- **Linux** (local VM, `tools/vm/aws-linux.sh run` from the main checkout, no AWS;
  `~/.scratch/aws-linux/local-20261104T192808Z-15006`): RelWithDebInfo ctest 70/70, ASan+UBSan 70/70.
- **Uncharged heap and records per flush** (`flush_cadence`, merges on, 400,000 writes; main (A),
  the branch before the charge (B), after it (F); deterministic, 2 trials identical; under
  `quiet strict wbufland`; `docs/perf/wbuf-inline/land-measure.sh`, raw `land-cadence.txt`).
  Strata's uncharged heap at the peak is heap peak - budget peak - 1.06 MiB (the probe's generator):

  | Value, share | A records/flush | B | F | F/A | B uncharged MiB | F uncharged MiB |
  |---|---:|---:|---:|---:|---:|---:|
  | 100 B, 8 MiB | 32,264 | 55,189 | 55,173 | 1.71x | 1.19 | 0.20 |
  | 500 B, 8 MiB | 12,711 | 15,197 | 15,185 | 1.19x | 2.78 | 0.34 |
  | 500 B, 32 MiB | 50,841 | 60,788 | 58,862 | 1.16x | 1.37 | 0.34 |
  | 1024 B whole, 32 MiB | 28,340 | 28,340 | 27,545 | 0.97x | -0.28 | -1.09 |
  | 100 B, 1 MiB | 4,033 | 6,899 | 6,887 | 1.71x | 1.33 | 1.33 |
  | 500 B, 1 MiB | 1,589 | 1,900 | 1,892 | 1.19x | 0.85 | 0.87 |

  Target met at 8 and 32 MiB: 0.20-0.34 MiB uncharged, 1.16-1.71x the records per flush of main.
  At 1 MiB the charge changes little (the lane's chunks sum to just under 1 MiB) and the uncharged
  heap stays 0.87-1.33 MiB, not attributed here. Digests identical across arms.
- **Write rows A/B** (`bench/kv/ab.py rows OUT wbufw 11`, B (before the charge) as A, F as B, then
  B against itself, 11 interleaved trials, under `quiet strict wbufland 20 "wbuf budget A/B"`;
  `land-ab-report.txt`, `land-aa-report.txt`): fillrandom 100 B tie (0.990 [0.965, 1.015]);
  readrandom 100 B 1.07x slower (0.938 [0.900, 0.968]); ycsb_a 100 B tie (0.919 [0.630, 1.109]);
  ycsb_a 1024 B tie (0.993 [0.967, 1.030]). The A/A: three ties and ycsb_a 100 B 1.04x slower
  (0.965 [0.954, 0.997]), so the floor here is about 4%; readrandom's 6% sits just past it. A TSan
  ctest ran beside it on the light cores. Branch numbers, not a gate row.
- **prospect** (prospect:c0c4260 against this branch, `-DFETCHCONTENT_SOURCE_DIR_STRATA`): ctest
  51/51; churn and dump output identical to prospect on strata main (the reviewer's
  `.scratch/review/p-main`, same engine sources) at flush_threads 1 and 2
  (`prospect-wt/wbuf-inline-flushes/.scratch/dump-id.txt`).
- `file_sink`'s reserve: no test of its capacity; an append never passes `gather` by construction
  (a write that would is drained first, one at least `gather` is passed through).

### 2026-11-04 peak RSS (performance engineer)

- Attribution with exact peaks (max RSS, footprint peak, live heap peak, budget peak), A/B/A-A 5
  interleaved trials under `quiet strict wbufrss`, allocation sites at the heap peak, `vmmap`, and
  `MallocSpaceEfficient=1` arms: `docs/perf/wbuf-inline/peak-rss.md` and the files beside it.
- H1 (bigger flush working memory) removed as the cause: about 0.5 MiB of flush structures. The
  rise is retained free medium-zone pages; the budget's real gap is the lane's open chunk and the
  file sink's uncharged gather regrow (pre-existing).
- Fix prototyped in `.scratch/fix-src` and `.scratch/fix2-src` (patches in docs), not applied to the
  branch's code; Linux VM run in the note.

### 2026-11-04 fix round (CR1, CR2, estimate, D3)

- **Reproduced** the review's probes on the fix: `probe_long_key` 500/2000/5000 erase and 1500 cap
  all `ok` under ASan+UBSan; `probe_extra_part` 3 of 3 builder adds (`.scratch/probes/*.log`).
- **Revert checks** (`.scratch/probes/mutants.py`, copy of the tree in `.scratch/mut-src`):
  R1a (tombstones inline unbounded, lane check kept): `write_buffer_test` `long_key_records` fails
  in Release with `logic_error` (7 sizes). R1b (bound and lane check both reverted,
  `.scratch/mut-asan`): the probes abort with heap-buffer-overflow, WRITE of size 2000/5000/1500.
  R2 (the old inline rule, static checks removed): `fields_test` `own_builder_parts` fails. The
  estimate reverted alone: `defer_io_test`'s identity hash returns to main's `0xbdaed060c9a3c0ca`,
  so the new hash `0xdbed32b9cfd54b0f` is the flush points alone.
- **Mutants**, each caught: M1 `lane_fits` capping at a chunk (over-count): `long_key_records` cap
  case rejected; M2 varint byte count wrong: `write_buffer_reads_test` `record_strides` and
  `body_is`; M3 inline charged a quarter of its stride: `index_test` budget case and
  `defer_io_test` identity.
- **Tests changed:** `defer_io_test` identity hash updated (attribution above);
  `index_test`'s budget case writes 16,000 records instead of 4,000 so each index still flushes
  several times (7 segments each; 4,000 left 1).
- **Sanitizers:** Release ctest 69/69; ASan+UBSan RelWithDebInfo 69/69, no reports (macOS,
  Homebrew LLVM). TSan not rerun: no synchronization changed (CI's Linux TSan is the authority).
- **prospect** (prospect main, `.scratch/review/prospect-src`) built against the branch with
  ASan+UBSan (`.scratch/p-fix`): churn on `churn-plain.ops` and `churn-longkey.ops` and dump of six
  queries, at `PROSPECT_THREADS` 1 and 2, byte-identical to prospect on strata main
  (`.scratch/review/p-main`), µs column removed: 132 and 2 churn lines, 36 dump lines; no sanitizer
  reports. prospect ctest
  (ASan+UBSan): 49/51, no sanitizer reports; `index` and `index_mapped` fail one check,
  `test/index_test.cc:1469` `a.segments().size() > 2 && b.segments().size() > 2` (4,000 writes under
  a 200 KiB budget). It still fails with the estimate change reverted, so it is the inline key-only
  record of an inverting layout (about 40 B against main's 80 B whole record) that halves the lane's
  share of the inverting estimate; the review's run aborted on CR1 before this check. strata's own
  copy of the test needed 16,000 writes; prospect's copy needs the same change (Blocked on 3).
- **Bytes per record** (`kv_proto_bufmem`, 5 interleaved rounds, RSS over 200,000 unflushed
  records, `docs/perf/wbuf-inline/bufmem-cadence-rounds.txt`), main / u32 pair / varints:
  document 100 B 231.4-237.3 / 198.4-204.0 / 190.6-196.2; document 500 B 639.2-644.8 /
  598.6-599.1 / 590.6-591.2; carry 100 B (u32 / varint) 203.7-204.3 / 190.7-196.3; carry-extra and
  1500 B unchanged (whole). Varints are the smaller: 4% smaller than the u32 pair at 100 B, 1% at
  500 B; scan and index digests identical across all arms.
- **Records per flush** (`flush_cadence`, 400,000 records, 3 rounds, deterministic counts), main →
  branch: 100 B at 8 MiB 32,264 → 55,189 (1.71x more); 100 B at 32 MiB 129,056 → 220,753 (1.71x);
  500 B at 8 MiB 12,711 → 15,197 (1.20x); 500 B at 32 MiB 50,841 → 60,788 (1.20x); 1024 B (whole)
  28,340 both. Index digests identical. Peak RSS: see Blocked on 2.
- **Linux** (local VM via `tools/vm/aws-linux.sh run` from the main checkout, per-run directory, no
  AWS; `~/.scratch/aws-linux/local-20261104T171253Z-42817`): RelWithDebInfo ctest 70/70 and
  ASan+UBSan ctest 70/70 on Ubuntu 24.04 (clang). Bytes per record not run there:
  `kv_proto_bufmem` is outside the default build and the script did not build it.
- **Throughput** (`bench/kv/ab.py rows OUT wbufw 11`, the four rows that flush under pressure,
  main against the branch, then an A/A of main, each 11 interleaved trials under `quiet strict
  wbufin2`; `docs/perf/wbuf-inline/ab2-*`): fillrandom 100 B 1.06x faster [1.015, 1.115]; ycsb_a
  100 B 1.10x faster [1.025, 1.147]; readrandom 100 B tie (1.042 [0.943, 1.139]); ycsb_a 1024 B
  (whole records) tie (1.003). A/A: all four ties, 0.993-1.013. Peak RSS in the driver equal (30/30,
  43/43, 172/173 MiB). Branch timing: functional evidence only; the lead measures on main.

### 2026-11-04 first round

Code, tests, proof and notes done and pushed. CI not watched (the lead watches). Not landed.

**Done (round one)**

1. strata:55773d2 the inline record for every layout (`index.hh` record, `push_record`, `lane_fits`,
   flush), `writer::add_stored_view`, `stored_builder::add_view`; tests for both forms, sizes through a
   chunk's edge, rejected batches, the room check with big records; `kv_proto_bufmem` by layout; the
   `wbuf` set in `bench/kv/ab.py`.
2. strata:58d40e5 flush test probes; the mechanism note.
3. strata:48fd9f1 and the commit after it: perf note, raw data, exploration-log row.
4. Proof: bytes per record per layout with an A/A twin (macOS 5 rounds, Linux 3); A/B and A/A of ten kv
   rows, 11 trials: all ties, B/A 0.987-1.075 against an A/A of 0.958-1.057; 21 scan digests identical,
   fallback sizes included; ctest 69/69 in Release, ASan+UBSan and TSan on macOS; 70/70 Release and
   ASan+UBSan on the local Linux VM; two mutants caught (room check ignoring stored bytes: abort with
   length_error; flush dropping a byte: write_buffer_test and core_kv_test fail).
5. No fuzz target: nothing here decodes untrusted bytes; the record body is written and read by the
   same code from lengths it wrote.

**Next (round one)**

1. Review (code reviewer): the writer.hh change (an inline record's views are checked against the lease;
   the whole-document path is unchanged) and the lane room check.
2. The lead: CI, and the owner's rulings below.

**Blocked on (round one)**

1. Owner/architect: layouts whose parts carry more than stored bytes (field-carrying parts with data,
   prospect's `flush_threads > 1` whole-document mode) keep whole `document` records, so they get no
   saving. Inline for them needs a per-plugin serializer for the buffered part. Options: (a) leave
   them whole (as built); (b) a `buffered_size/put/get` hook per plugin, with the flush materializing
   a document for them; (c) drop the whole-document mode in prospect. Recommendation: (a) until a
   measurement on prospect shows the cost.
2. Lead: the buffer's memory estimate (`doc_bytes`, `estimate_for`) is unchanged, so flush points match
   main; charging real strides would flush later and change flush cadence.
3. Prospect (not in this repository): repin needs `inline_part` nowhere (the default keeps its parts
   whole); untested here. Its inverting layout's records now hold the key alone; not measured.

**Design vs code (round one)**

See "Design vs code" in `docs/mechanisms/write-buffer.md`: the design's 24-byte header and 143 B
record versus 32-byte header and 156 B; parts beyond key and stored kept whole; the unchanged
estimate. Also: values past about 900 B stay whole because a record must fit the 1 KiB first chunk.
