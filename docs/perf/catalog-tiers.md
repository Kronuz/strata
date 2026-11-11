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
  busy host, so it is reported, not compared). The row passes on zero `ENFILE`, `EMFILE`, `ENOMEM`
  and `too_many_open`, every peak below the budget's hard caps, and open and hot counts within one
  pass (`pass_posts`, 16) of the soft caps 3 s after the requests end.
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
tiers.sh t2 OUT d1s1 500 3                   # T2/T2d, with d1000s1 and d1000s10 in the same job
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

macOS x86_64 (the development Mac), cache warm, strict jobs at strata:dbfc1898 with main
strata:15d39c30's `idle_index` interleaved. Each range is the spread of the job's pairs (D59).

**T1, memory per tier** (261110-211221-3a7f09; 1,000 indexes, 2 pairs). Heap per index from the
counting build is exact; RSS per index at 1,000 indexes carries about 5 KB of process baseline, so
it is not comparable with the 5.9 KB target set at 100,000.

| Fixture | main heap | hot | warm | cold | hot RSS vs main | maps per index hot / warm / cold |
|---|---|---|---|---|---|---|
| d1s1 | 6,484 B | 7,134 B | 5,084 B | 292 B | 12,730 vs 11,555 B (+10%) | 1.03 / 0.03 / 0.03 |
| d1000s1 | 6,849 B | 7,499 B | 5,204 B | 292 B | 21,324 vs 20,120 B (+6%) | 1.03 / 0.03 / 0.03 |

- Hot through the catalog costs 650 B of heap per index over main's open: the slot and its
  payload (5 allocations). `idle_index`'s own hot, without the catalog, matches main exactly
  (K6 below).
- Cold keeps 292 B and 1.08 allocations per index: the catalog's entry for the key. T1's "cold
  ≤ 16 B" is not met at step 2.
- defer_io changes no row. Warm holds one descriptor per index and no map.
- The 10-segment rows at dbfc1898 (261110-211223-969922) are void: the fixtures held one segment
  (main's `idle_index` ignores SEGMENTS). Rerun on rebuilt fixtures: 261111-010418-28f20d.

**K6, T1's step-1 rows** (261110-211225-45bedb; `idle_index` alone, 1,000 indexes, 2 pairs): hot's
heap equals main's on every fixture (6,484, 6,518 and 27,798 B on d1s1, d100s1, d100s10), and its
RSS is within -2.0% to +1.4% of main's. Warm: 4,798, 4,806 and 11,110 B of heap, 0.02 maps per
index. Lazy: 4,826, 4,834 and 11,138 B, 0.01 maps.

**T2 and T2d, first query per tier** (261110-211226-81fd7c; 500 indexes, 3 pairs; p50 and p99 in
µs, the range over the pairs):

| Fixture | main open | main query | hot | warm | warm defer (T2d) | cold | cold defer |
|---|---|---|---|---|---|---|---|
| d1s1 | 747-21,920 / 37,392-59,099 | 0.7-1.5 / 1.3-5.4 | 1.7-2.7 / 3.2-5.4 | 40-47 / 65-69 | 54-90 / 119-229 | 20,986-43,097 / 58,087-74,984 | 20,963-21,069 / 45,097-58,603 |
| d1000s1 | 1,223-21,036 / 3,746-58,236 | 4.8-7.9 / 13-19 | 7.8-9.8 / 13-14 | 42-70 / 74-107 | 54-72 / 149-160 | 262-41,761 / 57,115-74,352 | 20,029-21,076 / 42,065-57,141 |

- A5 on macOS, cache warm: the wake (warm defer) is below cold at p50 and p99 on every fixture, by
  about 300 times at p50. The 10-segment row reruns as 261111-010421-b797a5 (d1000s10 at dbfc1898
  held one segment).
- An open costs 21 ms at p50 in most pairs and about 1 ms in a few: the two syncs of every open
  (below) dominate it.

**T3, a count far above the caps** (261110-211228-c5746c): 50,000 one-document indexes against
soft_open 2,000 and soft_hot 500, a touch of every index then 100,000 Zipf 0.99 requests at 8
threads. PASS: zero `ENFILE`, `EMFILE`, `ENOMEM`, `too_many_open`; peaks below the hard caps;
back to open 2,000 and hot 461 within a pass after. Peaks during the run reached open 8,821 and
hot 4,846 (the sweep catches up a pass behind). The kernel showed up to 1,050 descriptors beyond
the budget's count. Touch at 1,336 opens/s; Zipf at 361 requests/s, misses paying the open's
syncs.

**T4, hit ratio against the S3-FIFO models** (261110-211229-6ec5bb, 261110-211231-026a08):
20,000 indexes, soft_open 1,000, soft_hot 250, 20,000 warm-up and 40,000 measured requests.

| Theta | Scan | Hit ratio | s8_variants | Gate (model less 1 point) | Hottest key pin+unpin, by key / by handle |
|---|---|---|---|---|---|
| 0.99 | no | 0.6813 | 0.6753 | PASS | 5,263 / 572 ns |
| 0.99 | yes | 0.6812 | 0.6751 | PASS | 3,961 / 576 ns |
| 1.2 | no | 0.8729 | 0.8717 | PASS | 6,280 / 713 ns |
| 1.2 | yes | 0.8743 | 0.8713 | PASS | 5,725 / 638 ns |

No kernel error and no `too_many_open` in any cell.
