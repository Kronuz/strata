> Status: MEASURED (2026-11-21) on `explore/xapiand/eager-open` (strata:7107f9bc) and prospect's
> `explore/xapiand/eager-open`. The harness (`bench/eager_open`) and the field check at each map
> (`docs/mechanisms/field-check.md`) land with field-check-at-map; its rows are the last section.

# What one open index costs Xapiand, eager against lazy

Question (Xapiand hunt list, "Xapiand nodes open every index eagerly"): what does each open index
cost Xapiand today, and how much would lazy segments (cold-tier K7) save? The owner's workload is
millions of indexes per host, so the meters are per open index on one thread.

## Where the eager path is

Strata main (strata:efa5214b):

- `include/strata/core/index.hh:765-768`: `eager_segments` is off by default; a segment maps at its
  first read.
- `index.hh:775-778` and `:860`, `:877`: `defer_io` forces `eager_segments` on in `create()` and
  `open()`, because a first-read map would be I/O inside `apply()`. `load_files` (`:5846`) then maps
  every segment at open.

Xapiand master (`src/engine/strata`, `src/database`):

- `src/database/pool.cc:605-618`: the catalog's `open_index` hook sets `host.host = true` and opens
  with `engine::strata::index_options(host)`, which sets `options.defer_io = host.host`
  (`src/engine/strata/shard.cc:172`). Every catalog open is eager.
- `src/database/shard.cc:516`: `open_payload` (`ShardEndpoint::open_blocking`) calls
  `engine::strata::borrow`, which calls `Core::verify_fields()` (`src/engine/strata/shard.cc:1613`).
- `src/engine/strata/shard.cc:337-342`: `verify_fields()` calls `view.seg()` on every segment and
  runs `dictionary().check_fields(table.size(), table.key_field())`, a pass over the field directory
  and every pooled term. `seg()` maps a lazy record, so even the hostless path (`open_local`,
  `acquire` at `:482`, `open_index` at `:292`, not `defer_io`) maps every segment at open.
- Lazy segments therefore never apply in Xapiand, on either path.

## Method

- Harness: `bench/eager_open/eager_open.cc` on prospect's search layout, the meters of
  `bench/idle_index` (`meters.hh`, `heap_count.hh`). `ab.sh` builds it twice from one source:
  `base` over strata main (strata:efa5214b) and prospect main (prospect:0448b87), `proto` over this
  branch and prospect's `explore/xapiand/eager-open`; `aa` is a copy of `base` for the A/A.
- Fixture: one index of 1,000 Xapiand-shaped documents (14 fields: two text fields with
  positions, keywords, six flag fields, the key; two value slots; 200 stored bytes), committed in
  1, 4 or 10 segments (`d1000s1`, `d1000s4`, `d1000s10`; 345 to 441 KB on disk), cloned 1,000 or
  10,000 times with APFS `clonefile`. `d1000s4c` drops the manifest's last field, so one segment
  names a field id outside the table (`test/index_test.cc`'s manifest reseal).
- Arms, opened on one thread, each with a `PosixIO`, the shared `byte_budget` and
  `reader_registry`, and a reader handle (the catalog's shape), `auto_merge` off so the fixture
  keeps its segments:
  - `today`: `defer_io` (so eager), then Xapiand's `verify_fields` loop, verbatim.
  - `eager`: `defer_io`, no check of the host's.
  - `lazy`: no `defer_io`, `eager_segments` off, no check of the host's.
  - `warm`: `today`, then `demote()` (where the catalog's sweep leaves an idle index after 30 s,
    `catalog.hh:376`).
- After the opens, a key get and a term query (`body:w3`, every segment) per index: the first maps
  a lazy index. Meters per index are process deltas over the open count: dirty footprint
  (`phys_footprint`), resident, file-backed resident, mapped virtual bytes, descriptors, mappings,
  open and first-query latency (p50, p99), and open CPU (user + system). The counting build gives
  live heap.
- Host: the development Mac (x86_64, macOS 15, APFS, Apple clang, Release), strict jobs beside
  other agents' light work, page cache warm. Each row interleaves the sides and arms run by run;
  a cell is the median of its runs with the [min-max] range.

## Results

### Memory and descriptors per open index (exact: the A/A agrees to 0.1 KB)

`d1000s4`, 10,000 indexes (261121-110628-8967e0); the 1,000-index rows match to 1 KB.

| arm (base) | dirty KB | heap KB | resident KB | file-backed KB | mapped KB | maps | fds | dirty after 1st query KB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| today (Xapiand) | 30.6 | 28.5 | 102.6 | 72.0 | 499 | 4.01 | 2 | 32.3 |
| eager, no check | 28.9 | 26.9 | 68.9 | 40.0 | 504 | 4.01 | 2 | 30.6 |
| lazy, no check | 11.0 | 10.8 | 11.0 | 0.0 | 41 | 0.00 | 2 | 30.7 |
| warm (today, demoted) | 11.9 | 10.7 | 12.2 | 0.3 | 138 | 0.03 | 1 | 31.5 |

By segment count (1,000 indexes, 261121-105439-df8150, 261121-105434-cad36c, 261121-105441-6d8f26;
heap from 261121-105445-20137d):

| fixture | today dirty KB | lazy dirty KB | today heap KB | lazy heap KB | saved per segment |
|---|---:|---:|---:|---:|---:|
| d1000s1 | 15.4 | 9.4 | - | - | 6.0 KB dirty |
| d1000s4 | 31.1 | 11.7 | 28.5 | 10.8 | 4.9 KB dirty, 4.4 KB heap |
| d1000s10 | 62.7 | 15.7 | 56.5 | 14.9 | 4.7 KB dirty, 4.2 KB heap |

- An eager segment costs about 4.5 KB of heap (its readers, the copied term index), one mapping
  and its touched pages; a lazy one costs its record only. Lazy saves 62% of the dirty memory at
  4 segments (30.6 to 11.0 KB) and 75% at 10 (62.7 to 15.7 KB).
- The first query maps every segment (a term query reads each), and every arm converges to the
  same 31 to 33 KB (d1000s4). Lazy defers the cost; it does not remove it for an index that is read.
- `verify_fields` itself costs 1.6 KB of heap per index at 4 segments and 4.0 KB at 10 (the pool
  dictionaries it opens stay cached, as a query's would) and 32 KB of file-backed resident pages
  at 4 segments (80 KB at 10): it reads the pooled terms a query may never touch.

### Latency and CPU per open (noisy: the A/A spread is up to 42%)

The A/A job (261121-110630-c0b41f, identical binaries) moves open p50 by 5 to 42% between `base`
and `aa` (eager 472 against 671 µs at 1,000 indexes) and p99 by orders of magnitude when a sync
stalls; only a difference beyond that band counts.

| fixture, count | today open p50 / CPU µs | lazy open p50 / CPU µs | today 1st query p50 µs | lazy 1st query p50 µs | job |
|---|---|---|---|---|---|
| d1000s1, 1k | 464 / 451 | 287 / 273 | 146 | 254 | 261121-105439-df8150 |
| d1000s4, 1k | 877 / 868 | 464 / 451 | 162 | 428 | 261121-105434-cad36c |
| d1000s4, 10k | 905 / 964 | 479 / 475 | 175 | 480 | 261121-110628-8967e0 |
| d1000s10, 1k | 2,056 / 1,974 | 745 / 742 | 179 | 1,015 | 261121-105441-6d8f26 |

- Lazy opens 1.6 to 2.8 times faster (beyond the A/A band from 4 segments), and its first query
  is 1.7 to 5.7 times slower: open plus first query is within noise of eager's (d1000s4, 10k:
  905 + 175 = 1,080 against 479 + 480 = 959 µs; d1000s10: 2,235 against 1,760 µs, the lazy arm
  without any field check).
- The field check is the measurable part of an eager open: `today` against `eager` is 72 µs at
  one segment, 126 µs at 4 and 640 µs at 10 (CPU 60, 160 and 527 µs), 30 to 64 µs per segment,
  22 to 31% of the open at 4 and 10 segments. It is the pass over every pooled term.

### Experiment 4: the pool check in two seeks (pending)

prospect:5ac2314 checks the pool with two seeks while the table has at most 240 fields: the pool
is in key order and a field id's bytes sort as the id (D10), so a key past the table (malformed
leads included) sorts after every valid one and the key field's keys sort together. Larger tables
keep the pass. A local identity smoke (100 indexes of `d1000s4` and `d1000s4c`, every arm) gave
the same hashes and the same refusals. Its rows, `base` against `fast` (this branch over
prospect:5ac2314), are queued: build 261121-111130-dda9d6, `d1000s4` 261121-111132-b1a619,
`d1000s10` 261121-111134-f179e5, identity 261121-111136-e15a28.

## What the lazy saving is worth in Xapiand

Xapiand's catalog opens an index on a request (`DatabasePool::pin` to `catalog.pin_request`,
`src/database/pool.cc:846`) and the request reads it at once; under `defer_io` a read of an
unmapped index goes through the catalog's wake, which runs `promote()` and maps every segment
(`catalog.hh:1870-1886`). So an open that defers its maps pays them on the same request, and the
catalog's sweep already demotes an index idle for 30 s (`demote_idle_ns`, `catalog.hh:376`) to the
`warm` row above. Lazy opens would change only the 30 s window after an open that no read
follows, which Xapiand's request path does not produce.

For 1M open indexes on a host (d1000s4, macOS, the meters above):

| state | dirty memory | page cache held | mappings | descriptors |
|---|---:|---:|---:|---:|
| hot (open, or read in the last 30 s) | 30.6 GB | 72 GB | 4.0M | 2M |
| warm (idle 30 s, demoted) | 11.9 GB | 0.3 GB | 0.03M | 1M |
| lazy open, never read | 11.0 GB | 0 | 0 | 2M |

Lazy and warm differ by 0.9 KB per index (0.9 GB per 1M) and one descriptor. The steady state of
a many-indexes host is set by its hot set (`soft_hot`, `demote_idle`), not by how an open maps:
each index in the hot set costs 30.6 KB plus 72 KB of page cache, each warm one 11.9 KB.

## The prototype: the field check at each map

Moving the check from Xapiand's open into strata's map keeps its guarantee wherever a segment
maps, and is what any lazy path needs first, since a lazy open never runs Xapiand's loop.

- strata (this branch): `vouch` (the manifest's claim a mapped file must meet, `index.hh`)
  carries the manifest's field-table size and key field (`vouch_of`), and `open_body_of` calls
  `basic_segment::check_fields(size, key)`, which runs each plugin reader's `check_fields` when it
  has one. Every map runs it: an eager open, a lazy first read, a merge's inputs, a wake. A segment
  this process wrote keeps the default vouch, which bounds no field.
- prospect (`explore/xapiand/eager-open`, prospect:3f88c3c): `inverted_plugin::reader::check_fields`
  forwards to the dictionary's.
- Xapiand would delete `verify_fields` from `borrow`, `acquire` and `open_index`
  (`src/engine/strata/shard.cc:1613`, `:482`, `:292`) and keep it in `check()`.

Identity (261121-110632-e10bb1, 1,000 indexes of each fixture, every arm, both sides):

- Results: every arm on both sides gives the same hash of the key get, the term query and the
  index digest on each good fixture (`d1000s1` 0e0fdda1ad7eb910, `d1000s4` f21c5648f9f055c0,
  `d1000s10` da0735ac41071fd0).
- Refusals on `d1000s4c`: `base` refuses at open only where Xapiand's loop runs (`today`, `warm`,
  "field id outside the field table"); without it, `eager` and `lazy` open and query the corrupt
  index (hash 878cc613f60d2790), which is the guarantee resting on Xapiand's loop alone. `proto`
  refuses every eager arm at open with the same message and no host check, and `lazy` at its first
  read with `corrupt_segment`, the record marked failed (D9, D11): the refusal moves to where the
  segment maps. Its message is wrapped twice ("strata: corrupt segment: strata: corrupt segment:
  ..."), as for any first-map refusal on main (`open_segment::fail` wraps `what()` again).
- Suites at the change: strata's index, warm, reader_check, background_merge and key_index pass
  (261121-111138-374fa5). prospect's fielddict, fields, index, lifecycle, merge, multisegment,
  background_merge and reclaim suites over this branch are queued (261121-111140-64ce5a).

### The record's size

The first A3 rows at strata:55a5ea73 failed on memory: `proto eager` against `today`'s A/A band,
resident 106,152 B over a top of 106,148 at `d1000s4` (261124-122702-df0b09), and dirty 64,455 B
over 64,164 and resident 228,647 over 228,356 at `d1000s10` (261124-122705-fcbe11). The counting
build (`eager_open_count`, 200 clones, macOS x86_64) names the cause: the same allocations, 16
more bytes per segment, on every arm, lazy included, so in the record, not the check.

| Heap after open, B/index | main `today` | 55a5ea73 `eager` | fixed `eager` | main `lazy` | 55a5ea73 `lazy` | fixed `lazy` |
|---|---|---|---|---|---|---|
| `d1000s4` (4 segments) | 28,489 | 28,553 (+64) | 28,489 (0) | 10,761 | 10,825 (+64) | 10,761 (0) |
| `d1000s10` (10 segments) | 56,497 | 56,657 (+160) | 56,497 (0) | 14,921 | 15,081 (+160) | 14,921 (0) |

`vouch` grew from 16 to 32 bytes (the field-table size and the optional key field), and
`open_segment` with it from 200 to 216 bytes (224 in malloc's 16-byte classes, against 208). The
record padded `version` and its three flags into three words; they now share one, and the record
is 200 bytes again with the full claim. The check keeps nothing after the open: the fixed `eager`
equals main's `today` (Xapiand's loop) byte for byte, after the open and after the first query.

Beyond the band, the check (Xapiand's loop on main, the map's here) keeps prospect's pool reader:
`fielddict::reader::check_fields` scans the pool through `pool()`, which opens it into the reader's
cache (a map node and a `termdict::reader`), 400 B in 2 allocations per segment, kept while the
index is open. A query does not open it (`eager` on main, no check, holds 26,889 and 52,497 B).
Scanning through a reader of its own, dropped at return, gives the same refusals (`d1000s4c`) and
the same identity hash, and takes `eager` and `today` to 26,889 B (`d1000s4`, -1,600) and 52,497 B
(`d1000s10`, -4,000): 7% of a hot index's heap. It is prospect's change, not this quest's.

## Recommendation

1. Drop lazy segments for Xapiand's open as a memory or latency quest. On the request path the
   maps move into the first read of the same request (open plus first query within noise), and
   an idle index already reaches the warm row (11.9 KB, 1 descriptor) 30 s after its last use;
   lazy would save 0.9 KB more and only for opens no read follows. `defer_io` forcing eager stays.
2. Take a strata quest, field check at map: the vouch change on this branch, prospect's forwarding
   `check_fields`, and Xapiand deleting its three open-time `verify_fields` calls. Same refusals
   at open for every eager open, no memory or latency change within the A/A band, and the check
   then also covers merge inputs and wakes, which Xapiand's loop never reached (strata-fields SF9).
   Its acceptance: a prospect test that reseals a manifest with a field dropped and expects the
   refusal at open (eager), at first read (lazy) and at a merge's input map.
3. The one-thread win on the request path is the check itself: 30 to 64 µs per segment per
   open, 22 to 31% of an eager open at 4 and 10 segments. The two-seek pool check (experiment 4)
   targets it; its rows decide whether it joins the quest.
4. For 1M indexes per host the lever is the hot set, not the open: 30.6 KB plus 72 KB of page cache
   and 4 mappings per hot index against 11.9 KB per warm one; the warm 10.7 KB of heap (the kv
   layout's is 5 KB, `catalog-tiers.md`) is the next thing to take apart.
