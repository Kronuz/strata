# The cold tier's gate rows through the catalog

`bench/catalog_tiers` measures `docs/design/cold-tier.md`'s T1 to T4 and T2d as a host runs the
catalog: a `catalog<kv_index>` over `PosixIO` directories that `idle_index build` makes, a
`work_pool_executor` (eight chores at once), a timer thread, one shared `reader_registry`, and the
process's real `resource_budget`, which raises `RLIMIT_NOFILE` to its hard limit and caps from it.
`bench/catalog_tiers/tiers.sh` builds this tree's harness beside `main`'s `idle_index`, the
re-measured baseline (A3), and runs each row with the baseline interleaved in the same job.

## How each row runs

- **Tiers.** Hot is every index opened by `try_pin` (eager segments, so every segment mapped);
  warm is `begin_demote()` on each slot (`open` with nothing mapped, or `owed` under `defer_io`);
  cold is demoted then `begin_close()`d, so no slot holds it. The sweep is not started, so nothing
  moves a slot between forcing and measuring.
- **T1** meters the process as `idle_index` does (resident, footprint, descriptors, mappings)
  after a 5 s idle, and the counting build (`catalog_tiers_count`, `bench/idle_index/heap_count.hh`)
  gives the live heap per index.
- **T2 and T2d** take one request pin and one `get` per index in a random order from one thread off
  the pool. A warm `defer_io` index's pin starts the wake, `promote()` runs on the pool, and the pin
  resumes on the requesting thread; a cold one's pin posts the open to the same pool. Under
  `defer_io` the run then times `promote()` alone on 10,000 indexes opened without the catalog,
  per segment, against D3's 33 to 50 µs estimate.
- **T3** starts the sweep with the row's caps, gets every index once on 8 threads, then 1M Zipf
  requests at θ = 0.99. A sampler reads the kernel every second: mappings and descriptors, and on
  macOS this process's vnodes (the distinct files behind its descriptors and mapped regions;
  `kern.num_vnodes` is the system's vnode cache, which sits at `kern.maxvnodes`, 263,168 here, on a
  busy host, so it is reported, not compared). The harness prints PASS on zero `ENFILE`, `EMFILE`,
  `ENOMEM` and `too_many_open`, every peak below the budget's hard caps, and open and hot counts
  within one pass (`pass_posts`, 16) of the soft caps 3 s after the requests end. The design's
  target is every sample after the touch ends; the verdict below reads the samples against it.
- **T4** replays `test/s3fifo_model.hh`'s trace (mt19937_64 seeded 42; the scan variant inserts one
  access to every key after the warm-up) on 8 threads from one cursor, each request a pin by key
  and a re-pin by handle. The hit ratio over the measured part is one less the misses (opens of an
  absent key, reopens of a closing one, retries) over the requests; the models (`s8_variants` and
  `model_total`) replay the same trace at the catalog's `soft_open`. The row passes at the
  `s8_variants` ratio less one point with no kernel error.

## Commands

Each row is one strict job of at most 10 minutes holding its interleaved pairs, whose spread is
the interval (D59); the counts are scaled to fit, and the caps scale with them for T3 and T4.

```sh
tiers.sh build OUT MAIN_SRC 0                # light: binaries only (0 keeps the fixtures)
tiers.sh t1 OUT d1s1 1000 2                  # T1, with d1000s1; d100s10 and d1000s10 a second job
tiers.sh t2 OUT d1s1 500 3                   # T2/T2d, one job per fixture (d1000s1, d1000s10)
tiers.sh t3 OUT 50000 100000 2000 500        # T3: 25x the open cap, 100x the hot cap
tiers.sh t4 OUT 0.99 noscan 20000 40000 20000 1000 250   # T4: two cells per job
```

At full scale (milestone checks, run once on main): `tiers.sh build OUT MAIN_SRC 100000
1000000`, `t1 OUT FIXTURE 100000 3`, `t2 OUT FIXTURE 100000 1 [drop]`, `t3 OUT 1000000`, `t4 OUT
THETA [scan]` with the platform's caps.

A one-document index has one segment, so the 10-segment fixtures carry 100 and 1,000 documents.

## Findings while building it

- **Every open syncs twice.** `basic_index::open` syncs the directory and then the manifest log
  (`index.hh`, the rollover's D6.6 rule), on `main` too. On the development Mac under other jobs'
  load, a sample of `idle_index open` spent nearly all its time in those two `fcntl` calls, and
  opens took 25 to 60 ms each. T2's cold row, T3's touch and T4's misses pay it per open.

## Results

macOS x86_64 (the development Mac), cache warm, strict jobs at strata:9621595f with main
strata:15d39c30's `idle_index` interleaved. Each range is the spread of the job's pairs (D59).

**T1, memory per tier** (261111-035757-efe281 one segment, 261111-035757-083bc8 ten; 1,000
indexes, 2 pairs). Heap per index from the counting build is exact; RSS per index at 1,000
indexes carries about 5 KB of process baseline, so it is not comparable with the 5.9 KB target
set at 100,000.

| Fixture | main heap | hot | warm | cold | hot RSS vs main | maps per index hot / warm / cold |
|---|---|---|---|---|---|---|
| d1s1 | 6,484 B | 7,134 B | 5,084 B | 292 B | 12,669-12,685 vs 11,612-11,616 B (+9%) | 1.03 / 0.03 / 0.03 |
| d1000s1 | 6,849 B | 7,499 B | 5,204 B | 292 B | 21,271-21,291 vs 20,124-20,161 B (+6%) | 1.03 / 0.03 / 0.03 |
| d100s10 | 27,798 B | 30,248 B | 11,396 B | 292 B | 73,613-73,683 vs 70,611-70,840 B (+4%) | 10.04 / 0.03 / 0.03 |
| d1000s10 | 28,058 B | 30,508 B | 11,476 B | 292 B | 114,745-114,749 vs 111,800-112,157 B (+3%) | 10.03 / 0.03 / 0.03 |

- Hot through the catalog costs 650 B of heap per index over main's open with one segment and
  2,450 B with ten (5 and 23 allocations more). `idle_index`'s own hot, without the catalog,
  matches main exactly (K6 below).
- Cold keeps 292 B and 1.08 allocations per index: the catalog's entry for the key. T1's "cold
  ≤ 16 B" is not met at step 2.
- defer_io changes no row. Warm holds one descriptor per index and no map.
- The 10-segment fixtures were built with 10 segments by this tree's `idle_index` (main's
  ignores SEGMENTS).

**K6, T1's step-1 rows** (261111-035757-a253be; `idle_index` alone, 1,000 indexes, 2 pairs; its
binary is the one built at strata:dbfc1898, whose diff to strata:9621595f is `reader.hh`'s stored
frame decode, which an open does not reach): hot's heap equals main's on every fixture (6,484,
6,518 and 27,798 B on d1s1, d100s1, d100s10), and its RSS is within -0.0% to +3.3% of main's.
Warm: 4,798, 4,806 and 11,110 B of heap, 0.017 to 0.026 maps per index. Lazy: 4,826, 4,834 and
11,138 B, 0.005 to 0.022 maps.

**T2 and T2d, first query per tier** (500 indexes, 3 pairs; p50 and p99 in µs, the range over the
pairs):

| Fixture | main open | main query | hot | warm | warm defer (T2d) | cold | cold defer |
|---|---|---|---|---|---|---|---|
| d1s1 | 930-20,882 / 44,036-57,075 | 0.9-1.5 / 1.5-2.5 | 1.3-2.1 / 1.9-2.9 | 33-47 / 74-96 | 47-85 / 108-180 | 212-21,244 / 42,060-59,913 | 264-20,946 / 42,224-59,607 |
| d1000s1 | 1,265-41,108 / 42,142-58,753 | 5.1-9.0 / 6.1-18 | 5.9-9.3 / 9.3-18 | 43-58 / 64-96 | 54-71 / 135-188 | 20,868-20,955 / 41,995-56,980 | 379-41,137 / 56,074-72,264 |
| d1000s10 | 2,527-22,037 / 42,345-50,817 | 5.7-6.4 / 7.8-19 | 5.9-7.7 / 9.0-15 | 338-344 / 509-579 | 351-394 / 501-799 | 972-21,901 / 43,610-57,882 | 1,097-21,908 / 42,923-57,516 |

- Jobs, one strict job per fixture: 261111-085642-fdea98 (d1s1), 261111-085644-609714
  (d1000s1), 261111-035757-f9dbcf (d1000s10).
- A5 on macOS, cache warm: the wake (warm defer) is below cold at p50 and p99 on every fixture.
  No interval overlaps: the slowest warm defer is below the fastest cold at p50 and at p99. The
  margin at p50 is smallest in a pair whose cold opens ran fast: 3.1 times on d1s1 (264 against
  85 µs), 5.3 times on d1000s1 (379 against 71 µs) and 2.8 times on d1000s10 (1,097 against
  394 µs); in the other pairs it is 60 to 760 times. `promote()` costs 90 to 116 µs at p50 with
  one segment and 56 to 63 µs per segment with ten (134 to 239 µs p99), above the design's 33 to
  50 µs estimate.
- The margin rests on the open's two syncs (Findings): cold's p50 is 20 to 41 ms because of them,
  and where they ran fast cold was 212 to 1,097 µs and the margin 2.8 to 5.3 times. If the sync
  deferral lands, T2d is rerun before D3's latency sentence stands on it.
- An open costs 21 to 41 ms at p50 in most pairs and about 1 to 2.5 ms in a few: the two syncs of
  every open (above) dominate it.

**T3, a count far above the caps** (261111-034629-8f5208): 50,000 one-document indexes against
soft_open 2,000 and soft_hot 500, a touch of every index then 100,000 Zipf 0.99 requests at 8
threads. Zero `ENFILE`, `EMFILE`, `ENOMEM`, `too_many_open`; peaks below the hard caps; open
2,000 and hot 426 3 s after the requests end (the harness's PASS). The soft caps after the touch
miss the design's target (verdicts below). Peaks reached open 4,905 and hot 3,241 during the
touch, and open 2,355 and hot 811 after it; the kernel's peak was 8,106 descriptors, 3,414
mappings and 11,355 vnodes against hard caps of 995,174 descriptors and 157,900 vnodes. The
sampler reads the budget, then the kernel (`PROC_PIDLISTFDS` and the whole `mach_vm_region`
walk), then the budget again, and reports the kernel's descriptors beyond the larger read: at
most 11. The steady excess is 3, the stdio descriptors (193 of 309 samples); the rest are
short-lived descriptors opened inside the window, at most one per concurrent chore (eight), so
the bound grows with the chores' concurrency and the window's width, not the index count (the
earlier 1,050 were the sampler's own skew, `fd-budget.md`). Touch at 709 opens/s, Zipf at 355
requests/s, misses paying the open's syncs.

**T4, hit ratio against the S3-FIFO models** (261111-035757-a2d41c, 261111-035757-bbb067):
20,000 indexes, soft_open 1,000, soft_hot 250, 20,000 warm-up and 40,000 measured requests.

| Theta | Scan | Hit ratio | s8_variants | Gate (model less 1 point) | Hottest key pin+unpin, by key / by handle |
|---|---|---|---|---|---|
| 0.99 | no | 0.6824 | 0.6753 | PASS | 4,525 / 648 ns |
| 0.99 | yes | 0.6806 | 0.6751 | PASS | 4,394 / 603 ns |
| 1.2 | no | 0.8735 | 0.8717 | PASS | 5,097 / 548 ns |
| 1.2 | yes | 0.8735 | 0.8713 | PASS | 4,031 / 738 ns |

No kernel error and no `too_many_open` in any cell.

## Verdicts against the design's targets

Each from the jobs above, macOS, cache warm, scaled under D59. T2's are judged pair by pair
within a job (an open plus `get` as the sum of their p50s and of their p99s).

- **T1 hot, within 5% of the baseline:** MET through K6 (A9), the design's procedure: heap equal
  to main's, RSS -0.0% to +3.3%. Through the catalog hot costs 650 B (+10%) of heap with one
  segment.
- **T1 warm, the sum of its pieces plus 7%, 0 maps:** 0 maps MET (0.03 per index, equal to cold's:
  the harness's own regions). The heap is not judgeable from these jobs: no job measures the
  pieces at this commit, and N1's probe ran on code where main's hot was 5,394 B, not 6,484 B.
  Indicative only, from N1's pieces: hot 7,134 B less the segment objects 1,866 B plus D3's
  record of about 200 B gives 5,468 B, plus 7% 5,851 B, above the measured 5,084 B.
- **T1 cold, ≤ 16 B, 0 fds, 0 maps:** MISS: 292 B per index (the catalog's entry for the key);
  0 fds and no map of its own met.
- **T2 hot on d1s1, within 5% of main at p50 and p99:** MISS: p50 +56%, +40% and -7% over the
  three pairs (0.9 vs 1.4, 1.5 vs 2.1, 1.4 vs 1.3 µs); p99 +27%, +29%, +16%. Hot through the
  catalog adds a request pin by key, about 0.5 µs, to main's bare `get`.
- **T2 cold, within 10% of open plus `get`:** d1s1 MET (at most +1.7% at p50 and +5.0% at p99;
  one pair's cold ran 77% faster). d1000s1 MISS in one pair: +1,538% at p50 and +35% at p99
  (main's opens 1.3 ms, cold's 20.9 ms); the other two within or below. d1000s10 MISS in two
  pairs: +765% at p50 (2.5 against 21.9 ms) and +13.9% at p99. The open is bimodal, about 1 ms
  or 21 to 41 ms by its two syncs, so a pair whose sides land on different modes misses by that.
- **T2d, warm below cold at p50 and p99:** MET on every fixture (A5 above).
- **T3, within one sweep pass of the soft caps after the touch ends:** MISS. Over the 248 samples
  after the touch (about 1.15 s apart; the sweep's period is 1 s and a pass that leaves the caps
  exceeded reposts at once, 10,892 passes in the run), open exceeds 2,016 in 212 and hot exceeds
  516 in 167, up to open 2,355 (+18%) and hot 811 (+62%); medians 2,055 and 545. Both stay within
  only from the first sample after the Zipf requests end, about 282 s after the touch. Likely
  cause, not measured: under load the counts carry the transitions the sweep has posted but the
  eight chores have not yet run, queued behind opens paying their two syncs.
- **T3, zero kernel errors and `too_many_open`, peaks below the hard caps:** MET.
- **T4, hit ratio at least the model's less 1 point:** MET in 4 of 4 cells.
