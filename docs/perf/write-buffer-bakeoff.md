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
- **Tuning experiments**: run afterwards, see "Scan tail" below (seven experiments on C's T3).
- T4 (beside `kv_proto_buffer`), T5, T7's gate form, the other platforms, WB-6 mixed acknowledgement levels, `install()`/`heal()`.

## Owner decision (2026-10-26)

C is the write buffer. Its scan p99.9 at full write speed, 1.5 to 1.8x RocksDB's after the scan-tail
changes ("Scan tail"), is accepted; at 2 MB/s C beats RocksDB and A'. Closing the remaining gap (a
sampled index per big level, or the visibility position in the entry) is a buffer-internal
optimization, queued after the buffer is built. Xapiand's mode (rule 3) is still decided by T11.

## Verdicts under the rules

1. **C or A' (rule 2, as read before timing)**: C wins T1 (4 rows) and freeze (1) = 5; A' wins T2 (1 row, 4 writers) and T3 (4) = 5; T6 ties.
   Both have rows worse than 1.10x the other (A' on T1, C on T3), so the count decides and a tie goes to **C**.
   The design's own rule 1 gives the same answer (A' misses T1 against RocksDB at every writer count, 0.72 to 0.86x; C misses only T3).
   C as built did not pass T3. After the scan-tail work below it still misses at full write speed (p99.9 1.5 to 1.8x RocksDB's, not 1.0x) and
   meets T3 at 2 MB/s (0.4x); A' meets T3 at every rate and loses T1 by 4 to 7x.
2. **Both finalists miss (rule 4)**: not triggered; C beats today's buffer by 1.9 to 8.1x.
3. **Xapiand's mode (rule 3)**: **undecided, T11a missing.** What exists points to the fallback: C is 3 to 11% slower than F at one writer
   (T1), and no partial T11a row reached 1.5x or 0.5x p99. C's T1 win at 2 to 8 writers (1.25 to 2.7x F) is journal-less and does not decide the applier path.

## Scan tail (candidate C's T3, 2026-10-25, macOS x86_64 i9-9980HK; plan in `lanebuf-scan-tail.md`)

Scan = seek + 10 nexts, 4 readers beside W writers at full speed, 200,000 loaded keys, 100 B values, 2 s runs (the
buffer grows to 4 to 5 million records, one buffer, as in T3 above). p99 in ns unless stated; trial ranges are the
spread of p99 over the trials. Raw lines: `write-buffer-bakeoff-data/scan-tail-*.jsonl`. Every variant passes `check`
(model, scans compared with the model) and `cases`. RocksDB's p99.9 is the T3 target (1.0x).

**Profile of the baseline (4 writers).** Readers spend about 70% of their time in `runs_at`, sorting the private tail
(`tail_avg` 900 to 4,700 entries, `tail_max` over 10^6); the single pool builder is busy 90% of the time (sort of the backlog
36%, merges into the main run 58%) at about 380 ns per entry, below the 2 to 3.5M writes/s the writers deliver. C's writers
run 2 to 4x faster than A''s or RocksDB's (2.1M against 0.46M and 0.6M per second at 4 writers), so the same buffer is
4 times larger when the scans run.

### Experiment 1 (E1): prefixed entries in a binary counter. WINS, not enough

Build sorts only the backlog; runs are 12-byte entries (8-byte key prefix, reference) merged when a level is not more than twice
the next. Six interleaved trials with an A/A twin (C2).

| Writers | p50 C / E1 | p99 C | p99 C2 | p99 E1 | p99.9 C | p99.9 E1 | scans/s C / E1 | p99 A' | p99 RocksDB | p99.9 RocksDB |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2.2k / 21.1k | 9.03M | 8.14M | 311k | 523M | 6.7M | 1.6k / 66.6k | 11.8k | 15.7k | 26.5k |
| 4 | 2.5k / 21.8k | 4.38M | 3.28M | 623k | 630M | 13.8M | 1.7k / 42.9k | 13.9k | 19.7k | 30.3k |
| 8 | 2.7k / 21.1k | 0.90M | 1.07M | 1.10M | 822M | 29.9M | 1.5k / 24.8k | 17.5k | 30.6k | 39.7k |
| 1 at 2 MB/s | 6.8k / 6.5k | 42.5k | 42.8k | 16.6k | 72.7k | 24.7k | 386k / 547k | 5.9k | 8.1k | 17.9k |
| 4 at 2 MB/s | 6.9k / 6.4k | 42.5k | 42.0k | 16.8k | 71.7k | 30.5k | 381k / 547k | 5.6k | 8.4k | 19.1k |

(C's p50 of 2 us is the readers being starved: 1,500 scans per second.) E1 turns the full-speed scan rate up 16 to 41 times
and the p99 down 7 to 30 times (1 and 4 writers; 8 writers is inside C's own spread, 90 to 360%), and C's writers speed up 1.9 to 2.0 times (the readers no longer hold the machine in sorts). It still misses T3 by 20 to 36 times on p99 and 250 to 750 times on p99.9 at full speed, and by 1.4 to 2.1 times at 2 MB/s. **Kept as the base of the later experiments.** Memory: runs go from 1.9 to 10 to 11 bytes
per record (see the cost table).

### Experiment 2 (E2): the builder stays on the buffer. TIE, removed

The source had a use-after-free (the job reloaded the installed run set while the lease holder could retire it; the
`length_error` abort); fixed by building from the job's own last output. Against E1 the six-trial medians agree within
the spread (p99 at 4 writers 544k against 623k, spread 17 to 49%; p99.9 14.5M against 13.8M; 1 and 8 writers the same). The
builder is already saturated, so waking it sooner buys nothing. Removed (the eager loop and its flag).

### Experiment 3 (E3): select the smallest tail entries instead of sorting the tail. TIE/LOSS on E1 (removed, then retried on the split jobs below)

`nth_element` for the first 32 entries, the rest sorted when a scan runs past them. Five trials, E1 as the base.

| Writers | p50 E1 / +SEL | p99 E1 / +SEL | p99.9 E1 / +SEL | scans/s E1 / +SEL |
|---|---|---|---|---|
| 1 | 21.4k / 20.1k | 338k / 537k | 7.1M / 6.0M | 64.4k / 73.9k |
| 4 | 22.1k / 20.9k | 635k / 930k | 14.9M / 10.0M | 42.1k / 52.2k |
| 8 | 21.1k / 20.4k | 1.00M / 1.73M | 30.8M / 17.7M | 26.0k / 30.9k |

Inside the A/A spread (C2 vs C: 12 to 19% on p99) on p50, worse on p99, better on p99.9 only at the largest tails. The tail pass
(reading each entry's record) and the pool lag, not the comparison sort, set the common case. Removed at this point.

### Experiment 4 (E4): the position index holds the prefixed entries. WINS, small

The leader writes `(prefix, reference)` into the position index; the tail pass finds the visible end by binary search (positions
never decrease along the index) and reads entries in sequence, dereferencing a record only on a prefix tie; the builder
copies entries instead of reading records.

| Writers | p50 E1 / +PFX | p99 E1 / +PFX | p99.9 E1 / +PFX | scans/s E1 / +PFX |
|---|---|---|---|---|
| 1 | 21.4k / 18.0k | 338k / 226k | 7.1M / 4.7M | 64.4k / 90.1k |
| 4 | 22.1k / 18.0k | 635k / 350k | 14.9M / 7.2M | 42.1k / 68.7k |
| 8 | 21.1k / 17.8k | 1.00M / 553k | 30.8M / 15.4M | 26.0k / 46.3k |

p99 falls 33 to 45% and p99.9 about half, beyond the A/A spread (14 to 44%). With +SEL as well, p99.9 is a third of E1's at 4 and 8
writers (4.4M, 6.3M) but p99 is not better (627k, 995k). Kept: +PFX. It costs 8 bytes per record in the position index (see the cost table).

### Experiment 5 (E5): publishing a fresh run and compacting big levels become separate jobs. WINS, large

Profile that motivates it (E1 with E4, 3 s, 4 writers, 4 readers): of 7,335 builds, 52 spent more than 1 ms in the cascade of merges
(1,877 of the 2,712 ms the builder spent in total, the longest 571 ms), and while the single builder is inside one the backlog is
never published: the tail-size histogram shows about 5% of scans seeing a tail past 1,000 entries and a long run to a few 10^5.

Change: a fresh job sorts the backlog and merges small levels (up to 16,384 entries); a compactor job merges the big ones, on a
second pool thread. Each works on a copy of the levels it was posted with and returns a splice (the consecutive levels it consumed,
the level it produced, the covered position); the lease holder applies it to whatever set is installed by then, so the compactor
never blocks a publication. The tail threshold falls to 64 (the knob is cheap here: the fresh job no longer queues behind a
cascade). Two quick rounds on the first version at threshold 256 and 64 put the p50 at 22 and 18 us; 64 kept.

| Writers | p50 before / after | p99 before / after | p99.9 before / after | scans/s before / after | RocksDB p99 / p99.9 |
|---|---|---|---|---|---|
| 1 | 2.2k / 17.3k | 4.23M / 41.7k | 518M / 59.1k | 2.1k / 218k | 15.7k / 25.7k |
| 2 | 2.1k / 17.3k | 4.75M / 44.3k | 459M / 61.2k | 2.0k / 216k | 14.4k / 26.5k |
| 4 | 2.4k / 17.8k | 4.42M / 53.0k | 810M / 91.6k | 1.5k / 193k | 20.4k / 27.8k |
| 8 | 2.9k / 20.1k | 725k / 72.2k | 717M / 106.0k | 1.6k / 173k | 30.6k / 37.6k |

("before" is the source at the start of this section, E0; "after" is E1 + E4 + E5; five trials, A/A twin: median ratios C/C2 0.87 to
1.18 on p99 and 0.90 to 1.05 on p99.9, trial ranges 12 to 24%.) The p99.9 falls by about four orders of magnitude (6,800 to 8,800x), the p99 by 80 to 110x (1 to 4 writers) and 10x (8 writers). The remaining miss against RocksDB is 2.3 to 3.3x on p99.9. Kept. TSan and ASan clean with the cap at 32
entries and the threshold at 16, where splices and compactions run hundreds of times per check.

### Experiments 3 again (selection) and 6 (prefix merge), on the split jobs. Both WIN, small

E3 returns because the tail is now short enough (median 100 to 250 entries) for the selection to be worth its branch; E6 changes the
scan's merge: each source carries the first 8 key bytes (one load, byte-swapped), one pass finds the minimum key and the sources
holding it, only those step, and the key is compared by prefix before `memcmp`.

| Writers | p50 split / +SEL / +MRG / both | p99 split / +SEL / +MRG / both | p99.9 split / +SEL / +MRG / both |
|---|---|---|---|
| 1 | 17.3k / 17.0k / 13.5k / 13.5k | 41.7k / 40.7k / 37.6k / 38.7k | 59.1k / 51.5k / 56.1k / 47.4k |
| 2 | 17.3k / 16.5k / 13.6k / 13.6k | 44.3k / 44.3k / 36.6k / 34.6k | 61.2k / 55.6k / 55.6k / 46.3k |
| 4 | 17.8k / 17.5k / 14.4k / 14.7k | 53.0k / 52.0k / 43.8k / 41.7k | 91.6k / 70.1k / 84.5k / 57.1k |
| 8 | 20.1k / 19.8k / 15.9k / 16.2k | 72.2k / 56.1k / 53.5k / 50.9k | 106.0k / 74.2k / 93.7k / 69.1k |

+MRG takes 19 to 22% off the p50 and 10 to 26% off the p99 and raises scans per second 19 to 22% (at 8 writers the p99 gain is near the 24% trial range); +SEL moves mostly the p99.9 (9 to 32% down in all four rows, with or without +MRG). Both kept; the pair takes
p99.9 to 47, 46, 57 and 69 us, 1.8 to 2.1x RocksDB's.

### Experiment 7 (E7): counter ratio. WINS, small

A level merges into the one before it while that is at most R times its size (R = 2 so far): fewer levels, so fewer sources per scan,
for about the same merge volume (estimated write amplification 1.5 log2 N at R = 2 against 1.25 log2 N at R = 4). Five trials, A/A twin.

| Writers | p50 R=2 / 4 / 8 | p99 R=2 / 4 / 8 | p99.9 R=2 / 4 / 8 | scans/s R=2 / 4 / 8 |
|---|---|---|---|---|
| 1 | 13.6k / 12.6k / 12.2k | 35.6k / 33.0k / 32.4k | 45.8k / 42.2k / 42.8k | 263k / 282k / 292k |
| 2 | 13.8k / 13.1k / 12.5k | 35.6k / 33.0k / 30.6k | 50.9k / 42.8k / 39.7k | 261k / 280k / 290k |
| 4 | 14.5k / 14.0k / 13.4k | 42.8k / 38.7k / 36.6k | 60.2k / 52.5k / 51.5k | 232k / 251k / 261k |
| 8 | 16.1k / 14.8k / 14.3k | 47.9k / 46.3k / 41.2k | 65.3k / 65.3k / 63.2k | 211k / 225k / 236k |

R = 8 is 3 to 22% better on p99.9, 9 to 14% on p99 and 11 to 12% on scans per second, in all four rows, against trial ranges of 1 to 30%; kept as the default (`LANEBUF_RATIO`). Write throughput beside the scans moves 1 to 3% down.

### Where C stands (final: E1 + E4 + E5 + E3 + E6 + E7), five trials, readers 4

| Writers | p50 | p99 | p99.9 | scans/s | writes/s | p99 / RocksDB | p99.9 / RocksDB | A' p99.9 / RocksDB |
|---|---|---|---|---|---|---|---|---|
| 1 | 12.2k | 32.4k | 42.8k | 292k | 1.36M | 2.05x | 1.60x | 0.61x |
| 2 | 12.5k | 30.6k | 39.7k | 290k | 1.41M | 2.11x | 1.47x | 0.56x |
| 4 | 13.4k | 36.6k | 51.5k | 261k | 1.58M | 1.82x | 1.80x | 0.78x |
| 8 | 14.3k | 41.2k | 63.2k | 236k | 1.70M | 1.34x | 1.66x | 0.71x |
| 1 at 2 MB/s | 3.1k | 5.2k | 7.2k | 1.22M | 17k | 0.64x | 0.39x | 0.42x |
| 4 at 2 MB/s | 3.1k | 5.1k | 6.9k | 1.23M | 17k | 0.64x | 0.33x | 0.37x |

(The 2 MB/s rows ran at ratio 2 in the guard hold; RocksDB p99 8.2k and 8.0k, p99.9 18.3k and 21.1k.) **T3 is met at 2 MB/s
(C is faster than RocksDB and A' there), and missed by 1.5 to 1.8x on p99.9 (1.3 to 2.1x on p99) at full write speed**, where C's writers also run 2.1 to 3.5x faster than RocksDB's and 2.6 to 4.5x faster than A''s (so each scan sees a buffer that grew that much faster). Against the start of this section the full-speed miss fell from 24 to 330x on p99 and 17,000 to 29,000x on p99.9 to 1.3 to 2.1x and 1.5 to 1.8x.

### Cost on the other axes (guard hold, 5 trials, A/A twins; before = the source at 737948f, after = final but ratio 2)

| Axis | Row | Before | After | After / before | A/A spread |
|---|---|---|---|---|---|
| Throughput | T1 fillrandom 100 B, 1 writer | 2.59M/s | 2.53M/s | 0.976 | 6% |
| | 2 writers | 2.70M/s | 2.63M/s | 0.974 | 5% |
| | 4 writers | 3.05M/s | 3.02M/s | 0.991 | 4% |
| | 8 writers | 3.65M/s | 3.53M/s | 0.968 | 1% |
| Time | T1 CPU per op, 1 writer | 385 ns | 395 ns | 1.03 | 6 to 8% |
| Time | T2 get p99 beside 1 / 4 / 8 writers | 572 / 596 / 660 ns | 572 / 596 / 644 ns | 1.00 / 1.00 / 0.98 | 3 to 8% (RocksDB 3.6 / 3.8 / 4.5 us) |
| Memory | T6 accounted bytes per record, 1 writer | 167.8 | 181.7 | 1.083 | 0 (deterministic) |
| Memory | T6 resident bytes per record, 1 writer | 185.1 | 193.5 | 1.045 | 3 to 5% |
| Memory | T7 heap bytes per index, idle / after one write | 1,415 / 5,810 | 1,431 / 6,338 | +16 / +528 | 0 |
| Time | freeze at 10^6 buffered, 10% replaces | 6.86 ms | 6.24 ms | 0.91 | 4 to 6% |
| Time | flush sort at the same size | 1,366 ms | 1,348 ms | 0.99 | |
| Disk | not measured: the flush reads the same records in the same order (flush sort time unchanged); the position index and the runs are memory only | | | | |

T1 loses 1 to 3% (inside the A/A spread in every row but consistent in sign: the leader writes 12 instead of 4 bytes per record into the
position index) and T2 is unchanged; the scan-tail fix charges nothing to a workload that never scans, because runs are built only when a
scan asks. Memory pays: +8.3% accounted per record (the position index at 12 bytes in geometric blocks that overallocate up to 2x) and +528
bytes per index after one write (the first block holds 64 entries). The design's 153 B per record target was already missed at 167.8; it is now
missed by 19%. The runs of an index that is being scanned add memory too: 10 to 11 bytes per record with E1 alone (against 1.9 before), not re-measured on the final.

### Gates

`check` (model, scans compared with the model) and `cases` pass on C, A' and F natively (4 + 4 and 8 + 8 threads), on the final C also with ratio 8;
TSan and ASan/UBSan clean on the final C at the default constants and with the cap at 32 entries and the threshold at 16 (hundreds of splices and
compactions per check); the T10 litmus passes natively (10^6 writes, 3.9M reads) and under TSan. Raw lines: `write-buffer-bakeoff-data/scan-tail-*.jsonl`
and `scan-tail-5-guard.log`.

### Savings, and what is left

This is a latency and throughput fix to a prototype, not a CPU saving: a scan-heavy index that is also being written flat out answers
12 to 14 us at the median, 31 to 41 us at p99 and 40 to 63 us at p99.9 where it answered 4 ms and 500 ms (one writer), and completes 260 to 290 thousand scans
per second per four readers where it completed 1.5 to 2 thousand; at write rates people run (2 MB/s) it halves the scan p50 (7.0 to 3.1 us) and
beats RocksDB. Builds cost pool CPU only while scans ask. No dollar figure is claimed.

Left (Next): the full-speed gap of 1.5 to 1.8x on p99.9 is source-count and cache-miss bound (about 11 sources per scan; the phase timers put 45% of the mean scan in building the sources, tail included, and 53% in the merge): a sampled index over each big level, or storing the visibility position in the entry
(16 bytes, no record read per source), are the candidates, each costing memory. Memory: the position index could hold a 4-byte prefix (+4 instead
of +8 bytes per record) at the price of more tie reads. A' remains the T3 winner at full speed (0.56 to 0.78x RocksDB) and loses T1 by 4 to 7x.
