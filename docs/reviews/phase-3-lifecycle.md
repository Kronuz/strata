# strata review: Phase 3 lifecycle rows

- Date: 2026-10-13.
- Reviewed:
  - `docs/gates/phase-3-lifecycle.md` at strata `5035146`, which reports the rows at
    `8ddff6f`.
  - `include/strata/{index,files,faultfs,merge,merge_policy,budget,check,reader,format,writer}.hh`,
    `test/{lifecycle_test,index_test,merge_test,merge_policy_test,files_test,faultfs_test,segment_test}.cc`,
    and `fuzz/` at `8ddff6f`.
  - The commits the report cites: `c48fada` (`recover --to`), `79f7dcc` (storage
    admission), `405fbc0` (merge policy), and `84d3fbe` (C13). Also the dictionary change
    that landed between the fuzz runs and the gate's commit: strata `28c7175`, with termdict
    `2473e8f` and `4e3d37d`.
  - The fuzz logs behind T6: `.scratch/fuzz-seg12.log`, `.scratch/fz5.log`, and
    `.scratch/ff.log`.
- Checked against:
  - PLAN: Phase 3, Track C, Phase 3R's start rule, "Ratchets", and "Every gate".
  - `docs/design/phase-3.md`: Commit and recovery (D1–D5), Merges (D11), Memory budget
    (D17, D20), faultfs (D19), Digest (D12), and the gate's T1, T2, T5, and T6.
  - `docs/reviews/phase-3-design.md` with its re-check, and the conditions the Phase 2 gate
    review carries into Phase 3 (`docs/reviews/phase-2-gate.md`, C13 and C16).
  - TESTING.md and `docs/testing/parity.md`.
  - Detent's `docs/LIBRARY_BOUNDARIES.md`, "Progress", at `7487c5e`.
- Checked by:
  - Building strata `852a154`, exported with `git archive`, against the pinned libraries.
    Its `include/`, `test/`, and `fuzz/` equal `8ddff6f`'s; `852a154` changes only CMake
    pins. Four builds on x86_64 macOS: Release (Apple clang 17), ASan with UBSan
    (`-fno-sanitize-recover=undefined`) and TSan (Homebrew LLVM 23), and source coverage.
  - Running every test in each build. Release: 13 of 13 pass, and `lifecycle_test` reports
    10,000 crash points over 35 workloads in 40 s. ASan and UBSan: 13 of 13, with
    `lifecycle_test` at 375 s. TSan: 13 of 13, no reports.
  - Measuring coverage over every test and both replayed corpora, as the report does.
  - Probes in `.scratch/review-p3l/probes/`, built against the same tree. Each finding
    names its probe.
  - Not run: benchmarks, anything on the gate VM, and fuzzing.

## Verdict

**REJECT: not approved yet.** The lifecycle rows do not pass at `8ddff6f`, so Phase 3R
does not start on this report. Four items, L1–L4, block it.

What holds, and reproduces:

- The commit protocol is D1's (`index.hh:990-1057`). Recovery selects, verifies, and fails
  closed as D3 specifies (`index.hh:1127-1203`).
- faultfs models what D19 asks for. Its one gap changes no row (finding 21).
- T1 passes as written: 10,000 crash points over 35 workloads, zero losses, zero
  inconsistent opens, in Release and under ASan and UBSan. After each of those 10,000
  recoveries the index also applies, merges, commits, and reopens to the same digest, with
  0 failures (`probe_t1_continue.cc`).
- T2 passes, its named sequences included.
- `recover --to` (`c48fada`) is atomic under the model, and its test cuts power at every
  mutation of its own open.
- The storage permit equals the bytes of the files on disk after commits and reopens
  (`index_test.cc:297-325`).

What blocks:

- **L1.** `install` overflows a heap buffer when a checkpoint's live-docs file disagrees
  with its manifest entry, and it commits other inconsistent checkpoints that the next open
  refuses. Phase 3R repairs replicas through `install`.
- **L2.** Coverage misses TESTING's target by the report's own figure. A check that
  `84d3fbe` removed as unreachable fires on resealed data, and a C13 test the Phase 2 review
  asked for is still missing.
- **L3.** The segment target's fuzz hours ran before the dictionary coding and the reader
  changes that the gate's commit ships.
- **L4.** Storage admission does not reserve what T5 requires. Admitted single-document
  commits hit ENOSPC on a disk of the budget's size, and a merge can write more than it
  reserved.

None changes the format. The re-check covers L1–L4 only. Phase 3R also waits for Track C's
C1–C4; split steps 1–3 landed on 2026-10-13 (LIBRARY_BOUNDARIES, "Progress").

## Rows

| Row | Evidence | Ruling |
|---|---|---|
| T1 Power loss | Reproduced. Not reached: deletes during a merge (merges are synchronous), `install` beyond 5 crash points, recovery past its sixth mutation, log positions, `create` | Pass as run; L1, conditions 1–3 and 7 |
| T1 Injected corruption | Two cases at the index level; neither reaches the checks that bind a file to its manifest entry. `install` overflows a heap buffer on an inconsistent checkpoint | Not met (L1; condition 4) |
| T2 Digest | Reproduced | Pass; merges losing documents while they run wait for D18 (condition 7) |
| T5 Disk full | The tests reproduce; admission fails for small commits and for merges | Not met as specified (L4) |
| T6 Fuzzing | `fuzz_files` stands; the segment target's hours predate `28c7175` and `84d3fbe` | Not met at the gate's commit (L3) |
| T6 Fixtures | `small.sx` at format 12, a test that rejects format 11 (`segment_test.cc:415-423`); `manifest.bin`, `live.bin`, `meta.bin` | Pass |
| T6 Sanitizers | ASan, UBSan, and TSan reproduced | Pass; flush and merge threads wait for D18 (condition 7) |
| T6 Coverage (C16) | 22 of 24 cells reproduce; `reader.hh` branches at 94.57% | Not met (L2) |
| T6 Corruption checks (C13) | A removed check fires on resealed data; the wrapped position-index test is owed; two open guards carry a reason that does not hold | Not met (L2) |
| T6 Test parity (R8) | Not re-derived row by row | Accepted as listed |

## Findings

### Power loss (T1)

1. **The harness tests what T1 defines.** A dry run counts a workload's mutations. Each
   crash point replays the workload with power cut after that many mutations, rebuilds a
   power-loss state, and requires the reopened digest to be a commit point between the last
   acknowledged commit and the last attempted one (`lifecycle_test.cc:451-538`). The digest
   reads every live posting, position, and stored record, so a torn file that still opens
   fails the check.

2. **What the workload does not reach.**
   - **Deletes during a merge.** Merges run on the writer's thread (`index.hh:254-302`), so
     no delete arrives while one runs, and D11's carry-over at install is not built. The
     report's T2 row says so; its T1 row lists merges as passing.
   - **`install`.** The test cuts power at `seed * 3 % 40` for 15 seeds
     (`lifecycle_test.cc:283-294`). Each checkpoint there is one segment with no live-docs
     file, and installing it takes 12 mutations. Five of the 15 cuts fall before its last
     mutation; the other 10 fall at or after its end (`probe_install_points.cc`). No second
     crash follows.
   - **Recovery.** The second power loss is armed after 0–6 recovery mutations at every
     third crash point (`lifecycle_test.cc:519-527`). Recovery takes 2.47 mutations on
     average and up to 22. The second loss interrupts a recovery at 1,242 of the 10,000
     points, and no cut falls after a recovery's sixth mutation (`probe_t1_recovery.cc`).
   - **Log positions.** Every T1 transaction applies at position `{}`
     (`lifecycle_test.cc:477, 498`), so the applied position Phase 3R replays from never
     meets a crash. Applied at positions, the recovered pair (digest, applied position)
     matches a commit point's at all 626 crash points of six workloads, and replaying the
     whole log after recovery reaches the uninterrupted digest (probe 5 in `probes.cc`).
     The property holds; T1 does not show it.
   - **`create`.** Crash points start after it. Power lost during its 9 mutations leaves a
     directory that `open` reports as corrupt (`corrupt_index("no manifest")`,
     `index.hh:1153`). In 57 of 72 sampled crashes a second `create` fails as well, on the
     surviving `owner.lock` or on a leftover file's name (probe 4). A replica that loses
     power while being created stays down until its directory is cleaned by hand.
   - **`release`** is not in the workload.

3. **A missing head manifest rolls back.** With the head manifest removed, `open` selects
   its predecessor, and retention then deletes the file only the lost commit named, its
   live-docs generation (`probe_corrupt.cc`). D3 selects the highest manifest present, so standalone this
   follows the design. Under groups it loses data: once the journal truncates against a
   manifest's applied position, a replica that opens an older manifest cannot replay the
   difference. Condition 5.

4. **`recover --to` drops pins and reuses revisions.**
   - The new head takes the chosen manifest's pins (`index.hh:1029, 1196`). Rolling back
     to checkpoint c1 unpins c1 and a later checkpoint c2, and three commits later retention
     deletes both manifests (probe 6).
   - Revision stays the chosen manifest's, since nothing was applied (`index.hh:1033`), so
     the next commits reuse revision numbers from the abandoned history. Xapiand keys WAL
     replay and replication on revision (I9).

   Condition 6.

5. **Retention's order is not durable.** D4 deletes a manifest "before the data files only
   it referenced" (`phase-3.md:139-140`). `retain()` removes manifests and then data files
   with no directory barrier between the two passes (`index.hh:1115-1122`), and faultfs lets
   any subset of unsynced removals survive. A crash can keep a manifest whose files are
   gone. Only `recover --to` that generation reads it, and it fails closed, so nothing is
   lost; the design states an order the code does not enforce. Condition 9.

6. **I13 is not implemented.** The design checks the header's 16-byte id against the
   manifest entry at open (`phase-3.md:89-90`). The reader checks the magic and the version
   (`reader.hh:70`). `install` writes each segment under a new id with the source's header
   bytes (`index.hh:487-491`), so the check as designed would refuse every installed
   segment. Condition 8.

### `install` and injected corruption (T1, D5)

7. **`install` checks less than `recover`.** `recover` compares each segment's document
   count with its entry (`index.hh:1168`), and each live-docs file's segment, generation,
   document count, and live count with its entry before setting any bit
   (`index.hh:1173-1176`). `install` compares size and the footer or trailing CRC
   (`index.hh:480-481`), then sets one bit per deleted ordinal in a bitset sized by the
   segment (`index.hh:493-494`).
   - A checkpoint whose live-docs file claims 4,096 documents with ordinal 4,000 deleted,
     over a 10-document segment, resealed so the manifest's size and CRC match: ASan
     reports a heap-buffer-overflow at `index.hh:494` (probe 1x in `probes.cc`).
   - A live-docs file whose live count is one below its entry's: `install` commits it, and
     the next open fails closed with "live docs disagree with the manifest" (probe 1). A
     replica that opened before `install` no longer opens after it.
   - A failure reading the source fences the target. `copy` reads the source inside
     `guarded`, which fences on any `std::system_error` (`index.hh:478, 484, 623-626`). A
     checkpoint released during the copy, or a transfer error in Phase 3R, forces the
     target to reopen.
   - D5 has `install` verify each file whole (`phase-3.md:158-159`). The segment
     constructor verifies the pinned sections; terms, postings, positions, and stored
     records are verified per region on first read.
   - `install` reserves no storage, settles no permit, and keeps `reserved_` from the
     buffer it discards (`index.hh:470-526`).

8. **Recovery's binding checks are never exercised.** Over every test and both corpora,
   the throwing branch of each check that binds a file to its manifest entry is never
   taken: size (`index.hh:1213`), footer CRC (`1166`), segment document count (`1168`),
   live-docs CRC and fields (`1171`; `1174` never runs), metadata CRC and generation
   (`1182, 1184`), and the manifest's generation against its name (`1162`). `index_test`'s
   changed file flips a footer byte, which `decode_footer`'s own checksum rejects before
   `1166` compares (`index_test.cc:481`, `format.hh:192`). Damage to each file class fails
   closed and leaves the disk unchanged (`probe_corrupt.cc`). No test places an intact file
   from another generation or another index under a name the manifest uses, which is the
   case these checks exist for.

### Disk full (T5)

9. **Admission underestimates small flushes.** A transaction reserves `estimate_bytes`
   (`index.hh:671-679`), about 180 bytes for one small keyed document. The segment its
   flush writes is 1,088 bytes, and the commit adds a manifest. On a 64 KiB disk under a
   64 KiB budget, committing each document alone, 58 transactions are accepted and the
   commit of the 58th fails with ENOSPC; no transaction ever receives `no_space` (probe 2).
   At ten documents per commit the overhead amortizes: 489 accepted, then `no_space`, and
   no ENOSPC. `index_test`'s admission case commits 3,000-byte incompressible records ten
   at a time (`index_test.cc:337-354`), where `stored.size() / 16` covers the overhead.

10. **A merge can write more than it reserves.** Two segments of 2,000 documents each, with
    contiguous docids far apart, total 3,880 and 6,120 bytes; their merge writes 10,796
    bytes (probe 3). The merged docids section is no longer contiguous, and the versions
    section's width can grow the same way. The report's "A merge reserves its sources'
    bytes, which bound its output" (`phase-3-lifecycle.md:50-51`) does not hold.

11. **No reservation covers a commit's own files.** T5 reserves "a commit's bytes"
    (`phase-3.md:416`) so that "a commit that frees space can always be written"
    (`phase-3-design.md:525-526`). The permit holds the files' bytes plus the estimates of
    open transactions (`index.hh:660-668`), and a flush zeroes `reserved_`
    (`index.hh:984`) before the commit writes its live-docs, metadata, and manifest files.

12. **ENOSPC never strikes mid-file.** faultfs fails a whole `write_at` before its effect
    when it would pass the capacity (`faultfs.hh:265`), and no index-level test runs with
    short writes. "ENOSPC at every 97-byte offset" steps the remaining room by 97 bytes
    (`index_test.cc:421`). Every file that fails is empty when it is removed, so D2's
    partial-file path never runs.

The safety half of T5 holds. Every ENOSPC in these tests and probes left the index
unfenced, and every retry matched the reference.

### Fuzzing (T6)

13. **The segment target's hours predate what ships.** The report's 4.3 CPU-hours are in
    the logs: 21,576 runs in 1,201 s on one worker, ending at 10:17 (`fuzz-seg12.log`), and
    455,987 runs in 2,411 s on six, ending at 13:55 (`fz5.log`), with 11,406 PCs and no
    findings. After them, `28c7175` (14:36) pinned termdict `4e3d37d`, which codes key
    suffixes over the segment's alphabet (`2473e8f`, 14:14), and `84d3fbe` (15:37) removed
    three reader checks. The segment target has run against neither.

14. termdict's coded dictionary has its CPU-hour in a commit message only ("Ten minutes on
    six workers: 3.9M runs, no findings", `4e3d37d`). No log was kept.

15. **`fuzz_files` stands.** `files.hh` has not changed since `5a8b5e3`, which precedes its
    run: 228,952,315 runs in 1,810 s on six workers, 1,127 PCs, no findings (`ff.log`).

### Coverage and corruption checks (C16, C13)

16. **Coverage reproduces and misses.** Of the table's 24 cells, 22 reproduce exactly,
    among them `reader.hh` branches at 94.57% (37 of 681 missed), under the 95% target. The
    other two come out higher, because the table predates `84d3fbe`: `reader.hh` lines at
    99.08% (8 of 873 missed), and `format.hh` branches at 98.86%. TESTING also measures over
    `parity` and `verify` on a sample index (`TESTING.md:25-27`); neither ran.

17. **A removed check fires on resealed data.** `84d3fbe` removed "position block order"
    on the premise that "a document's positions start at or after its block's first"
    (`reader.hh:1129`). The first position index is a 64-bit sum of unchecked LEB deltas
    (`reader.hh:798`) and per-document counts (`reader.hh:618`), so it can wrap.
    In a resealed wide segment whose group-0 first position is 2^64 − 64, `advance` to
    ordinal 100 reads position 37 where the file stores 101, and nothing fences
    (`probe_pos.cc`, clean under ASan). The removed check fenced that read. Ordinal 10
    still fences at `reader.hh:1070`, because its index has not wrapped yet. Reads stay in
    bounds; the segment answers wrongly where it used to fence.

18. **The Phase 2 review's last C13 item is owed:** "A resealed unit test of the wrapped
    position index (a first index near 2^64)" (`phase-2-gate.md:963`). `segment_test`'s
    sweep changes one byte at a time (`segment_test.cc:678-734`) and cannot build one.

19. **Two of the seven open guards carry a reason that does not hold.** `or_bitset`'s byte
    and count checks (`reader.hh:687, 701`) guard the path where `fill` ORs whole bitset
    blocks into a window without loading them (`reader.hh:669-673`). `load_bitset` never
    sees those blocks, so these two checks are the only ones on that path. They can fire,
    and a resealed window count reaches them. The span check (`reader.hh:684`) cannot come
    from data, as the report says: `fill` passes only blocks after one it consumed at or
    past the window's base. The FOR-block and position-block guards
    (`reader.hh:853, 860, 1130, 1133`) need coordinated changes, as the report says; they
    are not re-derived here.

20. The two bitset checks `84d3fbe` removed (`reader.hh:683, 890`) are redundant as stated.
    `decode_group` requires every block after the first to raise the last ordinal by at
    least one (`reader.hh:791`).

### Model and policy notes

21. **faultfs.** A sync that fails while the power is off restores the file's durable
    bytes before `crash()` runs (`faultfs.hh:281-290`), so the file being synced at a crash
    point is never kept or torn. strata never depends on such a file, and no row changes.

22. **Merge policy** (T3, which gates Phase 4):
    - A window skips any segment whose live bytes exceed `max_merged_bytes`
      (`merge_policy.hh:192`). A 20 GiB segment from `force_merge` is not rewritten to
      reclaim deletes until 76% of its documents are deleted (`probe_policy.cc`). D11's
      bound is 20%, and Lucene rewrites such a segment alone.
    - `merge_policy_test`'s budget check re-implements the policy's own formula
      (`merge_policy_test.cc:105-144`), so it cannot catch a difference from Lucene's.
      Lucene 10.4.0's source is not on this machine, and I did not check the defaults
      against it.

### Report text

23. Corrections for the re-check:
    - T1's row lists merges and `install` as covered (finding 2).
    - "Every third crash point loses power again during the recovery that follows": 1,242
      of the 10,000 points do.
    - T5: "Admitted work commits without ENOSPC on a disk of the budget's size"
      (findings 9–11), and "every 97-byte offset" (finding 12).
    - The coverage table predates `84d3fbe` (finding 16).
    - "Open" says Track C's step 3 is next. LIBRARY_BOUNDARIES records it as landed on
      2026-10-13 (`consensus::transport`).
    - The C13 row omits the wrapped position-index test (finding 18).

## Required changes

1. **L1. `install` validates a checkpoint the way `recover` validates the head.**
   - One function, shared by both, checks every file against its manifest entry: size,
     footer or trailing CRC, and the segment's document count; each live-docs file's
     segment, generation, document count, live count, and live length; the metadata file's
     generation; and the manifest's generation against its name.
   - `install` verifies each segment whole (`segment::verify_all`) before its manifest
     commits, as D5 says.
   - A failure reading the source leaves the target unfenced and unchanged.
   - Resealed tests, each rejected with the target unchanged and clean under ASan: a
     live-docs file with an ordinal past its segment, one whose live count disagrees with
     its entry, a segment whose document count disagrees, and a damaged postings region.
   - Power loss at every mutation of `install`, on checkpoints holding several segments,
     live-docs files, and metadata, with a second power loss during the recovery that
     follows.

2. **L2. Coverage and C13.**
   - `reader.hh` branches at 95% or more, measured as TESTING specifies (every test,
     `parity`, `verify` on a sample index, and both corpora), with the report's table
     regenerated at the re-check's commit.
   - "Position block order" restored, or `position_index` rejecting a sum that wraps, with
     a resealed test that sets a group's first position near 2^64 and reads a document
     whose index wraps. It fences.
   - A resealed test that reaches `or_bitset`'s byte and count checks through a window
     count.
   - Every check still unreached is listed in the report with a reason that holds.

3. **L3. Fuzzing at the commit the re-check reviews**, after L1 and L2 land. The segment
   target runs one CPU-hour under ASan and UBSan, with a `-timeout` of seconds and the
   pinned termdict. termdict's coded-dictionary target runs its hour with its log kept. The
   report gives runs, PCs, corpus size, and CPU time for each, and the logs stay in
   `.scratch`.

4. **L4. Storage admission does what T5 says, or T5 says what it does.** Either:
   - Each index holds a standing allowance for one commit's files: a manifest at its
     current segment count, a metadata file, and a live-docs file for each segment with
     deletes. The first reservation of each buffer adds a segment's fixed overhead, and a
     merge reserves a bound on its output that holds for docids and versions. Tests show
     single-document commits on a disk of the budget's size never hitting ENOSPC, and a
     merge of segments with distant docids staying within its reservation.
   - Or the design and the report state that admission is approximate and that recovering
     in place from ENOSPC is the guarantee, and they drop "a commit that frees space can
     always be written".

   Either way, an index-level test runs with short writes, so ENOSPC strikes mid-file and
   the partial file is removed.

## Conditions

Each lands by its stated point. None blocks Phase 3R's start once L1–L4 close.

1. **Log positions in T1.** The workload applies at positions and checks the recovered
   pair (digest, applied position) against the commit points, then replays the whole log
   and reaches the uninterrupted digest. Before Phase 3R replays from
   `applied_position()`.
2. **`create` under power loss.** Crash points include `create`. An interrupted `create`
   either runs again over its own leftovers, or `open` tells a directory that never
   committed apart from a corrupt one. Before Phase 3R creates replicas.
3. **Recovery at every mutation.** For a sample of crash points, the second power loss
   steps through every mutation of the recovery, as `recover --to`'s test does. `release`
   joins the workload, and T1 checks continued operation after each recovery (apply,
   merge, commit, reopen). Before the Phase 3 gate.
4. **The binding checks reached.** One test per check in finding 8, each placing an intact
   file from another generation or another index under the name the manifest gives. Before
   the Phase 3 gate.
5. **A floor for `open` under groups.** `open` takes the lowest manifest generation the
   journal still depends on and fails closed below it, with a test that removes the head
   manifest. Before Phase 3R's journal truncates against a manifest.
6. **`recover --to`.** It keeps the head's pins, or the design says why not. The design
   also states the revision a rollback commits, which never reuses a number the abandoned
   history used. Before Phase 3R's checkpoint references, and before Phase 7a's WAL.
7. **Background merges (D18).** When merges leave the writer's thread: T1 with deletes
   arriving during merges, T2's merges losing documents while they run, D11's carry-over at
   install, and TSan over flush and merge threads. Before background merges run in any
   build Phase 3R tests, and before the Phase 3 gate.
8. **I13.** `open` checks the header id against the manifest entry, with `install` either
   writing the local id or the manifest recording the header's; or the design drops I13.
   Before format 12 freezes.
9. **Text and small fixes**, with the re-check:
   - Retention's order (finding 5): a directory barrier between the two removal passes, or
     a design note that the order is best effort and why that is safe.
   - D12's digest after every transaction on small runs (`phase-3.md:373-374`), which no
     test compares yet.
   - The report's corrections in finding 23.
   - The merge-policy notes in finding 22, carried to the T3 gate.

## Ruling

**REJECT.** Phase 3R does not start yet. It starts when a re-check closes L1–L4 and Track
C's C1–C4 pass. Conditions 1–9 carry, each to its stated point.

## Re-check

- Date: 2026-10-14.
- Reviewed:
  - `docs/gates/phase-3-lifecycle.md` at strata `33cf29f`, which reports the rows at
    `bc10295`; `33cf29f` changes only that report and `docs/perf/reclamation.md`.
  - `git log 1ecf85b..33cf29f` and its diffs: `c266040` (L1); `28e2362` and `5e6b247`
    (L2); `d41c957`, `a3b6f18`, and `cf206b2` (L3); `48b2154` (L4); `b4f8a3b`, `90b23aa`,
    and `7ca96ad`; for the conditions, `ccb4fb1`, `914db86`, and `1616e52`; and the changes
    to the reviewed code since, `6d52f79`, `41204b6`, and `bc10295`, with the D3, D4, and
    merge text they add to `docs/design/phase-3.md`.
  - The new tooling (`bench/soak`, `bench/engines`, `bench/query_trials.sh`,
    `tools/vm/freebsd.sh`, `tools/vm/linux.sh`, `strata_engine churn`) only for whether it
    touches the reviewed code. It does not.
- Checked against: the first review's L1–L4, conditions 1–9, and findings 1–23;
  `docs/design/phase-3.md` D3–D5, D12, D17, D20, T1, T2, T5, and T6; TESTING.md; PLAN's
  Phase 3R start rule.
- Checked by:
  - Building `33cf29f`, exported with `git archive`, four ways on x86_64 macOS: Release
    (Apple clang 17), ASan with UBSan (`-fno-sanitize-recover=undefined`) and TSan
    (Homebrew LLVM 23), and source coverage.
  - Running every test in each build. Release: 14 of 14 in 108 s. ASan and UBSan: 14 of
    14 in 464 s; `lifecycle_test` prints 10,000 crash points over 31 workloads, 533 inside
    recoveries, and 10 `create` points with 9 created again; `index_test` prints 47
    one-document commits and 28 reclamation crash points. TSan: 14 of 14 in 875 s, no
    reports.
  - `tools/coverage.sh` twice at `33cf29f` with the report's corpus, ranks, and queries.
  - Each fix's tests against the code before it: the fix commit exported, its code files
    taken from its parent (`.scratch/review-p3l2/rv-<commit>/`, `probes/run_reverts.sh`,
    `probes/run_l4.sh`).
  - The termdict regression input against termdict `4e3d37d` and `0297799`
    (`probes/replay_termdict.sh`), and one libFuzzer pass over the checked-in segment
    corpus with `-runs=0` (`probes/fuzz_pcs.sh`).
  - Probes in `.scratch/review-p3l2/probes/`; each finding names its probe.
  - Not run: fuzzing beyond that one corpus pass, benchmarks, the gate VM, and the
    FreeBSD VM. The M1 is unreachable, so the fuzz logs behind T6 were not read.

### Verdict

**REJECT: not approved yet.** Phase 3R does not start on this report. L2 closes but for
one listing, L4 closes as specified, and conditions 1, 3, and 4 are met. L1 stays open on
one residual that loses a live document, and L3 lacks its record. Two new items block:
L5, merge and `install` reservations short of what they write (T5); and L6, retention
deleting a pinned checkpoint on a transient read error (D4).

What holds, and reproduces:

- `recover` and `install` check files through one function (`index.hh:1379-1408`), and
  `install` reads and verifies the whole checkpoint before anything changes, outside the
  fencing guard (`index.hh:550-571`). Without `c266040`, the first damaged checkpoint
  overflows a heap buffer in `install` under ASan.
- A wrapping position index fences at its source (`reader.hh:618, 801-802`). Without
  `28e2362`, `segment_test` fails 5 checks.
- Admission holds the commit allowance, a segment's fixed bytes, and the docid and
  version bound (`index.hh:255-269, 307-318, 751-788`). Without `48b2154`, the allowance
  checks fail and 58 one-document commits end in ENOSPC (`index_test.cc:322, 326, 412`).
  Over 240 mixed workloads (423,017 steps: adds, replaces, deletes, metadata, pins,
  auto-merge, forced merges), admitted work never takes the budget past its capacity
  (`probe_admission.cc`).
- T1 applies at log positions, replays the whole log after each recovery, then merges,
  commits, and reopens; it releases pins and cuts the recovery after each of its
  mutations at every 50th point (`lifecycle_test.cc:594-739`).
- Reclamation is crash-safe under the model with pins, releases, two predecessors, and
  256-byte truncation steps (finding 29).
- `segment_view`'s null bitmap is safe: `any_deleted` handles null (`searcher.hh:22-23`),
  and `dead` equals the set bits on every path but finding 24's.
- The sanitizer runs, the coverage targets, and the termdict fix reproduce.

What blocks:

- **L1** (residual): a resealed live-docs file can list an ordinal twice. The next commit
  drops a segment that still holds a live document; `install` commits such a checkpoint,
  and the target's next open refuses it (finding 24).
- **L3**: the fuzz logs are not in `.scratch`, and L1's fix changes a fuzzed decoder.
- **L5**: a merge can write about 1.5× its reservation, and `install` reserves neither
  the checkpoint's metadata file nor its manifest (findings 26, 27).
- **L6**: one EIO or EMFILE reading a pinned checkpoint's manifest during an open deletes
  the checkpoint (finding 28).

### Rows

| Row | Evidence | Ruling |
|---|---|---|
| T1 Power loss | Reproduced at 10,000 points under ASan; conditions 1 and 3 met; reclamation crash-safe with pins and truncation (finding 29) | Pass; condition 2 partly open, 7 carries |
| T1 Injected corruption | Five damaged checkpoints refused; one binding test per check. A resealed live-docs file with a repeated ordinal opens, then loses a document | Not met (L1) |
| T2 Digest | Reproduced; the digest after every transaction on ten seeds | Pass |
| T5 Disk full | Allowance and one-document commits reproduce; merges and `install` write past their reservations | Not met (L5) |
| T6 Fuzzing | Run at a commit whose decoders equal `bc10295`'s; no logs to read | Open on the record (L3) |
| T6 Sanitizers | ASan, UBSan, and TSan reproduced | Pass |
| T6 Coverage (C16) | Every file at its targets; `reader.hh` branches 95.05% and 95.20% in two runs | Pass |
| T6 Corruption checks (C13) | Six unreached sub-conditions, not five (finding 30) | One listing owed |

### Status

1. **L1. Partially closed.**
   - Closed:
     - `load_files` checks each segment's size, footer checksum, and document count; each
       live-docs file's checksum, segment, generation, counts, and live length; and the
       metadata's checksum and generation (`index.hh:1379-1408`). `install` adds
       `verify_all` (`index.hh:1386`) and the manifest's generation against its name
       (`index.hh:557`).
     - A bad checkpoint or a failed read throws before the guarded section
       (`index.hh:555-571`). `index_test` refuses five damaged checkpoints with the target
       unchanged and unfenced, clean under ASan (`index_test.cc:528-632`). Without the fix,
       ASan reports a heap-buffer-overflow in `install`; in Release, the refusal check
       (`index_test.cc:615`) fails and the target's reopen throws.
     - Power loss at every mutation of an install of several segments with live docs and
       metadata, with a second loss during recovery at every third point
       (`lifecycle_test.cc:462-519`). It passes without the fix too: it tests atomicity,
       which `install` already had.
     - `install` reserves the copy and drops the discarded buffer's reservations
       (`index.hh:559-571, 600`).
   - Open: `decode_live_docs` lets a delta wrap 2^64, and `dead` counts list entries, not
     bits (finding 24). The listed live-count test reaches a different check (finding 25).
2. **L2. Closed; one listing owed.**
   - `position_index` and `decode_group` reject a sum that wraps (`reader.hh:618,
     801-802`), so `decode_block`'s loop premise holds (`reader.hh:1134`). The resealed
     tests fence every wrapped read (`segment_test.cc:741-788`) and reach `or_bitset`'s
     byte and count checks (`segment_test.cc:789-851`). Without the fix, 5 checks fail.
   - Coverage meets TESTING's targets as TESTING measures it. `reader.hh` branches: 95.05%
     and 95.20% in two runs, 653 and 654 of 687 outcomes against the 653 that 95% needs.
   - Owed: one unreached check sub-condition is not listed (finding 30).
3. **L3. Open on the record.**
   - The report's hours ran at `d41c957`. From there to `bc10295`, no decoder, fuzz target,
     or pin changes; only the regression input is added. `-fno-sanitize-recover=all` is in
     the fuzz build (`CMakeLists.txt:86`, `cf206b2`).
   - The regression input aborts under UBSan against termdict `4e3d37d` ("applying
     non-zero offset 21779 to null pointer", `termdict.hh:218`) and replays clean against
     `0297799` (`probes/replay_termdict.sh`).
   - The reported 5,073 PCs fit the target: one pass over the checked-in corpus on x86_64
     reaches 5,194 (`probes/fuzz_pcs.sh`). 8 workers × 482 s is 1.07 CPU-hours.
   - Not met: the logs stay on the M1, so runs, PCs, corpus, and CPU time stand on the
     report's word; L3 asked for the logs in `.scratch`. L1's fix changes `files.hh`, so
     `fuzz_files` owes its hour at the commit that carries it.
4. **L4. Closed as specified.**
   - The standing allowance (`index.hh:757-767`), a segment's fixed bytes on the
     transaction that starts a buffer (`index.hh:258`), and a merge bound for docids and
     versions (`index.hh:311-315`) are in. On a 64 KiB disk under a 64 KiB budget, 47
     one-document commits run and none meets ENOSPC. Under 97-byte short writes, ENOSPC
     strikes mid-file and the partial file goes (`index_test.cc:452-480`).
   - The merge-bound test passes without the fix: its room steps (0, 4,000, 8,000, 16,000
     bytes, `index_test.cc:416`) skip the 10,000–10,796-byte window where the old
     reservation admitted and the disk refused. The short-write test also passes without
     it; it covers D2's partial-file path, which the code already had.
   - `7ca96ad`'s test fix is real: at `48b2154`, `index_test` ends in a
     stack-use-after-scope in `~index` under ASan.
   - The bound does not hold for postings, and `install` reserves less than it writes:
     L5.
5. **Condition 1. Met.** Positions, the recovered pair against the allowed commit points,
   and the whole-log replay (`lifecycle_test.cc:610-692`).
6. **Condition 2. Partially met** (finding 31).
   - In: `index_not_created`, and `create` over an interrupted create's files
     (`index.hh:183-221, 1274-1294`; D3 at `phase-3.md:135-139`). Without `ccb4fb1`, the
     test ends in an uncaught `corrupt_index("no manifest")`.
   - Open: a second `create` fences when `owner.lock` did not survive and a leftover did;
     `create` over a committed index without its lock succeeds and is shadowed; a committed
     index with no segment and no manifest reads as never created.
7. **Condition 3. Met.** Releases in the workload, the recovery cut after each of its
   mutations at every 50th point (533 points), and apply, merge, commit, and reopen after
   each recovery (`lifecycle_test.cc:673-736`).
8. **Condition 4. Met.** One case per binding check, each matched by its error string,
   with the disk unchanged (`index_test.cc:904-1032`). Three sub-conditions of the
   live-docs check are still never true: finding 25.
9. **Conditions 5–8.** Not due; carried.
10. **Condition 9. Partially met.**
    - Retention's order: the barrier is in (`index.hh:1228-1235`). No checked-in test fails
      without it: with the sync removed, `index_test` and 10,000 T1 points pass, while
      `probe_reclaim_crash` finds 183 manifests on disk naming missing files (0 with it).
    - D12's digest after every transaction on ten seeds: met (`lifecycle_test.cc:148,
      162`).
    - Finding 22's first bullet: met (`merge_policy.hh:188-197`; without it,
      `merge_policy_test.cc:179` fails). The second carries to T3.
    - The report's corrections: item 11.
11. **Finding 23. Partially corrected.** Corrected: T1's merges and `install`, the
    second-loss wording, the coverage table, Track C, and the C13 row. Open:
    - "Writes cut to 97 bytes with the capacity at every 97-byte step from 0 to 6,000"
      (`gates/phase-3-lifecycle.md:20`): the short-write loop steps 211 bytes to 3,000
      (`index_test.cc:455`); the 97-byte loop to 6,000 runs without short writes
      (`index_test.cc:516`).
    - "which bound its output" (`:53-55`) and T5's Pass (`:20`): L5.
    - "five guards stay unreached" (`:111`): six (finding 30).
    - "Conditions 1–4 are met ... condition 9's items are done" (`:162-164`): condition 2
      and condition 9's barrier test are open.
    - `reader.hh` 95.20% and `search.hh` 93.66% reproduce within one branch outcome
      (95.05% or 95.20%; 93.51% twice): one outcome (`reader.hh:1141`) varies between
      runs.

### Findings on the new code

24. **A repeated live-docs ordinal** (`probe_live_dup.cc`, clean under ASan).
    - The delta path checks only `v == 0` and `prev >= doc_count` (`files.hh:291-293`).
      `prev + v` wraps, so a list can descend and repeat an ordinal, and the live count is
      checked against the list's length (`files.hh:299`). `load_files` sets `dead` to that
      length (`index.hh:1396`), and the commit drops a segment whose `dead` equals its
      document count (`index.hh:1115`).
    - A 200-document segment whose live docs list 0, 2–199, then 0 again (200 entries,
      live count 0), resealed: the index opens, and Q1 is live and found by key. One commit
      drops the segment; after reopening, Q1 is gone. No step reports an error.
    - `install` of that checkpoint succeeds and drops Q1 at its own commit.
    - `install` of 0, 2, 0 with live count 197 succeeds. It rewrites the bitmap under the
      entry's count (`index.hh:583-587`), and the target's next open fails with "live
      docs: live count": finding 7's second case.
    - The decoder gap predates this review; the dead-segment drop (`41204b6`, `bc10295`)
      turns it into data loss.
25. **The binding tests reach less than they name** (`probe_binding.cc`; coverage).
    - `index_test`'s "a live count unlike its entry" (`index_test.cc:572-578`) lowers an
      entry's live count without the manifest's total, so `decode_manifest` rejects it
      ("manifest: totals") before `load_files` compares. With the total kept in step, the
      binding check fires ("live docs disagree with the manifest").
    - Over every test, three sub-conditions of that check are never true: segment, live
      count, and live length (`index.hh:1391-1392`). Each fires when reached: the other
      segment's live docs, a live count, and a live length, with totals in step.
    - `install`'s manifest-name check (`index.hh:557`) is never true in any test.
26. **A merge can write past its reservation** (`probe_merge_bound.cc`).
    - The reservation assumes that "postings, positions, stored records, and terms shrink
      or stay" (`index.hh:307-310`). Postings do not. A term with one posting in segment A
      and 127 in segment B is a tail of 1-byte deltas in B (`writer.hh:652-653`); merged,
      it is one 128-entry FOR block whose width the jump across A sets
      (`writer.hh:625-629`).
    - Wdf-0 terms (boolean terms, as Xapiand's filters use) shared by A's first document
      and B's 127: with A at 4,096 documents and 3,000 terms, the sources hold 426,460
      bytes, the reservation is 507,114, and the merge writes 762,180, 255,066 over. With
      16,384 documents and 6,000 terms: 1,730,420 against 1,163,410.
    - The output is in memory before it is written (`index.hh:331-337`), so its real size
      is known at no cost.
    - On a shared disk of the budget's size, the excess is space another index's admitted
      commit was promised; that commit then meets ENOSPC, which T5 rules out.
27. **`install` reserves the copy, not its commit** (`probe_install_space.cc`).
    - `need` counts segments and live-docs files (`index.hh:559-560`). The commit also
      writes the checkpoint's metadata file and a manifest of its segment count
      (`index.hh:1145-1176`); the standing allowance, sized for the target's own state,
      covers neither.
    - A checkpoint with 33,125 bytes of metadata, on a disk of the budget's size with 0 to
      39,000 bytes of room beyond `need` in 1,000-byte steps: admission accepts all 40
      installs, and 33 then meet ENOSPC.
    - After that ENOSPC the target holds the installed content in memory while `install`
      reports failure (`index.hh:594-611`); the next commit makes it durable (33 of 33).
28. **Retention deletes a pinned checkpoint on a transient read error**
    (`probe_pin_read_error.cc`).
    - `manifest_files` reads an older kept manifest once and turns any exception into an
      empty file list, cached (`index.hh:1245-1259`). `retain()` then removes that
      manifest as obsolete and queues its files (`index.hh:1224-1241`).
    - One EIO, or one EMFILE, on the second open of a pinned checkpoint's manifest during
      an open (the first tracks its size, `index.hh:1306`): the open succeeds, the head
      still lists the pin, and the manifest and its files are gone. Installing the
      checkpoint fails ("No such file or directory").
    - `8ddff6f`'s `retain()` has the same rule (`continue` on an unreadable manifest);
      `41204b6` keeps it and caches the empty list. D4 retains pinned manifests
      (`phase-3.md:143-144`), and Phase 3R repairs replicas from them.
29. **Reclamation holds, by the barrier** (`probe_reclaim_crash.cc`).
    - Truncation runs only on files no manifest on disk names, after the manifests' removal
      is durable (`index.hh:1228-1241`); a crash leaves orphans that the next open queues
      (`index.hh:1296-1355`).
    - 12 workloads with pins, releases, merges, two predecessors, and 256-byte steps, power
      lost at 6,853 points: 30,728 manifests on disk checked and 4,520 truncated orphans
      seen. No manifest names a missing or changed file, so `recover --to` any of them
      opens; no pin is missing; no open fails; no file is left unnamed after `reclaim()`;
      and the permit always covers the bytes on disk.
    - Without the sync at `index.hh:1235`, 183 manifests on disk name missing files, and no
      checked-in test fails. `index_test`'s 28 points use no pins or predecessors, and T1's
      files never exceed one 16 MiB step, so T1 never truncates.
30. **Coverage and C13** (`tools/coverage.sh` twice; `probe_group_last.cc`).
    - Every file meets its targets in both runs. Every figure but `reader.hh`'s and
      `search.hh`'s branches equals the report's.
    - Aggregated over instantiations, six check sub-conditions are never true in either
      run: the report's five (`reader.hh:687, 858, 865, 1135, 1138`) and `read_group`'s
      `f.last <= f.prev_last` (`reader.hh:769`). `segment_test`'s lowered group last
      (`segment_test.cc:382-413`) fences first in `top_k` ("skip group order"); a plain
      walk of the same file reaches `reader.hh:769` ("skip group").
31. **`create` and `index_not_created`** (`probe_create.cc`, `probe_create_lockless.cc`,
    `probe_uncreated.cc`).
    - With 200 crash seeds per point instead of one, 1,755 of 2,000 crashed creates open
      as `index_not_created`, and in 152 of them a second `create` fences on "File
      exists". The lock did not survive and `meta-1.kv` did, so `acquire_owner(true)`
      succeeds and `clear_uncreated` never runs (`index.hh:185-191`).
    - The same path lets `create` succeed over a committed index whose `owner.lock` is gone,
      once retention has removed its `meta-1.kv`, although `index.hh:181-182` promises
      refusal. The new index's generations 1–5 sit beside the old index's 6–8; the next
      open returns the old content, and the new index's acknowledged document is gone.
    - A commit drops segments with no live document (`index.hh:1115`), so an index whose
      documents are all deleted keeps no segment or live-docs file. With its manifests
      lost, `open` reports `index_not_created`, and `create` over it discards one metadata
      entry, last docid 2, and applied position (8, 1). D3's "no manifest and no segment
      or live-docs file never committed" (`phase-3.md:135-136`) does not hold. That
      directory held `meta-2.kv`; an interrupted `create` leaves only `meta-1.kv` and
      `manifest-1.tmp`.
32. **Smaller items.**
    - `commit()` runs `retain()` and `reclaim()` after its manifest is durable, unguarded
      (`index.hh:377-384`). A failed truncate makes `commit()` throw although the commit
      survives power loss (`probe_reclaim_error.cc`). Inside a merge's admission, the same
      failure fences the writer (`drain()` within `guarded`, `index.hh:316`).
    - `install` drops the target's pins (`index.hh:609`), as `recover --to` drops the
      head's (finding 4).
    - Two stacked comments on `create` contradict each other (`index.hh:180-182`).
    - faultfs counts bytes (`faultfs.hh:206-210`); a filesystem allocates whole blocks per
      file. "A disk of the budget's size" holds in faultfs; on a real disk the budget needs
      about one block of headroom per file.
    - `tools/vm/linux.sh` now passes discards to the host (`discard=unmap,
      detect-zeroes=unmap`), so gate-VM I/O figures before and after it are not
      interchangeable without a re-baseline.

### New blocking items

**L5. Reservations cover what merges and `install` write (findings 26, 27).**

- `merge`: after `merge_segments` builds the output in memory and before
  `write_unsynced` (`index.hh:334-337`), grow the reservation by the output's excess
  over `need`, draining the queue first, or throw `space_error` with nothing written. The
  comment at `index.hh:307-310` and the report's "which bound its output" go.
- `install`: `need` adds the checkpoint's metadata size and a bound on its manifest (its
  encoded size plus 160 bytes a segment). An ENOSPC from `install`'s commit leaves the
  target as it was, or D5 states that the target then holds the installed content
  uncommitted.
- Tests: a merge of finding 26's shape stays within its reservation or is refused by
  admission; `install` on a disk of the budget's size at every 1,000-byte step never meets
  ENOSPC after admission; the distant-docids merge test steps its room finely enough to
  fail without `48b2154`.

**L6. Retention never deletes a kept manifest it could not read (finding 28).**

- When a kept manifest cannot be read, for any reason, `retain()` removes nothing that
  round, and `manifest_files` caches only successful reads.
- Test: an IO that fails one open of a pinned manifest with EIO and with EMFILE, during
  an open and during a commit; the checkpoint stays on disk and installs.

### What approval needs

- **L1's residual.**
  - `decode_live_docs` rejects any delta after the first that is 0 or at least
    `doc_count − prev` (`files.hh:290-293`), so ordinals strictly ascend; `dead` counts set
    bits (`index.hh:1395-1396`).
  - Resealed tests: finding 24's three cases, refused by `open` and by `install` with the
    target unchanged. `index_test`'s live-count case keeps the manifest's totals in step;
    add the other-segment and live-length cases and an `install` of a manifest under
    another generation's name (finding 25).
  - `fuzz_files` asserts that every decoded deleted list strictly ascends.
- **L3.** `fuzz_files` runs one CPU-hour at the commit that fixes `decode_live_docs`. The
  segment and termdict hours count once their M1 logs are in `.scratch`; if the M1 stays
  unreachable, they run again on a reachable machine at the approval commit. Every log
  stays in `.scratch`, and the report takes runs, PCs, corpus size, and CPU time from it.
- **L5 and L6**, as above.
- **L2's listing**: `segment_test`'s lowered-group-last case also walks the postings
  (reaching `reader.hh:769`), or the report lists that sub-condition with a reason.
- **Condition 9's barrier test**: finding 29's invariants in a test (pins, releases, two
  predecessors, truncation steps below the files' sizes, every manifest on disk checked
  after each crash) that fails without the sync at `index.hh:1235`.
- **The report**: item 11's corrections, and the T1 injected-corruption, T5, T6 fuzzing,
  and C13 rows restated.
- The sanitizer runs and `tools/coverage.sh` at the approval commit.

### Conditions

None of these blocks Phase 3R's start once L1, L3, L5, and L6 close and Track C's C1–C4
pass.

- **2. `create` under power loss**, before Phase 3R creates replicas (finding 31):
  - `create` runs `clear_uncreated` whichever way it takes the lock, so leftovers go and a
    directory holding a manifest, a segment, or live docs is refused.
  - `uncreated` accepts only what an interrupted `create` leaves (`meta-1.kv`,
    `manifest-1.tmp`, and names not strata's). D3 states the residual: an index that never
    changed its metadata and lost every manifest and segment looks uncreated.
  - The test crashes each point under several seeds.
- **5–8**, as stated in the first review. Condition 6 also covers `install`'s dropped
  pins (finding 32).
- **9**: finding 22's second bullet, carried to the T3 gate.
- **10. Reclamation errors after the commit point**, before Phase 3R acknowledges
  commits: a failed truncate or unlink keeps the file queued and neither fails
  `commit()` nor fences a merge, or `commit()` raises a distinct error that says the
  commit holds. `IO::open_reclaim_candidate` (null when absent) is the journal's hook for
  that path.
- **11. Storage units**: D20 and T5 state that the storage budget counts bytes, and the
  headroom per file a real filesystem needs (finding 32).
- **12. The gate VM's discard setting** is recorded with the ratchet rows' baselines
  (finding 32).
