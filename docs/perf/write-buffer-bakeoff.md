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

## Results

(timed pieces follow the design revision)
