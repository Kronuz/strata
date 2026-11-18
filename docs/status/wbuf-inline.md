> Status: CURRENT (2026-11-04) workstream perf/wbuf-inline, developer; chunk growth (ruling (c)) applied, ready for the lead to land.

# wbuf-inline: the write buffer's records hold their key and stored bytes inline, for every layout

Task: finish explore/wbuf-bytes (strata:bec2c09, `docs/perf/wbuf-bytes.md`) as one shape: no switch, no
`document_layout`-only path. Branch perf/wbuf-inline off main 9e344d8. Results: `docs/perf/wbuf-inline.md`;
how it works: `docs/mechanisms/write-buffer.md`.

## State

Review blockers closed (`docs/reviews/wbuf-inline-code.md`, strata:f52c654). The lane charge
(strata:8b1de41) and the lead's ruling (c) on Blocked on 2 (strata:c439e69, chunks grow by a
32nd of the lane) are applied with proof, sanitizers, Linux and measurements (log, chunk round).
prospect's companion test change is on prospect's `test/wbuf-inline-flushes` (prospect:c0c4260),
not merged. Not landed: the lead lands strata, then repins and lands prospect. CI not watched.

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
7. strata:c439e69: `wbuf::next_chunk`, chunks of 1 KiB first, then a 32nd of the lane from 4 KiB
   to 1 MiB (ruling (c) on Blocked on 2); `lane_growth`; main's bounds restored in
   `pressure_flush_bounded`, `defer_io_test`'s identity and `rejection_beside_readers`; the
   `wbufrr` A/B set.

## Next

1. The lead: CI for strata:c439e69 and the ledger commit after it (pushed; jobs at stop below), land
   strata, then repin prospect to it and land prospect's branch.
2. The architect: Blocked on 1 and 3.

## Blocked on

1. Architect: values past about 990 B (a record past the 1 KiB first chunk) stay whole, a second
   shape at a common size (review note 3). Measured cost of the current shape: 1500 B values
   1660.7-1666.3 B/record resident, the same as main (no saving). Not made cheap by ruling (c),
   so not built: a record sized chunk (up to `max_chunk`) is cheap only for records of one size.
   (a) A record that misses the tail opens a new chunk and abandons the tail's room, so a mix of
   small and large values wastes up to a chunk per large record, and 600 KiB values in 1 MiB chunks
   waste about 40% (each chunk holds one), all charged; whole records charge `doc_bytes` exactly.
   (b) `lane_fits` counts every record at the widest stride: with chunks sized by the record,
   a chunk holds at least one, so `room_for` can no longer be exact and a transaction with one
   large value and more than about 16,000 operations would turn invalid where it is accepted now;
   `long_key_records`' two-chunk lane case (1500 B keys) would too. A sound shape needs a second
   open position in the lane for large records (their own chunks beside the small-record tail) and
   a room check by record class: a design change to D3's lane, the architect's.
2. Closed by the lead's ruling (c), strata:c439e69 (log, chunk round).
3. Owner/architect, unchanged from round one: layouts whose parts carry more than the stored bytes
   (prospect's `flush_threads > 1` whole-document mode) keep whole records; the review ruled (a)
   for this branch.

## Design vs code

See "Design vs code" in `docs/mechanisms/write-buffer.md`: the design's 143 B omits the 3 B of
padding its own layout has (code: 146 B, 152 B in the lane); the ordinal field; parts beyond key and
stored kept whole unless opted in; values past about 990 B whole (open, above). The lengths are the
design's varints since strata:0b760d8. D3's "1 KiB doubling to 1 MiB" chunks: the code grows them by
a 32nd of the lane from 4 KiB after the first (ruling (c)); the ruling's example sized the first
chunks from the share (share/8), and the code sizes them from what the lane holds instead, which
bounds the open chunk at every share and under pressure (whose threshold moves) and keeps a
one-document index at 1 KiB.

## Log

### 2026-11-04 chunk round (ruling (c), strata:c439e69)

- **Change:** `wbuf::next_chunk`: 1 KiB first, then a 32nd of the lane in 1 KiB steps, 4 KiB to
  1 MiB; `append_n` and `room_for` share it. A first try (an eighth of the lane, 1 KiB floor)
  held one 500 B record per 1 KiB chunk: 0.87x main's records per flush at 64 KiB. Doubling
  looked good only at power-of-two shares (96 KiB: F 421 records per flush, G 605).
- **Revert check and mutants** (`docs/perf/wbuf-inline/chunk-mutants.sh`, log beside it): the
  doubling chunks restored fail `lane_growth` (2), `rejection_beside_readers` (2 peak bounds),
  `pressure_flush_bounded` and `defer_io_test`'s identity (4); M1 (a 16th) fails `lane_growth`;
  M2 (`room_for` off the lane's sizes) fails `chunk_limit_room`'s exactness (4 limits); M3
  (1 KiB chunks forever) aborts `lane_refs` with a full directory. Restored: all pass.
- **Targets:** `pressure_flush_bounded` makes 8 segments (main 11, doubling with the charge 15);
  `defer_io_test`'s identity is main's again (163 mutations, `0xbdaed060c9a3c0ca`, digest
  `0x6cc0ff118bf776d3`); `rejection_beside_readers` peaks 268,928 B, under main's bounds.
- **ctest, macOS:** Release 69/69, ASan+UBSan 69/69, TSan 69/69, no sanitizer reports
  (`.scratch/chunk/ctest-*.log`).
- **Linux** (local VM, `tools/vm/aws-linux.sh run`, no AWS;
  `~/.scratch/aws-linux/local-20261104T220554Z-33892`): ASan+UBSan 70/70; RelWithDebInfo 69/70,
  `resource_budget` `test/resource_budget_test.cc:494` (`tick against failure`'s race), code the
  branch does not touch. It fails 1 of 300 runs on main's tree too
  (`~/.scratch/aws-linux/local-20261104T232132Z-85807`): a flake on main, not this change.
- **prospect** (prospect:c0c4260 against this branch): ctest 51/51; churn and dump identical to
  prospect on strata main at flush threads 1 and 2.
- **Share sweep** (`flush_cadence`, merges on, 400,000 writes, deterministic counts, 1 trial; A
  main, F strata:8b1de41 (charge, doubling), G strata:c439e69; uncharged = heap peak - budget peak
  - 1.06 MiB generator; under `quiet strict wbufsweep`; `docs/perf/wbuf-inline/chunk-measure.sh`,
  raw `chunk-cadence.txt`). Digests identical across arms. G holds 1.07-1.67x main's records per
  flush at every share; at 8 and 32 MiB 1.16-1.67x and 0.20-0.42 MiB uncharged for 100 and 500 B
  (200 B at 8 MiB: 1.31 MiB in F and G alike, not attributed). Max RSS is noisy single-trial.

| value, share | flushes A/F/G | records/flush A | F | G | G/A | max RSS MiB A/F/G | uncharged MiB A/F/G |
|---|---|---:|---:|---:|---:|---|---|
| 100 B, 64K | 1581/950/1007 | 253 | 421 | 397 | 1.57x | 100/91/90 | 1.33/1.25/1.25 |
| 100 B, 96K | 1055/950/661 | 379 | 421 | 605 | 1.60x | 100/83/83 | 1.33/1.25/1.24 |
| 100 B, 256K | 396/233/240 | 1,009 | 1,714 | 1,666 | 1.65x | 86/100/95 | 1.26/1.29/1.29 |
| 100 B, 1 MiB | 99/58/60 | 4,033 | 6,887 | 6,663 | 1.65x | 89/98/112 | 1.30/1.33/1.30 |
| 100 B, 8 MiB | 12/7/7 | 32,264 | 55,173 | 53,748 | 1.67x | 88/74/86 | 0.86/0.20/0.20 |
| 100 B, 12 MiB | 8/4/4 | 48,396 | 82,765 | 80,178 | 1.66x | 70/93/107 | -2.76/0.06/0.08 |
| 100 B, 32 MiB | 3/1/1 | 129,056 | 213,827 | 214,641 | 1.66x | 82/85/92 | -8.13/-0.42/-0.41 |
| 200 B, 64K | 2185/1538/1632 | 183 | 260 | 245 | 1.34x | 104/115/101 | 1.28/1.33/1.09 |
| 200 B, 96K | 1459/1538/1072 | 274 | 260 | 373 | 1.36x | 116/110/106 | 1.34/1.33/1.29 |
| 200 B, 256K | 548/380/393 | 729 | 1,052 | 1,017 | 1.40x | 93/95/93 | 1.08/1.09/1.08 |
| 200 B, 1 MiB | 137/94/98 | 2,913 | 4,223 | 4,072 | 1.40x | 92/77/91 | 1.19/1.08/1.08 |
| 200 B, 8 MiB | 17/11/12 | 23,302 | 33,819 | 32,917 | 1.41x | 93/116/125 | 0.59/1.31/1.31 |
| 200 B, 12 MiB | 11/7/8 | 34,953 | 50,731 | 49,114 | 1.41x | 109/90/86 | -0.57/0.21/0.26 |
| 200 B, 32 MiB | 4/3/3 | 93,207 | 131,063 | 131,524 | 1.41x | 95/130/131 | -5.44/0.22/0.07 |
| 500 B, 64K | 3999/3508/3738 | 100 | 114 | 107 | 1.07x | 142/143/113 | 0.69/0.63/0.94 |
| 500 B, 96K | 2684/3508/2453 | 149 | 114 | 163 | 1.09x | 101/145/118 | 1.00/0.63/0.93 |
| 500 B, 256K | 1005/852/892 | 398 | 469 | 448 | 1.13x | 101/94/137 | 1.36/1.24/0.57 |
| 500 B, 1 MiB | 251/211/221 | 1,589 | 1,892 | 1,804 | 1.14x | 105/106/107 | 1.24/0.87/1.31 |
| 500 B, 8 MiB | 31/26/27 | 12,711 | 15,185 | 14,747 | 1.16x | 93/115/132 | -0.44/0.34/0.34 |
| 500 B, 12 MiB | 20/17/18 | 19,066 | 22,781 | 22,019 | 1.15x | 113/149/171 | -0.42/0.40/0.42 |
| 500 B, 32 MiB | 7/6/6 | 50,841 | 58,862 | 59,037 | 1.16x | 90/199/183 | -2.87/0.34/0.35 |
digests identical across arms: True

- **readrandom** (`bench/kv/ab.py rows OUT wbufrr 41`, 41 interleaved trials per pair, under
  `quiet strict wbufrr 20 "wbuf readrandom"`; `chunk-rr-*-report.txt`): main against G tie
  (1.020 [0.988, 1.054]); F against G, G 1.07x faster (1.074 [1.052, 1.116]); G A/A tie (1.009
  [0.969, 1.043]). F's slowdown was real and is its flush cadence, not a read-path cost: F's load
  left 5-6 segments in all 41 runs, G 1 or 3, main 2-8 (merge timing), and reads probe every
  segment. Branch numbers, not a gate row.

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
