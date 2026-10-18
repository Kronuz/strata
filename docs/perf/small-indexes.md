# Many small indexes

Targets the many-index rows of docs/PLAN.md ("The workload is many indexes", `b320132`):
memory, descriptors, mappings, and open time per open index, and aggregate indexing
throughput and CPU per document with many indexes writing at once, against the baselines
given the same cores. The working target from docs/design/small-indexes.md: an idle open
index under 1 KB of memory and no descriptor, opened in under 100 µs, a million per host.
Every experiment also reports query and indexing time where it touches them, and bytes.

## M0: measure first (strata)

Per idle open index, with 10,000 open at once in one process, for indexes of 1, 10, 100,
and 1,000 documents: the time to open each, the resident bytes it adds (anonymous and
file-backed separately), its file descriptors, and its mappings.

Hypotheses, each with what would refute it:

- H1: descriptors are two per open index (the directory `PosixIO` holds and the owner lock),
  whatever its size; refuted by any other count.
- H2: mappings are one per segment, so one per index here (each has one segment).
- H3: resident bytes are dominated by fixed per-index state (the `index` object, its own
  inline executor and group, the manifest, the segment's parsed table of contents and term
  index) rather than by the documents, up to 100 documents; refuted if bytes grow linearly
  from 1 document.
- H4: opening is dominated by system calls (directory scan, manifest and segment reads, an
  `mmap`, `flock`), so it is tens of µs, not ms; refuted above 1 ms per index.

Procedure, on the gate VM (Linux), then macOS: `strata_engine build-small ROOT COUNT DOCS`
writes COUNT keyed indexes of DOCS documents each from the corpus's first lines (cycled),
committed and closed, built on every core; `strata_engine open-many ROOT COUNT` opens them all
in one process, timing each open, and reports `/proc/self/status` (VmRSS, RssAnon, RssFile),
`/proc/self/fd`, and `/proc/self/maps` before and after. The descriptor limit is raised to
the hard limit inside the process, and `vm.max_map_count` only for the run. On the branch at
main `6e9ba58` with perf/inverter at `339acf7`.

### M0 results (2026-10-18, gate VM)

Raw: `~/bench/results/si-m0` on the gate VM (strata at `ad12899`'s index path, built Release
with clang 18; one segment per index; 10,000 indexes open in one process; cold opens after
dropping the page cache, warm right after).

| Documents per index | Segment | Disk per index (files) | Open, cold (mean, p50) | Open, warm (mean, p50) | Resident per index (anon + file) | Descriptors | Mappings |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1,728 B | 2.6 KB (5) | 1,060 µs, 991 | 103 µs, 83 | 11,958 B (7,267 + 4,691) | 2 | 1 |
| 10 | 7,748 B | 12.2 KB (5) | 2,276 µs, 2,127 | 129 µs, 105 | 21,656 B (7,496 + 14,161) | 2 | 1 |
| 100 | 68,028 B | 90.6 KB (5) | 1,410 µs, 1,342 | 122 µs, 107 | 90,025 B (8,629 + 81,397) | 2 | 1 |
| 1,000 | 576,612 B | 685 KB (5) | 3,874 µs, 3,413 | 200 µs, 149 | 122,881 B (13,452 + 109,429) | 2 | 1 |

H1 and H2 hold (two descriptors: the directory `PosixIO` holds and the owner lock; one
mapping, the segment). H3 holds: the anonymous bytes are 7.3 KB at one document and 13.5 KB at
a thousand. H4 holds warm (83 to 149 µs at the median) and fails cold (1 to 3.4 ms: an open
reads five files, a directory scan, and a mapping, each a disk read once the cache is cold).

Heap per idle open index at one document, from an attribution build (operator new and delete
counted by usable size; `-DSTRATA_ENGINE_COUNT_ALLOCS`, 1,000 indexes): the `index` object
itself 1,312 B; `index::open` 5,631 B, of which the segment's parsed state is 1,754 B (3,018 B
at 100 documents: its table of contents, region checksums, the pinned copy of the term index,
the dictionary reader), the index's own inline executor and its group 1,448 B (it has one when
the host passes none), an empty `std::deque` (the reclaim queue) 680 B, the options' copy
248 B, and about 1,500 B for the manifest, the file and manifest maps, the segment's wrapper and
mapping, and the owner lock; the `PosixIO` 56 B.

File-backed bytes are the segment pages an open touches: the footer, the table of contents,
and the sections it verifies whole at open (header, term index, norms, docids, stats,
versions, `pk_filter`), 4.7 KB for a one-document segment and 109 KB, 19%, of a 577 KB one.
They are clean page cache, reclaimable, unlike the anonymous bytes.

Against the working target (under 1 KB, no descriptor, under 100 µs): 7 to 13 times over in
anonymous bytes, 2 descriptors over, and at the open time only when the cache is warm.

## M1: the same for Tantivy and Lucene

The same four sizes and 10,000 open indexes, through a builder and an opener per engine that
create and open many indexes in one process (Tantivy's `Index::open_in_dir` and a reader;
Lucene's `DirectoryReader.open` over an `MMapDirectory`), to find the fastest baseline per
row.

### M1 results (2026-10-18, gate VM)

Raw: `~/bench/results/si-m1` on the gate VM (`open-ENGINE-DOCS-10000-RUN-ROUND.txt`). Fixtures:
10,000 keyed indexes per engine and size, from the same corpus lines (Tantivy 0.26 and Lucene
10.4 through `bench/engines/*/many*`, strata through `build-small`); the 10- and 1,000-document
baselines were not built (each 10,000-index set took 20 to 40 minutes on this host, against
strata's 1.5 to 3.5). strata at F4 (`05ae80d`). Opens warm in five rounds and cold in three
(the page cache dropped before each), the engine order rotated; the researcher's
`research_workloads` held one of the eight cores throughout (as it did for F1 and F2).

| Engine, documents | Open p50, warm (5 rounds) | Open p50, cold (3 rounds) | Anonymous per idle index | File-backed | Descriptors | Mappings | Disk per index (files) |
|---|---|---|---:|---:|---:|---:|---:|
| strata, 1 | 59, 60, 60, 64, 59 µs | 1,063, 1,150, 1,002 µs | 3,242 B | 96 B | 2 | 0 | 2.6 KB (5) |
| Tantivy, 1 | 123, 123, 124, 127, 126 µs | 4,491, 2,078, 1,968 µs | 12,953 B | 24,880 B | 0 | 6 | 3.6 KB (10) |
| Lucene, 1 | 527, 562, 526, 460, 445 µs | 2,365, 2,305, 1,472 µs | 33 to 42 KB (Java heap 15.3 KB) | 4,946 B | 0 | 1 | 4.3 KB (5) |
| strata, 100 | 59, 60, 61, 59, 59 µs | 1,015, 1,024, 998 µs | 3,242 B | 92 B | 2 | 0 | 90.6 KB (5) |
| Tantivy, 100 | 123, 124, 123, 124, 123 µs | 4,428, 4,402, 4,398 µs | 13,029 B | 84,200 B | 0 | 6 | 101.9 KB (10) |
| Lucene, 100 | 554, 444, 525, 432, 406 µs | 1,861, 1,862, 1,886 µs | 35 to 43 KB (Java heap 16.2 KB) | 80,500 B | 0 | 1 | 88.4 KB (5) |

The fastest baseline is Tantivy for warm opens and for memory, Lucene for cold opens and for
mappings, and both for descriptors. Against them: strata's warm open is 2.07x Tantivy's at
the median of the rounds' ratios (1.98 to 2.14 over the five), its cold open 1.5 to 1.9x
Lucene's, it holds a quarter of Tantivy's anonymous bytes and none of its 25 to 84 KB of
segment pages, and no mapping against Lucene's one and Tantivy's six (at six a process
reaches the default `vm.max_map_count` at about 10,900 open Tantivy indexes). strata loses
on descriptors: two against none.

That row compares unlike opens: strata's is an owning, writable open (the owner lock, the
directory handle, the writer's state), and the baselines' are readers, which take no lock and
close each file once it is mapped. A writable Tantivy or Lucene index holds its writer's lock
file open too; M1b measures them with a writer each.

## M2: many indexes writing at once

Aggregate throughput and CPU per document with N indexes writing at once, one thread each,
for N = 8 and N = 1,000 small indexes on 8 cores, against the fastest baseline given the
same cores.

Configuration, the same for every engine: one process per engine runs N builder threads, each
creating one keyed index (upsert by id), adding its documents, and committing once; the
indexes draw on the node budget T4 calibrated (2,056 MB) split evenly, so N = 8 gives each
257 MB and N = 1,000 about 2 MB (strata: one shared `byte_budget` with a share of capacity / N
and merges by its policy; Tantivy: a one-thread writer with that heap, at least its 15 MB
floor; Lucene: that RAM buffer, `SerialMergeScheduler`, and `forceMerge(1)`, which is a no-op
on one segment). Documents are the corpus's first lines, cycled so each index's ids are
distinct, read into memory before the clock's first index starts in every engine; N = 8
indexes of 100,000 documents and N = 1,000 of 100. `bench/many_write.sh` runs five trials
with the engine order rotated and the page cache dropped before each unit; `many_score.py`
reports wall time, CPU over wall, CPU per document, peak RSS, and bytes on disk, and strata's
throughput over each baseline's per trial with its bootstrap interval.

Each fix after these (lazy open, idle close, shared machinery, a small-segment format profile)
is written here before it runs; format changes go to format 13's designer as notes.

## F1: open reads no segment until it is used

Hypothesis: the largest cost an idle open index holds is its segments, read at open: the
mapping, the parsed state (1.8 to 3.0 KB of heap), and the pages the open verifies (4.7 to
109 KB, file-backed). An open that checks each segment against the manifest from its last
48 bytes (existence, size, and the footer, as today's open checks them) and maps, parses,
and verifies it on first use, as `segments()`, a key lookup, a merge, or a commit reaches it,
holds none of that: an idle open index keeps about 5.5 KB of heap, no mapping, and no
segment pages, and opens faster. Neither baseline defers it: Tantivy maps every segment file
at `Index::open`, Lucene opens every segment's readers in `DirectoryReader.open`.

What it changes beyond bytes: a segment whose pinned sections (term index, norms, docids,
stats, versions, `pk_filter`) are damaged now fails at first use as `corrupt_segment`, which
`heal()` handles, instead of failing the open as `corrupt_index`; a missing file, a wrong
size, or a footer that differs from the manifest still fails the open. This is a durability
semantic for the architect (flagged in the ledger), measured here first.

Change (`index.hh`, `format.hh`): `open_segment` builds its `segment` on first use through
`seg()`; recovery creates recovered segments cold, after reading each one's footer through
`format::decode_footer_tail`, the one footer check `decode_footer` now calls; segments a
flush, a merge, or an install writes start loaded; a merge loads its sources on the owner
thread before posting them.

Procedure: unit tests (index, multisegment, lifecycle crash points, faultfs) green; `verify`
and `parity` unchanged on a multi-segment index; then M0's open-many again at 1, 10, 100
documents (10,000 indexes, cold and warm), A against B, and a first query per index timed in
both (the load moves there).

Keep if the heap and file-backed bytes per idle open index fall, warm and cold opens are no
slower, and a first query costs no more than the open it saves; remove otherwise.

Result, warm (2026-10-18, gate VM; A at `1af9172` with the first-use timing of `2d3316a`, B at
`2d3316a` with F1; raw: `~/bench/results/si-f1-warm`, three alternating rounds of 10,000
indexes; each size's first A round ran on a cache cold from the builds, so A's rounds 2 and 3
are its warm ones):

| Documents | Open p50, A / B | Open mean, A / B | Anonymous per idle index, A / B | File-backed, A / B | Mappings, A / B | First use p50, A / B |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 96, 95 / 84, 82, 84 µs | 118, 113 / 111, 99, 99 µs | 7,267 / 5,410 B | 4,702 / 103 B | 1 / 0 | 10, 9 / 10, 10, 10 µs |
| 10 | 138, 124 / 76, 77, 80 µs | 247, 213 / 131, 138, 143 µs | 7,496 / 5,410 B | 14,165 / 103 B | 1 / 0 | 13, 10 / 9, 9, 10 µs |
| 100 | 121, 121 / 92, 92, 92 µs | 159, 167 / 134, 135, 134 µs | 8,629 / 5,410 B | 81,324 / 103 B | 1 / 0 | 26, 27 / 29, 29, 32 µs |

An idle open index now holds 5,410 B of heap at every size (26 to 37% less), no segment page,
and no mapping; opens are 12 to 40% faster at the median. The first use, which reads the
segment, costs at most 3 µs more than A's (the parsing moved there and is cheap; the loop's
own 9 to 27 µs is the search's first step in both). After the first use B holds what A held.
Descriptors are unchanged (2): the directory and the owner lock, which F1 does not touch.

## F2: no merge machinery until the first merge

Hypothesis: of the 5.4 KB an idle open index holds after F1, 2.1 KB is machinery it has not
used: its own inline executor and executor group (1,448 B, made at open when the host passes
no executor) and the reclaim queue's `std::deque` (680 B, which allocates when it is
constructed); making the group and executor at the first merge, and the queue a `std::list`
(which allocates nothing while empty), leaves about 3.3 KB, with no other change.

Procedure: the tests that run merges (background_merge, lifecycle with background merges,
multisegment, index, reclaim) green; open-many at 1, 10, 100 documents, warm, F1 against F2.

Keep if the heap per idle open index falls by about 2 KB and opens are no slower.

Result, cold (2026-10-18, gate VM; raw: `~/bench/results/si-f1-cold`, the page cache dropped
before each run): opens are disk-bound and vary 25 to 60% run to run within one binary; the
median of the three rounds' p50s is A 4,601 against B 4,912 µs at 1 document, 1,301 against
1,362 µs at 10, and 3,070 against 1,321 µs at 100, where B reads a 4 KB footer page instead of
81 KB of pinned sections. No slower cold beyond that noise, faster where segments are larger.
**F1 kept.**

F2's result (2026-10-18, gate VM, warm; A at F1 `2d3316a`, B at `c907d3d`; raw:
`~/bench/results/si-f2-warm`): an idle open index holds 3,242 B of heap at 1, 10, and 100
documents against F1's 5,410 B (2,168 B less, as attributed), with opens unchanged (p50 66 to
70 µs both, on this run's warm cache) and first uses unchanged. **F2 kept.**

What an idle open index holds now, against the working target: 3.2 KB of heap (the `index`
object's 1,312 B, the manifest and the file maps, the segment's cold wrapper, the owner lock,
the options' copy), no mapping, no segment page, 2 descriptors, and a warm open of about 70 µs.

## Notes for format 13's designer (small segments)

From the segments M0 wrote (`strata_engine sections`, the gate VM's `~/si/seg1`, `seg10`,
`seg100`): a one-document segment is 1,728 B, of which the sections hold 938 B and the table of
contents, the footer (48 B), and alignment the other 790 B (46%); at 10 documents 7,748 B
with 6,825 B of sections (12% overhead); at 100, 68,028 B with 67,265 B (1%).

- N5. The `lengths` section packs 128-value blocks at one width, so one document costs a whole
  block (96 B of 97 at a 6-bit width) where its own value needs one byte; a tail block packed
  to its count (or LEB128 below a block) is what a small segment needs.
- N6. `pk_filter` is 125 B for one key: the key range (both ends, about 45 B each) plus a fuse
  filter's minimum; with one or two keys the range alone answers, and with a few the keys
  themselves are smaller than a filter.
- N7. The table of contents is the largest fixed cost of a tiny segment (about 500 B for 12
  entries, each with its offset, length, checksum, and region checksums): sections of one
  region need no region checksum list beyond the section's own, and empty sections (`postings`
  is 0 B at one document) need no entry.
- N8. Each small index is five files (two manifests, the metadata, the owner lock, the
  segment), 2.6 KB on disk at one document; Tantivy's is ten (3.6 KB). Packing small indexes'
  segments together (the note's direction 3) is the format-level answer to files per index.

## F3: idle indexes closed behind handles (a measurement of the trade, not yet a component)

After F2 an open index holds 3.2 KB of heap and two descriptors (its owner lock and its
directory), and neither goes below about 2 KB and one descriptor while the index stays
open: the target (under 1 KB, no descriptor, a million per host) is reachable only for indexes
that are not open. The note's direction 2 is that: an LRU of open indexes behind cheap
handles, a closed index holding only its entry, reopened on its next use (about 70 µs warm
after F1 and F2, 1 to 5 ms cold).

Hypothesis: with accesses skewed the way a social workload's are (Zipf, s = 1 over 10,000
indexes), a cache of 1,000 open indexes serves most accesses without an open, holds a tenth
of the heap, descriptors, and mappings of all 10,000 open, and costs a mean access only the
reopens it misses.

Procedure (`strata_engine cache-sim ROOT N CAPACITY ACCESSES`, a prototype in the engine
only): N indexes of M0's fixtures, ACCESSES accesses drawn Zipf (s = 1, seed fixed), each an
acquire (an open on a miss, the least recently used closed past CAPACITY) and one key lookup
(`find_key`, which reads the segment); per access p50, p99, and mean time, the miss rate, and
the holdings at the end, for CAPACITY = 100, 1,000, and 10,000, warm. The public component
(its handle type, what closing an index with uncommitted work means, threads) is the
architect's to review before it is built in `include/`.

### macOS (the development Mac, APFS, 2026-10-18)

M0's fixtures rebuilt from the same first 100,000 documents (`.scratch/corpus-100k.json`,
sha256 `0a1d5344…`), 10,000 indexes of one document; open-many with A at the base (`1af9172`
with the same probes) and B at F2, three alternating rounds, the host busy with other
agents' work and this item's own fixture builds (F_FULLFSYNC-bound: 34 minutes for 10,000
one-document indexes, almost all of it waiting on syncs):

| | Anonymous per idle index | File-backed | Mappings | Descriptors | Open p50 (three rounds) |
|---|---:|---:|---:|---:|---|
| A, before F1 | 4,915 to 4,925 B | 4,553 B | 1 | 2 | 1,106, 940, 269 µs |
| B, F1 and F2 | 2,814 to 2,891 B | 14 B | 0 | 2 | 1,086, 945, 966 µs |

The heap falls 42% and the segment pages and mappings go, as on Linux; macOS opens are 10
times Linux's and too noisy here to compare (a p99 near 38 ms from file opens contending with
the syncs). FreeBSD is owed.

### F3 on macOS (the same host and fixtures)

`cache-sim` over the 10,000 one-document indexes, 100,000 Zipf accesses:

| Capacity | Misses | Access mean | p50 | p99 | Descriptors held | Mappings |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 (all open) | 8.6% (first touches) | 149 µs | 1.9 µs | 2.1 ms | 17,192 | 8,632 |
| 1,000 | 32.8% | 3,475 µs | 3.0 µs | 37.9 ms | 2,000 | 1,032 |
| 100 | 60.8% | 7,638 µs | 1,272 µs | 45.6 ms | 200 | 130 |

On this host a reopen costs about 10 ms (the miss rate into the mean), so closing idle indexes
trades a tenth of the descriptors and mappings for a mean 23 times longer: the trade depends
on the open's cost, which is the gate VM's next row.

### F3 on the gate VM (2026-10-18; strata at F4 `05ae80d`; raw: `~/bench/results/si-f3`)

The same simulation over the gate VM's 10,000 one-document indexes, warm:

| Capacity | Misses | Access mean | p50 | p99 | Descriptors held | Mappings | Heap held (anonymous) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 10,000 (all open) | 8.6% (first touches) | 23.3 µs | 11 µs | 274 µs | 17,192 | 8,597 | 33.9 MB |
| 1,000 | 32.8% | 49.8 µs | 12 µs | 414 µs | 2,000 | 1,001 | 5.2 MB |
| 100 | 60.8% | 75.7 µs | 85 µs | 486 µs | 200 | 101 | 1.8 MB |

A miss (a reopen and the segment's first load) costs about 110 µs here against macOS's
14 ms, so a cache of a tenth of the indexes costs a mean access 2.1 times the all-open one
(26 µs more) for an eighth of the descriptors and mappings and a sixth of the heap; on macOS
the same cache costs 23 times. Closing idle indexes is cheap where opens are, which makes the
open's own cost (F4, F5) the lever for the macOS side.

## F4: sizes from the directory, not from opening every file

Hypothesis: an open makes about 71 system calls per index on Linux (`strace -c` over
open-many, 1,000 one-document indexes, F2: 11 `openat`, 11 `close`, 28 `fstat`, 10
`geteuid`, 4 `pread64`, 2 `getdents64`, 2 `fcntl`, 1 `flock`), and five of the opens exist
only to learn each file's size for the storage accounting; asking the directory for the size
(`IO::entry_footprint`, an `fstatat` on POSIX, answered by faultfs too) removes those opens,
their closes and checks, and on a cold cache their inode reads, so opens get faster, more so
cold and on macOS, where a file open is about ten times Linux's.

Change (`index.hh`): recovery takes each strata file's size from `entry_footprint`, falling
back to opening it where an IO does not answer; a file that leaves between the scan and the
size is a system error as an open of it would be.

Procedure: the tests (index, lifecycle, media, faultfs, reclaim) green; `strace -c` again;
open-many warm and cold, F2 against F4, on the gate VM and macOS.

Keep if opens are faster with the same bytes and no test changes; remove otherwise.

Result on the gate VM (2026-10-18; A at F2 with open-many's segment count fixed, `5836d34`, B at
F4, `05ae80d`; five alternating rounds of 10,000 indexes after a warm-up pass; raw:
`~/bench/results/si-f4-warm`, scored by `bench/open_score.py`):

| Documents | Open p50 per round, A | B | A over B (p50) | Heap per idle index, A / B | First use p50, A / B |
|---:|---|---|---:|---:|---:|
| 1 | 68, 67, 66, 68, 83 µs | 61, 59, 58, 58, 75 µs | 1.136x [1.107, 1.172] | 3,242 / 3,242 B | 29 / 28 µs |
| 10 | 69, 67, 67, 67, 68 µs | 58, 60, 58, 60, 59 µs | 1.153x [1.117, 1.190] | 3,242 / 3,242 B | 31 / 30 µs |
| 100 | 67, 69, 68, 68, 70 µs | 59, 59, 59, 59, 60 µs | 1.153x [1.136, 1.169] | 3,242 / 3,242 B | 44 / 44 µs |

`strace -c` over 1,000 opens: 71,140 system calls before, 51,140 after (per open: `openat` 11
to 7, `close` 11 to 7, `fstat` 28 to 16, `geteuid` 10 to 6, and 4 `newfstatat`). Opens are 14
to 15% faster with the same bytes and first uses; keep on Linux, macOS next.

The first-use rows here are the first correct ones for a lazy segment: before `5836d34`
open-many counted segments through `segments()`, which loaded them, so F1's and F2's first-use
columns timed the search alone (10 to 27 µs). A lazy segment's first use reads and parses it:
28 µs at one document, 44 at a hundred. Against F1's rule (a first query costs no more than
the open it saves) that is about 18 µs more at first use for an open about 12 µs cheaper at
one document, from runs an hour apart; F5 measures the base, F1, F4, and F5 in one run.

Also seen, for the completion library (Detent's; additive only): `PosixIO` calls `geteuid`
once per file it opens to check ownership (10 per index open); the effective uid could be read
once per `PosixIO`.

## F5: open reads no segment file at all

Hypothesis: after F4 an open still opens each segment's file to check its size and footer
against the manifest (`openat`, three `fstat`, `geteuid`, a 48-byte `pread`, `close`: 7 of
the 51 system calls, and a disk read when the cache is cold), and its first use opens and reads
the same file again. The directory scan F4 added already gives each file's size, so an open
that checks a segment's existence and size against the scan, and leaves the footer and the
document count to the first use (which already checks both, as it reads the file whole),
opens each segment file once instead of twice: opens faster, warm and more so cold and on
macOS, first uses unchanged, and an open plus a first use at most what the base's open was.

What it changes beyond time: a segment whose footer or document count disagrees with the
manifest, at the right size, fails at first use (`corrupt_segment`, which `heal()` reports)
instead of failing the open; F1 already moved its pinned sections' verification there. A
missing segment or one of the wrong size still fails the open. The architect is asked to rule
on both (the ledger's "for the architect").

Change (`index.hh`): recovery passes the scan's sizes to `load_files`, whose cold path then
opens nothing; a backend without directory sizes keeps F1's footer read.

Procedure: the tests (index, lifecycle, media, faultfs, reclaim, multisegment) green, their
open-time expectations moved to first use where the footer is what they damage; `strace -c`;
then open-many warm with first use, five rotated rounds at 1, 10, and 100 documents, over four
binaries in one run: the base (`1af9172` with the probes), F1 (`2d3316a` with `5836d34`'s
count), F4, and F5; cold after.

Keep if F5's opens are faster than F4's with first uses no slower; then also report, per
size, whether F1 and F5 together meet F1's rule (open plus first use no more than the base's).

## M1b: indexes open to write

The baselines' rows above are readers. Procedure: `many open-writers` (Tantivy: a reader and a
one-thread writer at its 15 MB floor; Lucene: a writer with `SerialMergeScheduler` and a
near-real-time reader from it) over the one- and 100-document fixtures, 1,000 indexes, warm,
three rounds; strata's open-many is already this open.

## F1, F2, and F4 on FreeBSD

Each item must not lose on FreeBSD either. Procedure, on the FreeBSD 14.4 VM (UFS, 4 vCPUs,
4 GB): M0's fixtures (10,000 indexes of one and of 100 documents) built once (the items do
not change the format); four binaries, each the branch's `strata_engine` (whose open-many
reads FreeBSD's holdings through `sysctl`: `KERN_PROC_VMMAP` resident pages split by vnode
backing, its entry count, and `KERN_PROC_NFDS`) over the library at the base (`1af9172`), F1
(`2f9be5d`), F2 (`c907d3d`), and F4 (`05ae80d`); open-many warm with first use, five rounds,
the order rotated. Each item is judged against its predecessor by the same rule as on Linux.
