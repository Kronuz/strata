# Manifest retention: what it buys and what it costs (explore/retention)

Question: `retained_predecessors = 2` (I14) keeps the two manifests behind the head, which pins merged-away segments. What does each policy recover under damage, what does it cost on disk and in throughput, and what does a "trim once settled" policy give?

Policies: keep 0, keep 1, keep 2 (`retained_predecessors`), and trim settled (prototype, `options::trim_settled`): after each commit the head manifest is read back from its file, each new segment is read whole and verified, live-docs and metadata files are checked by CRC; once all pass, the predecessors are dropped, otherwise up to 2 are kept. After open the predecessors stay until the first verified commit.

Code: `settle_head()` and the `want` count in `retain()` in include/strata/core/index.hh; `test/retention_test.cc` (target `retention_test`, not a ctest); `KV_RETAIN`/`KV_TRIM` and a settle that drains reclaim in bench/kv/kv_strata.cc; a peak-disk sampler (100 ms, all policies alike) in bench/kv/kvdriver.hh. The env default `STRATA_TRIM_SETTLED` on the option is prototype-only, for running the crash suites.

## 1. Recovery under damage

Harness: faultfs, a seeded workload (5 flush commits of 30 operations, a force merge to one segment, optionally 2 more flush commits), 300 seeds per cell (random offset per seed). Recovery is what a host does: open the head and verify everything (`check()`, `heal()`); if that fails, `open(..., to)` each older manifest newest first, verifying each, until one opens. Outcomes: head opens (every acknowledged document), fallback with all data, fallback with documents missing, refused (no manifest opens: needs a replica or repair). "docs lost per loss" counts acknowledged documents missing after a lossy fallback. Rows where the head opens in about 1% of C cases are flips in bytes nothing reads.

Scenarios: A power loss at every mutation of a merge commit (new documents applied just before it; 12 crash seeds per point; the head must hold the pre- or post-merge content); B current manifest flipped or torn; C newest segment flipped after its commit; D1/D2 a segment only a predecessor names flipped (alone / with the newest segment); E merged segment damaged (sticky read fault) before the merge commits, standing for a write-path fault the read-back can meet. "(+2 commits)" means two flush commits follow the merge, so the merge is not the newest.

| scenario | policy | runs | head opens | fallback, all data | fallback, data lost | refused | docs lost per loss |
|---|---|---|---|---|---|---|---|
| A power loss at each of 24 mutations of a merge commit | keep 0 | 288 | 100.0% | 0.0% | 0.0% | 0.0% | 0.0 |
| B current manifest bit-flipped (merge is head) | keep 0 | 300 | 0.0% | 0.0% | 0.0% | 100.0% | 0.0 |
| B current manifest torn (merge is head) | keep 0 | 300 | 0.0% | 0.0% | 0.0% | 100.0% | 0.0 |
| C newest segment bit-flipped (merge is head) | keep 0 | 300 | 0.7% | 0.0% | 0.0% | 99.3% | 0.0 |
| D1 predecessor's segment flipped (merge is head) | keep 0 | n/a | | | | | |
| D2 predecessor's segment and newest segment flipped (merge is head) | keep 0 | n/a | | | | | |
| B current manifest bit-flipped (+2 commits) | keep 0 | 300 | 0.0% | 0.0% | 0.0% | 100.0% | 0.0 |
| B current manifest torn (+2 commits) | keep 0 | 300 | 0.0% | 0.0% | 0.0% | 100.0% | 0.0 |
| C newest segment bit-flipped (+2 commits) | keep 0 | 300 | 1.0% | 0.0% | 0.0% | 99.0% | 0.0 |
| D1 predecessor's segment flipped (+2 commits) | keep 0 | n/a | | | | | |
| D2 predecessor's segment and newest segment flipped (+2 commits) | keep 0 | n/a | | | | | |
| E merged segment damaged as it commits | keep 0 | 300 | 0.3% | 0.0% | 0.0% | 99.7% | 0.0 |
| A power loss at each of 18 mutations of a merge commit | keep 1 | 216 | 100.0% | 0.0% | 0.0% | 0.0% | 0.0 |
| B current manifest bit-flipped (merge is head) | keep 1 | 300 | 0.0% | 100.0% | 0.0% | 0.0% | 0.0 |
| B current manifest torn (merge is head) | keep 1 | 300 | 0.0% | 100.0% | 0.0% | 0.0% | 0.0 |
| C newest segment bit-flipped (merge is head) | keep 1 | 300 | 0.7% | 99.3% | 0.0% | 0.0% | 0.0 |
| D1 predecessor's segment flipped (merge is head) | keep 1 | 300 | 100.0% | 0.0% | 0.0% | 0.0% | 0.0 |
| D2 predecessor's segment and newest segment flipped (merge is head) | keep 1 | 300 | 0.7% | 1.3% | 0.0% | 98.0% | 0.0 |
| B current manifest bit-flipped (+2 commits) | keep 1 | 300 | 0.0% | 0.0% | 100.0% | 0.0% | 27.0 |
| B current manifest torn (+2 commits) | keep 1 | 300 | 0.0% | 0.0% | 100.0% | 0.0% | 27.0 |
| C newest segment bit-flipped (+2 commits) | keep 1 | 300 | 1.0% | 0.0% | 99.0% | 0.0% | 27.0 |
| D1 predecessor's segment flipped (+2 commits) | keep 1 | n/a | | | | | |
| D2 predecessor's segment and newest segment flipped (+2 commits) | keep 1 | n/a | | | | | |
| E merged segment damaged as it commits | keep 1 | 300 | 0.3% | 99.7% | 0.0% | 0.0% | 0.0 |
| A power loss at each of 17 mutations of a merge commit | keep 2 | 204 | 100.0% | 0.0% | 0.0% | 0.0% | 0.0 |
| B current manifest bit-flipped (merge is head) | keep 2 | 300 | 0.0% | 100.0% | 0.0% | 0.0% | 0.0 |
| B current manifest torn (merge is head) | keep 2 | 300 | 0.0% | 100.0% | 0.0% | 0.0% | 0.0 |
| C newest segment bit-flipped (merge is head) | keep 2 | 300 | 0.7% | 99.3% | 0.0% | 0.0% | 0.0 |
| D1 predecessor's segment flipped (merge is head) | keep 2 | 300 | 100.0% | 0.0% | 0.0% | 0.0% | 0.0 |
| D2 predecessor's segment and newest segment flipped (merge is head) | keep 2 | 300 | 0.7% | 1.3% | 4.7% | 93.3% | 24.0 |
| B current manifest bit-flipped (+2 commits) | keep 2 | 300 | 0.0% | 0.0% | 100.0% | 0.0% | 27.0 |
| B current manifest torn (+2 commits) | keep 2 | 300 | 0.0% | 0.0% | 100.0% | 0.0% | 27.0 |
| C newest segment bit-flipped (+2 commits) | keep 2 | 300 | 1.0% | 0.0% | 99.0% | 0.0% | 27.0 |
| D1 predecessor's segment flipped (+2 commits) | keep 2 | n/a | | | | | |
| D2 predecessor's segment and newest segment flipped (+2 commits) | keep 2 | n/a | | | | | |
| E merged segment damaged as it commits | keep 2 | 300 | 0.3% | 99.7% | 0.0% | 0.0% | 0.0 |
| A power loss at each of 24 mutations of a merge commit | trim settled | 288 | 100.0% | 0.0% | 0.0% | 0.0% | 0.0 |
| B current manifest bit-flipped (merge is head) | trim settled | 300 | 0.0% | 0.0% | 0.0% | 100.0% | 0.0 |
| B current manifest torn (merge is head) | trim settled | 300 | 0.0% | 0.0% | 0.0% | 100.0% | 0.0 |
| C newest segment bit-flipped (merge is head) | trim settled | 300 | 0.7% | 0.0% | 0.0% | 99.3% | 0.0 |
| D1 predecessor's segment flipped (merge is head) | trim settled | n/a | | | | | |
| D2 predecessor's segment and newest segment flipped (merge is head) | trim settled | n/a | | | | | |
| B current manifest bit-flipped (+2 commits) | trim settled | 300 | 0.0% | 0.0% | 0.0% | 100.0% | 0.0 |
| B current manifest torn (+2 commits) | trim settled | 300 | 0.0% | 0.0% | 0.0% | 100.0% | 0.0 |
| C newest segment bit-flipped (+2 commits) | trim settled | 300 | 1.0% | 0.0% | 0.0% | 99.0% | 0.0 |
| D1 predecessor's segment flipped (+2 commits) | trim settled | n/a | | | | | |
| D2 predecessor's segment and newest segment flipped (+2 commits) | trim settled | n/a | | | | | |
| E merged segment damaged as it commits | trim settled | 300 | 0.3% | 99.7% | 0.0% | 0.0% | 0.0 |

Reading, from the table:

1. Power loss (A): all four policies opened the head with the full content at every point (about 200-290 runs each). Retention is not what protects against power loss; the commit protocol is. The trim prototype's extra reads add no mutations; the lifecycle crash suite (10,000 crash points and background merges) passes with trim forced on, skipping its `recover --to` block.
2. Keep 0 and trim settled: any damaged current manifest or newest segment is refused (99-100%), under both layouts.
3. Keep 1 and keep 2 behave identically in B, C and E. With the merge as head they fall back with all data (the merge commit preserves content). With two commits after the merge they fall back to the commit before last and lose that last commit (27 documents here): a predecessor is the previous commit, not the previous merge.
4. Keep 2 beyond keep 1 shows up only in D2 (a predecessor's segment and the newest damaged): keep 1 refuses 98%, keep 2 falls back to the older manifest in 6% of runs, 4.7% of them with 24 documents silently missing. So in these scenarios the second predecessor mostly added a path to an older, lossy state, not extra recovery.
5. D1: a damaged predecessor-only segment does not affect open under keep 1/2; it only silently removes that fallback. Keep 0 and trim hold no such files.
6. Trim settled equals keep 0 for damage that arrives after the commit (B, C, D). It recovers E (damage present at commit time) in 99.7% of runs, as keep 1 and 2 do, because the read-back fails and the predecessors stay. Caveat: on a real file system the read-back is served from the page cache, so it detects what the writer wrote wrong or what the cache holds, not a later media fault; the faultfs model here reads the damaged medium. A real deployment would need an uncached read for that claim.

## 2. Cost

Rows: fillrandom, overwrite, ycsb_a at 100 B and 1 KiB, fits (256 MB budget) and 2x (budget half the data), one thread, no durability, 100,000 records, run capped at 20 s. 7 interleaved trials, policy order rotated per row and trial, "keep 2 again" is the A/A. Run under a strict hold (`quiet run explore-retention 40`). An earlier partial pass under a light hold (load average 16) was discarded for throughput; its disk numbers matched. Settled disk is file bytes after the final settle with the reclaim queue drained (the driver now drains it for every policy), median (min-max); peak is the largest of 100 ms samples during the run. ops/s is the run phase median; "vs keep 2" is the policy median over keep 2's median (above 1 means faster).

| workload | value | size | policy | n | settled MiB | peak MiB | ops/s median | ops/s min-max | vs keep 2 |
|---|---|---|---|---|---|---|---|---|---|
| fillrandom | 100 | fits | keep 2 | 7 | 6.9 (6.9-6.9) | 6.9 (6.9-6.9) | 1179k | 1145-1329k | 1.000 |
| fillrandom | 100 | fits | keep 2 again | 7 | 6.9 (6.9-6.9) | 6.9 (6.9-6.9) | 1310k | 1080-1339k | 1.111 |
| fillrandom | 100 | fits | keep 1 | 7 | 6.9 (6.9-6.9) | 6.9 (6.9-6.9) | 1238k | 1125-1304k | 1.050 |
| fillrandom | 100 | fits | keep 0 | 7 | 6.9 (6.9-6.9) | 6.9 (6.9-6.9) | 1208k | 1124-1316k | 1.024 |
| fillrandom | 100 | fits | trim settled | 7 | 6.9 (6.9-6.9) | 6.9 (6.9-6.9) | 1297k | 1153-1435k | 1.100 |
| fillrandom | 100 | 2x | keep 2 | 7 | 6.9 (6.8-6.9) | 23.5 (17.4-26.0) | 274k | 213-317k | 1.000 |
| fillrandom | 100 | 2x | keep 2 again | 7 | 6.9 (6.8-6.9) | 24.1 (17.8-26.1) | 276k | 207-335k | 1.007 |
| fillrandom | 100 | 2x | keep 1 | 7 | 6.9 (6.8-6.9) | 24.5 (18.2-25.7) | 236k | 219-294k | 0.863 |
| fillrandom | 100 | 2x | keep 0 | 7 | 6.9 (6.9-6.9) | 24.4 (18.9-27.1) | 284k | 217-320k | 1.035 |
| fillrandom | 100 | 2x | trim settled | 7 | 6.9 (6.8-6.9) | 23.7 (18.0-25.9) | 245k | 208-319k | 0.895 |
| fillrandom | 1024 | fits | keep 2 | 7 | 58.2 (58.2-58.2) | 58.2 (58.2-58.2) | 248k | 202-254k | 1.000 |
| fillrandom | 1024 | fits | keep 2 again | 7 | 58.2 (58.2-58.2) | 58.2 (58.2-58.2) | 245k | 231-251k | 0.987 |
| fillrandom | 1024 | fits | keep 1 | 7 | 58.2 (58.2-58.2) | 58.2 (58.2-58.2) | 244k | 217-258k | 0.986 |
| fillrandom | 1024 | fits | keep 0 | 7 | 58.2 (58.2-58.2) | 58.2 (58.2-58.2) | 248k | 220-254k | 1.000 |
| fillrandom | 1024 | fits | trim settled | 7 | 58.2 (58.2-58.2) | 58.2 (58.2-58.2) | 245k | 214-253k | 0.988 |
| fillrandom | 1024 | 2x | keep 2 | 7 | 58.2 (58.2-58.2) | 104.5 (104.5-116.3) | 196k | 190-204k | 1.000 |
| fillrandom | 1024 | 2x | keep 2 again | 7 | 58.2 (58.2-58.2) | 104.5 (104.5-104.5) | 197k | 192-207k | 1.004 |
| fillrandom | 1024 | 2x | keep 1 | 7 | 58.2 (58.2-58.2) | 104.5 (104.5-104.5) | 194k | 192-213k | 0.992 |
| fillrandom | 1024 | 2x | keep 0 | 7 | 58.2 (58.2-58.2) | 104.5 (104.5-104.5) | 198k | 193-204k | 1.011 |
| fillrandom | 1024 | 2x | trim settled | 7 | 58.2 (58.2-58.2) | 104.5 (104.5-104.5) | 200k | 193-205k | 1.022 |
| overwrite | 100 | fits | keep 2 | 7 | 13.8 (13.8-13.8) | 13.8 (13.8-13.8) | 853k | 755-878k | 1.000 |
| overwrite | 100 | fits | keep 2 again | 7 | 13.8 (13.8-13.8) | 13.8 (13.8-13.8) | 841k | 746-898k | 0.986 |
| overwrite | 100 | fits | keep 1 | 7 | 13.8 (13.8-13.8) | 13.8 (13.8-13.8) | 842k | 718-878k | 0.987 |
| overwrite | 100 | fits | keep 0 | 7 | 6.9 (6.9-6.9) | 13.8 (13.8-13.8) | 826k | 726-900k | 0.968 |
| overwrite | 100 | fits | trim settled | 7 | 6.9 (6.9-6.9) | 13.8 (13.8-13.8) | 868k | 745-906k | 1.017 |
| overwrite | 100 | 2x | keep 2 | 7 | 14.7 (14.1-15.2) | 46.2 (40.8-53.2) | 120k | 101-134k | 1.000 |
| overwrite | 100 | 2x | keep 2 again | 7 | 14.0 (13.8-14.6) | 46.9 (44.6-52.8) | 121k | 108-123k | 1.004 |
| overwrite | 100 | 2x | keep 1 | 7 | 15.0 (14.1-15.5) | 43.1 (36.1-48.1) | 131k | 114-150k | 1.091 |
| overwrite | 100 | 2x | keep 0 | 7 | 7.9 (7.0-8.6) | 45.7 (37.2-51.7) | 122k | 107-138k | 1.019 |
| overwrite | 100 | 2x | trim settled | 7 | 7.6 (6.9-8.4) | 45.5 (43.1-54.7) | 117k | 100-148k | 0.976 |
| overwrite | 1024 | fits | keep 2 | 7 | 130.7 (130.7-130.7) | 146.7 (146.7-146.7) | 218k | 198-223k | 1.000 |
| overwrite | 1024 | fits | keep 2 again | 7 | 130.7 (130.7-130.7) | 146.7 (146.7-146.7) | 213k | 203-227k | 0.977 |
| overwrite | 1024 | fits | keep 1 | 7 | 130.7 (130.7-130.7) | 146.7 (146.7-146.7) | 219k | 190-229k | 1.006 |
| overwrite | 1024 | fits | keep 0 | 7 | 72.5 (72.5-72.5) | 146.7 (146.7-146.7) | 215k | 203-230k | 0.987 |
| overwrite | 1024 | fits | trim settled | 7 | 72.5 (72.5-72.5) | 146.7 (146.7-146.7) | 217k | 201-220k | 0.995 |
| overwrite | 1024 | 2x | keep 2 | 7 | 122.7 (119.1-129.3) | 357.6 (308.8-413.3) | 50k | 43-54k | 1.000 |
| overwrite | 1024 | 2x | keep 2 again | 7 | 128.3 (116.4-130.1) | 339.0 (324.2-417.9) | 51k | 43-56k | 1.030 |
| overwrite | 1024 | 2x | keep 1 | 7 | 116.4 (116.4-128.2) | 416.7 (334.0-442.0) | 51k | 43-56k | 1.029 |
| overwrite | 1024 | 2x | keep 0 | 7 | 59.0 (58.2-71.9) | 382.1 (323.1-416.8) | 51k | 42-55k | 1.017 |
| overwrite | 1024 | 2x | trim settled | 7 | 59.2 (58.2-61.0) | 370.1 (359.0-416.9) | 51k | 51-57k | 1.028 |
| ycsb_a | 100 | fits | keep 2 | 7 | 13.8 (13.8-13.8) | 13.8 (13.8-13.8) | 822k | 684-850k | 1.000 |
| ycsb_a | 100 | fits | keep 2 again | 7 | 13.8 (13.8-13.8) | 13.8 (13.8-13.8) | 836k | 704-875k | 1.018 |
| ycsb_a | 100 | fits | keep 1 | 7 | 13.8 (13.8-13.8) | 13.8 (13.8-13.8) | 836k | 697-876k | 1.017 |
| ycsb_a | 100 | fits | keep 0 | 7 | 6.9 (6.9-6.9) | 13.8 (13.8-13.8) | 816k | 704-876k | 0.993 |
| ycsb_a | 100 | fits | trim settled | 7 | 6.9 (6.9-6.9) | 13.8 (13.8-13.8) | 826k | 690-890k | 1.005 |
| ycsb_a | 100 | 2x | keep 2 | 7 | 14.3 (13.9-14.7) | 23.9 (23.3-26.2) | 287k | 222-304k | 1.000 |
| ycsb_a | 100 | 2x | keep 2 again | 7 | 14.3 (13.9-15.5) | 25.8 (20.8-27.8) | 210k | 166-301k | 0.733 |
| ycsb_a | 100 | 2x | keep 1 | 7 | 14.3 (14.0-15.0) | 23.1 (18.4-28.3) | 275k | 191-294k | 0.958 |
| ycsb_a | 100 | 2x | keep 0 | 7 | 7.7 (7.5-8.2) | 24.3 (18.4-27.5) | 274k | 190-298k | 0.955 |
| ycsb_a | 100 | 2x | trim settled | 7 | 7.6 (7.3-8.2) | 24.5 (20.3-25.8) | 286k | 185-301k | 0.995 |
| ycsb_a | 1024 | fits | keep 2 | 7 | 97.1 (97.1-97.1) | 97.1 (97.1-97.1) | 533k | 481-568k | 1.000 |
| ycsb_a | 1024 | fits | keep 2 again | 7 | 97.1 (97.1-97.1) | 97.1 (97.1-97.1) | 509k | 451-545k | 0.953 |
| ycsb_a | 1024 | fits | keep 1 | 7 | 97.1 (97.1-97.1) | 97.1 (97.1-97.1) | 492k | 472-536k | 0.923 |
| ycsb_a | 1024 | fits | keep 0 | 7 | 66.2 (66.2-66.2) | 97.1 (97.1-97.1) | 507k | 495-533k | 0.950 |
| ycsb_a | 1024 | fits | trim settled | 7 | 66.2 (66.2-66.2) | 97.1 (97.1-97.1) | 532k | 468-544k | 0.998 |
| ycsb_a | 1024 | 2x | keep 2 | 7 | 117.8 (116.5-118.3) | 142.9 (142.3-201.6) | 266k | 192-306k | 1.000 |
| ycsb_a | 1024 | 2x | keep 2 again | 7 | 118.2 (116.5-126.2) | 162.9 (142.8-217.7) | 211k | 114-283k | 0.794 |
| ycsb_a | 1024 | 2x | keep 1 | 7 | 117.8 (116.5-121.7) | 201.9 (142.3-224.5) | 201k | 81-299k | 0.755 |
| ycsb_a | 1024 | 2x | keep 0 | 7 | 60.1 (58.3-60.2) | 143.1 (142.3-201.7) | 241k | 181-318k | 0.906 |
| ycsb_a | 1024 | 2x | trim settled | 7 | 59.0 (58.3-60.2) | 195.2 (142.8-215.4) | 208k | 120-287k | 0.782 |

Reading:

1. Settled disk: retention matters where merges happen. ycsb_a 1 KiB fits: keep 2 and keep 1 97.1 MiB, keep 0 and trim 66.2 MiB (32% smaller). overwrite 1 KiB fits 130.7 vs 72.5 MiB (45% smaller); at 100 B 13.8 vs 6.9 MiB (50% smaller). At 2x, ycsb_a 1 KiB 118 vs 59-60 MiB, overwrite 1 KiB about 120 vs 59, 100 B about 14.3 vs 7.6. fillrandom has no merge in these runs: all policies equal.
2. Keep 1 saves nothing over keep 2 here: the head's immediate predecessor already names all the merge inputs.
3. Peak disk does not move with the policy (146.7 MiB for overwrite 1 KiB fits in every policy; 2x medians overlap within their ranges): the peak is inputs plus outputs during a merge, and retention only lengthens how long the inputs stay after it.
4. Throughput: no policy differs from keep 2 by more than the A/A spread. The A/A ratios run 0.95-1.11 on fits rows and fall to 0.73-0.79 on the 2x ycsb_a rows, whose ranges (for example 114-283k ops/s for keep 2 again) are too wide for any claim there. The trim read-back costs no throughput that these rows can see; it reads files just written (cached) and does not appear in the run phase.

## Not measured

Read-back cost on a cold cache, the settle-phase duration, multi-shard and 4-thread rows, a replicated deployment (where "refused" means repair from a replica), and Linux or FreeBSD (this host is Darwin x86_64).
