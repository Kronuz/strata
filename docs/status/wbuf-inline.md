> Status: CURRENT (2026-11-04) workstream perf/wbuf-inline, developer; review fixes done, ready for the re-check.

# wbuf-inline: the write buffer's records hold their key and stored bytes inline, for every layout

Task: finish explore/wbuf-bytes (strata:bec2c09, `docs/perf/wbuf-bytes.md`) as one shape: no switch, no
`document_layout`-only path. Branch perf/wbuf-inline off main 9e344d8. Results: `docs/perf/wbuf-inline.md`;
how it works: `docs/mechanisms/write-buffer.md`.

## State

Fix round for the code review's REJECT (`docs/reviews/wbuf-inline-code.md`, strata:a6c7630) done and
pushed: CR1 and CR2 fixed, the lead's estimate ruling applied, D3's varint lengths adopted. Ready for
the rechecker against the review's probes. CI not watched (the lead watches).

## Done

1. strata:55773d2, 58d40e5, 48fd9f1, 6f26aa2: the inline record for every layout, tests, notes, first
   measurements (log below, 2026-11-04 first round).
2. strata:0b760d8 (CR1, CR2, the estimate, D3): `record::emplace` is the only way into the lane and
   bounds every record by the chunk, with the whole-record fallback for tombstones and inverting
   records too; `wbuf::lane::append_n` throws `logic_error` past a chunk in every build; `lane_fits`
   counts a record past a chunk at its whole stride. Inline is opt-in (`inline_kind`,
   `inline_layout()`), `writer::add_stored_view` refuses at compile time a layout whose builder it
   would skip. The estimate charges an inline record its stride. Varint lengths after the fields.
3. strata:3858928: bytes per record, records per flush, the `wbufw` A/B set.
4. Proof of the fix round (log 2026-11-04 fix round): revert checks for CR1 (Release and ASan), CR2
   and the estimate; mutants M1-M3 caught; Release and ASan+UBSan ctest 69/69 on macOS; prospect
   main against the branch: churn and dump identity at flush_threads 1 and 2; Linux VM.

## Next

1. Rechecker: CR1 and CR2 against `.scratch/review/` probes (reruns in `.scratch/probes/`).
   CI: pushed strata:3858928 and the ledger commit after it; no job watched.
2. The lead: CI (Linux TSan included), the charge-lane proposal (Blocked on 2), the architect question.

## Blocked on

1. Architect: values past about 990 B (a record past the 1 KiB first chunk) stay whole, a second
   shape at a common size (review note 3). Option: a record takes a chunk sized to it, up to
   `max_chunk`, leaving whole records for opted-out parts only. Measured cost of the current shape:
   1500 B values 1660.7-1666.3 B/record resident, the same as main (no saving).
2. Lead (explained by the performance engineer, `docs/perf/wbuf-inline/peak-rss.md`): the peak
   RSS rise in `flush_cadence` is freed lane chunks libmalloc keeps resident (clean, reusable
   medium-zone pages; footprint peak unchanged; `MallocSpaceEfficient=1` removes it). Beneath it the
   budget misses the lane's open chunk (D3 says chunks are charged as taken): strata's uncharged
   heap at the peak went from at most 0.9 MiB to 1.2-2.8 MiB. Proposed: `charge-lane.patch` (charge
   `lane.bytes()` for every layout; keeps the saving, 0.2-0.35 MiB uncharged after it with
   `file-sink-reserve.patch`). The lead decides whether the developer applies it here; it needs
   `defer_io_test`'s identity hash and a ctest run.
3. Lead / prospect's owner: prospect's `test/index_test.cc:1469` (its copy of the budget case)
   expects more than 2 segments after 4,000 writes; the branch's smaller inverting records flush less
   often, so it fails against the branch (log). Fix in prospect when it repins: more writes, as
   strata's copy got here. Not a strata defect; the digests in the same test match.
4. Owner/architect, unchanged from round one: layouts whose parts carry more than the stored bytes
   (prospect's `flush_threads > 1` whole-document mode) keep whole records; the review ruled (a)
   for this branch.

## Design vs code

See "Design vs code" in `docs/mechanisms/write-buffer.md`: the design's 143 B omits the 3 B of
padding its own layout has (code: 146 B, 152 B in the lane); the ordinal field; parts beyond key and
stored kept whole unless opted in; values past about 990 B whole (open, above). The lengths are the
design's varints since strata:0b760d8.

## Log

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
