# Write-buffer bake-off (step 0 of `docs/design/write-buffer.md`)

Branch `perf/lanebuf` from `design/write-modes` (`acdc908`). Prototype: `bench/kv/proto/lanebuf.hh`,
`lanebuf.cc` (the three candidates from one source), `lanebuf_work.hh` (workloads shared with the
RocksDB adapter `lanebuf_rocksdb.cc`). Benchmark code only; nothing outside `bench/kv/proto` and the
`STRATA_KV_PROTO` block of `CMakeLists.txt` changes.

## Owner's answers (2026-10-21, folded in before the run)

1. Q1: a reader sees a write only once it is acknowledged at the index's durability level. The
   prototype has no log (durability `none`), so a write is acknowledged and visible when its group's
   stage 1 ends; V is stored then.
2. Q2: search reads at a freeze (coalesced); gets and scans read live. The prototype measures gets and
   scans only.
3. Q3 (stricter than the designer's): keep the multi-writer mode for Xapiand only if, on the
   Xapiand-shaped load (T11), it gives **at least 1.5x the fallback's requests per second or at most
   0.5x its request p99**, while costing **at most 3% at one writer** (T1) and **at most 5% more CPU
   per request**.

## Decision rules (the lead, 2026-10-21; review C1 and WB-8)

Set before the first timed run; the results apply them, they don't revise them.

1. **Noise.** Every comparison runs A/A first. A difference counts only when it exceeds that
   row's A/A spread; the 3% single-writer bar (Q3) is met only if C's single-writer cost, minus
   the spread, stays under 3%.
2. **C against A'.** Pick the candidate that wins more of T1 (writers 1/2/4/8), T2/T3 (read and
   scan p99 while writing, including C's main-run merges in the p99), T6 (bytes per record) and
   freeze cost, with no row worse than 1.10x the other. A tie on that count goes to C (less
   memory, simpler freeze).
3. **Xapiand's mode (Q3, on T11a, the journaled path).** Multi-writer only if it reaches at least
   1.5x the requests per second or at most 0.5x the request p99 against one writer with live
   readers, costs at most 3% with one writer and at most 5% more CPU per request; else the
   fallback.
4. **Both finalists miss.** If neither C nor A' beats today's single-writer buffer by more than
   the A/A spread on T1 at 4 writers, record that, ship the fallback for Xapiand, and keep the
   prototype for unjournaled embedders only after a design revision.
5. **What isn't decided here.** T7 (the +1 KiB target) needs the engine build; the prototype's
   segments must be real (not stubs) before T11a's per-segment lookup cost counts.

## Candidates, one source

| Name | Writers | Order for scans |
|---|---|---|
| C | write group (queue, leader runs stage 1), per-writer lanes | shared sorted runs (main + delta + private tail) |
| A' | same | concurrent skip list, tower inside the record, links by the writers, V after the last link |
| F (fallback, D14) | exclusive lease per transaction, one lane | shared sorted runs |

Common: lanes with 32-bit references, one hash version map (8-byte atomic slots, 7/8 full, no
erase, growth with epoch-stamped retirement), leader-filled 24-byte headers, tombstones and `prev`,
the visible position V, freeze, a minimal view with frozen buffers and stub segments.

## Hypotheses and keep/remove rules

H1 (C vs A'): C's single-writer insert is cheaper than A' (no tower links) and A' wins only on scans
beside writers. Rule from the design: a finalist that misses T1, T2, T4, or T7 is out; A' beats C
only if its T3 p99 is more than 20% better at 4 writers while C misses T3, and A' stays within 10%
of C at one writer.

H2 (multi-writer vs fallback): on request-shaped load over many small indexes the leader handoff
costs more than the lease serialization saves. Kept only if Q3's bar above is cleared.

Tuning experiments (at most three, each one hypothesis, run after the candidates): chosen from what
the data shows; recorded below with the numbers whether they win or lose.

## Procedure

Interleaved trials (at least five, spread stated), strict holds in pieces of at most 20 minutes
(`quiet run bakeoff-<n> 20 ...`), builds as light holds at 4 jobs. RocksDB 11.11.1 through the same
workload code (`lanebuf_rocksdb.cc`) with memtables large enough that nothing flushes, WAL off,
`allow_concurrent_memtable_write` at its default, and through the kv harness rows (fits1, fits4,
fits8) for context. Host: Intel i9-9980HK, 8 cores / 16 threads, macOS x86_64 only; the other
platforms are not measured (caveat).

## How the rules are applied (written before the first timed run; the lead's rules above stand)

These readings fix what the rules leave open, so the results cannot steer them.

1. **A/A spread.** Every row runs its candidates interleaved, five trials or more, with twin arms (an arm
   run under a second name: C and C2, F and F2) so each row carries its own A/A. The spread is (max - min)
   over the median of the twin's trials pooled with its partner's. A difference counts only when
   |median ratio - 1| exceeds that row's spread; inside it, the rows tie. The 3% bar is met only if C's
   one-writer cost over F, minus the spread, is under 3%.
2. **Rows counted for rule 2** (C against A'), each a win, a loss or a tie:
   - T1: throughput on fillrandom, 100 B values, at 1, 2, 4, 8 writers (4 rows); overwrite and the 1 KiB
     rows are reported and break no tie.
   - T2: get p99 beside 4 readers at 1, 2, 4, 8 writers at full write speed (4 rows); T3: the same for
     seek + 10 nexts p99 (4 rows). The 2 MB/s rows and p99.9 are reported beside them.
   - T6: resident bytes per new record at 1 and 8 writers (2 rows).
   - Freeze cost: freeze at 10^6 buffered records with 10% replaces (1 row).
   "No row worse than 1.10x the other" is read as: a candidate with a counted row worse than 1.10x the other's
   is disqualified when the other has none; when both have such rows, the count decides and the rows are
   reported. A tie on the count goes to C.
3. **Both finalists miss (rule 4)** compares T1 at 4 writers against today's single-writer buffer under a
   lock (`lanebuf_main`, informational in scope: its `apply` carries the engine's whole stage 1).
4. **Xapiand's mode (rule 3)** reads T11a (journaled path) with the winner of rule 2 against F: requests per
   second at least 1.5x or request p99 at most 0.5x, in the hot-index (Zipfian) rows and the uniform rows,
   with live readers; at most 3% at one writer (T1, one writer, and T11a at one request thread); at most 5%
   more CPU per request. A row inside its A/A spread counts as zero.

## What was built before the first timed run (C1 of the architect's re-check)

Rebased onto `main` at `88a56e7`. The prototype (`bench/kv/proto/lanebuf_core.hh`, `lanebuf.hh`,
`lanebuf.cc`, `lanebuf_work.hh`; adapters `lanebuf_rocksdb.cc`, `lanebuf_main.cc`; runner `bakeoff.py`) was
rebuilt rather than patched; every fix names where it lives.

| Item | Where |
|---|---|
| Hazard registry instead of the epoch domain (WB-8, C1) | `strata::reader_registry` (`include/strata/core/hazard.hh`, with SR-D18's fix): readers and pre-stages hold a `guard<view>`; the lease holder retires each replaced view with the objects replaced under it and frees the oldest prefix no record names (`reclaim`, D10) |
| Read at `max(V, view.position)` (WB-1) | `begin_read_at`: V first, the view after, `r = max(V, view.pos)`; views carry `pos`, set by freezes, flush installs and merges |
| Installs carry pending deletes (WB-2) | `do_merge_install`: the output's live bits carry the bits cleared on its sources since the build, and the active buffer's pending deletes naming a source are mapped through `plan.omap` (a copy not carried is fenced); a flush install kills keys a newer buffer holds (shadowing), as a frozen buffer's pending delete |
| Freeze against writers mid-insert, lane ownership (WB-3, WB-10) | a lane belongs to one buffer and one pre-stage (`busy` from claim to verdict); the leader copies a sealed buffer's records into a lane of the active buffer it owns (`restage`) |
| Per-buffer position index (WB-4) | `pos_index`: append-only, geometric blocks never replaced, count published with release at group end; rejected transactions rewind it; run builds, tails and the flush read only it |
| Builds only when a scan asks (WB-17) | a scan whose private tail passes 256 sets `want`; the lease holder posts the build to a pool thread and installs the result as a chore (`maintain`); a scan on an idle index links a writerless group (`poke`); T1/T11 never scan and pay nothing |
| Component epoch (WB-18) | `view.cepoch` moves only at freeze, flush install, merge, add; a table or run-pair replacement bumps only `view.gen`; pre-resolution is valid while `cepoch` matches (`preres_used` / `preres_redone` count it) |
| Fixed chunk directory, push-only lanes (WB-19) | `chunk_dir`: 8 inline entries and 12 blocks of `8 << k` entries installed by compare-and-swap, 32,768 chunks, never replaced; lanes on a lock-free push-only list the readers never touch |
| "never" / "superseded" fields (WB-9) | gone, as revision 1 states: a rejected transaction restores its slots and rewinds the position index; the record's position stays an atomic 32-bit field |
| Real segments (WB-8) | `seg_data` holds strata's `keys::sorted_encoder` / `keys::reader` (range check, fuse filter, termdict) with documents by ordinal; no tombstones in segments, so a re-add after a delete restarts at version 1 whenever it lands (the engine's `evaluate`) |
| T11a / T11b | `app_engine`: proposers pre-stage (C, A') or only submit (F); appliers apply in log order after a modeled turn; `lb_engine`: the write group (C, A') or the lease (F) |

Same-key records of one transaction order by an operation index kept in the record's flags, so a
transaction that writes a key twice reads back its last write in every order (the first model-check
failure of the rebuilt runs path).

## Results (2026-10-21, macOS x86_64 i9-9980HK only; raw lines in `docs/perf/write-buffer-bakeoff-data/`)

Host caveat: the host was not idle. Two QEMU VMs and git ran outside `quiet` (load average 6 to 7 of 16) during
every strict hold. Arms were interleaved and carry A/A twins, so ratios are fair; absolute numbers are not clean.
The binaries are the Release (`-O3`) build of `bench/kv/proto/lanebuf.cc`; T1 and T2/T3 ran before the
driver-only edits (a JSON-length fix, a `bytes` mode, the T11 wait loop), the index code was unchanged.

### Functional gates (C1)

| Check | Result |
|---|---|
| Model check, native, 4 + 4 and 8 + 8 threads, freezes, flushes, merges, held views | C, A', F: 0 mismatches in 3 runs each (60,000 transactions) |
| Model check under TSan (3 + 3, 4 + 2) and ASan/UBSan | C, A', F: 0 reports |
| Cases WB-1, WB-2, WB-3 | pass on C, A', F (WB-3 n/a on F) |
| T10 litmus, native 10^6 writes / 4 x 10^6 reads, and TSan | 0 torn on all three |
| Mutants killed | 1 slot before header: native litmus (40 torn) and TSan; 2 relaxed slot: TSan only (x86 TSO, as predicted); 3 no undo: hang; 4 V early: model (C); 5 stale pre-resolution: model (C, A'); 7 freed at once and 13 freed ignoring the registry: crash and ASan; 8 read at V: WB-1 case (all) and model (C); 9 install drops pending deletes: WB-2 case (all; the random check kills it only by chance); 10 no leader copy: crash and ASan |
| Not built | the design's "map entry erased in place" (no erase exists) and "verdict before V" (WB-6, no log) mutants |

### T1, throughput, journal-less, 100 B fillrandom (ops/s, median of 5, A/A in the last column)

| Writers | C | F | A' | RocksDB (WAL off) | main buffer under a lock | C/F | C/A' | C/RocksDB | C/main | A/A spread C~C2 (F~F2) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2.53M | 2.61M | 0.35M | 0.48M | 1.31M | 0.97 | 7.3 | 5.3 | 1.9 | 11% (10%) |
| 2 | 2.54M | 2.04M | 0.38M | 0.47M | 0.71M | 1.25 | 6.6 | 5.4 | 3.6 | 15% (16%) |
| 4 | 3.02M | 1.64M | 0.60M | 0.73M | 0.39M | 1.84 | 5.0 | 4.2 | 7.8 | 8% (6%) |
| 8 | 3.55M | 1.31M | 0.91M | 1.07M | 0.44M | 2.72 | 3.9 | 3.3 | 8.1 | 6% (8%) |

Overwrite and 1 KiB rows (`t1.jsonl`) agree in direction. C over F at one writer: 0.97 (fillrandom 100 B), 0.94
(1 KiB), 0.89 (overwrite 100 B), 0.91 (overwrite 1 KiB). The pre-registered spread is a pooled range of ten
trials (4 to 27%), so "cost minus spread under 3%" passes on every row; the twins' medians agree to 0.2 to 1.4%
(F at two writers 4%), so the 6 to 11% one-writer cost on three rows is real at the median. `main` is today's
engine `apply` under one lock: informational in scope (it carries the whole stage 1).

### T2 and T3, readers beside writers (p99 ns, 4 readers, 2 s runs, 100 B, 200,000 loaded keys)

| Writers | get C | get A' | get RocksDB | scan C | scan A' | scan RocksDB | scan C/A' |
|---|---|---|---|---|---|---|---|
| 1 | 588 | 588 | 3,600 | 23,724,032 | 12,224 | 16,192 | 1,941 |
| 2 | 596 | 572 | 3,760 | 16,580,608 | 11,968 | 14,784 | 1,385 |
| 4 | 612 | 580 | 3,984 | 11,337,728 | 14,656 | 20,608 | 774 |
| 8 | 660 | 652 | 4,640 | 5,603,328 | 18,560 | 30,848 | 302 |
| 1 at 2 MB/s | 532 | 540 | 2,608 | 43,264 | 6,368 | 8,256 | 6.8 |
| 4 at 2 MB/s | 540 | 540 | 2,704 | 42,752 | 6,176 | 8,256 | 6.9 |

Gets: C and A' tie (within 0 to 9% spread, A' ahead only at 4 writers by 6%), both about 6 times better than
RocksDB. Scans: C's private tail reaches 10^5 to 10^6 entries when writers run flat out (`tail_max` 959,675 in a
smoke run) because one builder cannot keep up (about 520 ns per entry built, the main-run merge copying 4 bytes
per buffered record per 4,096 entries); each scan sorts it. Even at 2 MB/s C's scan p99 is 6.8 times A''s and its
scan throughput 3 times lower. C misses T3 by three orders of magnitude at full speed; A' meets it (0.6 to 0.8x RocksDB).

### T6 and freeze, T7

| Row | C | A' | F | RocksDB | Notes |
|---|---|---|---|---|---|
| RSS per record, bufmem method, 1 writer | 185.1 | 194.2 | 185.2 | 146.5 (T1 method) | C/A' 0.95, spread 4.8%: tie |
| RSS per record, 8 writers | 188.8 | 199.6 | 191.6 | 148 (T1 method) | C/A' 0.95, spread 11.8%: tie |
| Accounted per record, 1 / 8 writers | 167.8 / 178.1 | 174.7 / 188.6 | 167.8 / 167.8 | | deterministic: C 4 to 6% below A'; target 153 B missed by 10 to 16%: lane 147, slot table 14, position index 7 |
| Freeze, 10^6 buffered, 10% replaces (pending deletes 95,309) | 6.8 ms | 8.5 ms | 6.8 ms | | C wins (trial range 1%) |
| Freeze, 10^6 / 10^3 buffered, no replaces | 4.9 us / 2.5 us | 7.1 us / 3.4 us | 5.7 us / 1.1 us | | O(1) in records (target within 2x) met; the replace term is linear (71 ns per pending delete) |
| T7 heap bytes per index, idle / after one write | 1,396 / 5,792 | same | same | | not decided here (rule 5) |

### Not run, or not usable

- **T11a / T11b**: not decided. A first run produced no strata data (a 1 KiB JSON-line cut in the driver, fixed); a second, aborted at three
  trials per arm, showed zipfian rows swinging from 400k to 100 requests/s per trial: spinning request threads and
  appliers convoyed on a loaded host (fixed by yielding waits; a 3-run check afterwards gave 717k to 753k stable). The corrected
  harness (`lb_plain4`, `bakeoff.py req`, RocksDB prebuilt clones) was not measured for lack of quota. The aborted partial
  uniform rows (`req-aborted.jsonl`, 3 to 4 trials, loaded host) put C/F requests per second at 0.80 to 1.26 and p99 at 0.82 to 1.21: no row near 1.5x or 0.5x.
- **Tuning experiments**: none run. E1 (prefixed entries in a binary-counter run set) and E2 (a builder that stays on
  the buffer) are in the source behind `LANEBUF_E` and are unmeasured; they target C's T3.
- T4 (beside `kv_proto_buffer`), T5, T7's gate form, the other platforms, WB-6 mixed acknowledgement levels, `install()`/`heal()`.

## Verdicts under the rules

1. **C or A' (rule 2, as read before timing)**: C wins T1 (4 rows) and freeze (1) = 5; A' wins T2 (1 row, 4 writers) and T3 (4) = 5; T6 ties.
   Both have rows worse than 1.10x the other (A' on T1, C on T3), so the count decides and a tie goes to **C**.
   The design's own rule 1 gives the same answer (A' misses T1 against RocksDB at every writer count, 0.72 to 0.86x; C misses only T3).
   C as built does not pass T3: it needs a fix or A' for scan-heavy indexes.
2. **Both finalists miss (rule 4)**: not triggered; C beats today's buffer by 1.9 to 8.1x.
3. **Xapiand's mode (rule 3)**: **undecided, T11a missing.** What exists points to the fallback: C is 3 to 11% slower than F at one writer
   (T1), and no partial T11a row reached 1.5x or 0.5x p99. C's T1 win at 2 to 8 writers (1.25 to 2.7x F) is journal-less and does not decide the applier path.
