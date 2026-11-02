# Library performance issues filed 2026-11-01/02

> Status: CURRENT (2026-11-02). The owner asked that every issue the cpp-libraries gate filed under
> his account be addressed. Each gets an explorer: reproduce the issue's numbers on the library's
> head under a strict hold with an A/A control, find the cause, and prototype a fix. A verified win
> goes to a branch with CI and lands; a loss or a justified trade is written on the issue, which is
> then closed with the reason or left open with what would settle it.

Owner (2026-11-02): take only the valuable ones for now. Chosen by impact on the release's hot paths:

| Issue | Why | State |
|---|---|---|
| stash#1 | claim hint publish cost depth-64 p99 | landed stash:d2e8e59: one release store per chunk after the fill; depth-64 p99 back at the pre-hint level on the i9 (0.989/0.995 vs A/A 0.993/0.999), deep rows keep their win; M4 and Linux unmeasured |
| threadpool#3 | the trickle p99 | closed: a measurement artifact (start ran 70-80 ms behind schedule); bench gains an A/A row and a from-due p99 |
| strict-stox#2 | integer parsing on every document write | closed: does not reproduce on the i9 (head 2.5% faster); the same source swings ~20% with placement (strict-stox:1f9190c `docs/perf/stoi-short.md`) |
| threadpool#2 | `ThreadPool` start delay against Asio | moot: `ThreadPool` is being removed; scheduler moves to `WorkPool` (owner, 2026-11-02) |
| termdict#1 | point lookup, the term path every query takes | explored, open: entry walk is about half of 364 ns (about 10 ns per entry); three experiments lost (termdict docs/perf/point-lookup.md, explore/point-lookup c95bb4f). Next: measure its share of strata's query and upsert profiles before a format-changing skip table |
| docset#1 | bitmap AND/ANDNOT under prospect's boolean queries | closed: code placement (aligning the loop moves which rows are slow; start's own ANDNOT rows are the slow ones) |

Deferred, issues left open: journal#1 until 7b (the journal replaces the WAL there); allocators#1
(Xapiand uses only `total_allocated()`; started and stopped); every other issue below, revisited
after the release.

## The full list, as filed

Order: regressions against the pre-upgrade start first (the upgrade made them worse), then gaps on
strata's and Xapiand's hot paths, then gaps against outside libraries off our path.

## Wave 1: regressions against start

| Issue | What | State |
|---|---|---|
| stash#1 | claim hint: depth-64 p99 4-12% worse; b0fce1c's compromise never strict-timed | running |
| allocators#1 | one-block pool pair 0.74 to 1.00 ns on the i9 (copy-assignment fix) | running |
| threadpool#3 | trickle gap=1000 delay p99 on the i9 slower than e961e44 | running |
| strict-stox#2 | stoi 1-9 digits 24% slower than 752cf9f on the i9 | running |
| strings#2 | join of 1, 3, 4 strings and few-match replace 0.94-0.96 of start | queued |
| times#1 | `timespec_t(double)` over an array 0.92 / 0.89 of start | queued |
| fs#1 | `np_relative` slower than start on the M4 | queued |
| enum-reflection#1 | 8-member hit 6% slower than start on the M4 | queued |
| docset#1 | bitmap AND/ANDNOT of 20 and 40 members slower than start on the i9 | queued |

## Wave 2: our hot paths

journal#1 (a durable append makes three full flushes against one), termdict#1 (point lookup),
threadpool#2, reactor#1, http#1, scheduler#1, logger#1, varint#1, crc32c#1, compressors#6,
unicode#1, strict-stox#1, char-classify#1.

## Wave 3: off our path

double-metaphone#1, htm#1, namegen#1, generation-cache#1, cartesian#1, constexpr-phf#2, escape#1,
repr#1, strings#1, string-similarity#1, term-color#1, queue#1.
