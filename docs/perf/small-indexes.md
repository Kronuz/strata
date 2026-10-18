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
the hard limit inside the process; `vm.max_map_count` (1,048,576 on the gate VM) needed no
change. On the branch at main `6e9ba58` with perf/inverter at `339acf7`.

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
distinct, read into memory first in every engine (inside the timed process); N = 8 indexes
of 100,000 documents and N = 1,000 of 100. `bench/many_write.sh` runs five trials
with the engine order rotated and the page cache dropped before each unit; `many_score.py`
reports wall time, CPU over wall, CPU per document, peak RSS, and bytes on disk, and strata's
throughput over each baseline's per trial with its bootstrap interval.

### M2 results, N = 8 indexes of 100,000 documents (2026-10-18, gate VM)

Raw: `~/bench/results/si-m2-8-100000` (GNU `time -v` per unit). strata at F5 (`e3e5f2e`, with
perf/inverter at `339acf7`); Tantivy 0.26 and Lucene 10.4 through the many tools; corpus
`corpus-1000000.json`'s first 200,000 lines (the even indexes hold the first 100,000, the odd
ones the next); five trials, the
order rotated, the page cache dropped before each unit; the researcher's workload held one of
the eight cores. A dry run (one trial of 8 x 20,000 and of 100 x 100) came first.

| Engine | Wall per trial (s) | Median | CPU / wall | CPU per document | Peak RSS | On disk |
|---|---|---:|---:|---:|---:|---:|
| strata | 7.7, 7.3, 7.2, 7.2, 7.1 | 7.2 s | 614% | 55.7 µs | 3,010 MiB | 366.0 MiB |
| Tantivy | 11.1, 10.2, 10.0, 10.1, 10.2 | 10.2 s | 599% | 75.5 µs | 2,442 MiB | 402.7 MiB |
| Lucene | 26.0, 25.5, 25.2, 26.1, 25.1 | 25.5 s | 603% | 192.6 µs | 2,334 MiB | 357.0 MiB |

strata's aggregate throughput is 1.404x Tantivy's [1.383, 1.440] (the fastest baseline) and
3.495x Lucene's [3.358, 3.626], at 26% less CPU per document than Tantivy. It loses on
memory: its peak RSS is 23% above Tantivy's and 29% above Lucene's on the same 2,056 MB node
budget (each process also holds the 351 MiB of corpus lines): less those lines, strata peaks
about 700 MiB over the budget's 1,961 MiB, Tantivy 130 MiB over, and Lucene within it; that
is the inverter's and the budget's accounting (perf/inverter, D17), noted for them. On disk it is 9% smaller than
Tantivy and 2.5% larger than Lucene. In CPU, the 19.8 µs per document it saves over Tantivy
is 5.5 CPU-hours per billion documents indexed: real but small next to what an open index
holds (M1, M1b), which is what bounds how many indexes a host can serve.

### M2 results, N = 1,000 indexes of 100 documents (2026-10-18, gate VM)

Raw: `~/bench/results/si-m2-1000-100`; the same binaries, corpus, and procedure, 1,000 builder
threads at once in each engine, each index created, filled, and committed once; budget 2,056
MB over 1,000 (Tantivy at its 15 MB floor per writer).

| Engine | Wall per trial (s) | Median | CPU / wall | CPU per document | Peak RSS | On disk |
|---|---|---:|---:|---:|---:|---:|
| strata | 4.1, 4.7, 4.4, 4.5, 4.5 | 4.5 s | 323% | 143.7 µs | 705 MiB | 78.9 MiB |
| Tantivy | 23.1, 21.0, 20.0, 20.8, 19.6 | 20.8 s | 264% | 535.4 µs | 3,063 MiB | 89.3 MiB |
| Lucene | 52.1, 42.9, 43.6, 47.8, 51.2 | 47.8 s | 313% | 1,577.4 µs | 1,624 MiB | 77.3 MiB |

strata's aggregate throughput is 4.519x Tantivy's [4.322, 5.625] and 10.558x Lucene's [9.078,
12.681], at 27% of Tantivy's CPU per document and 23% of its peak RSS; on disk 12% smaller
than Tantivy and 2% larger than Lucene. Against the design target (4.5x the fastest baseline)
the median meets it and the interval's lower end does not. None of the engines uses the
cores here: strata's 323% is half system time (7.0 of 14.4 CPU-seconds), the cost of creating
and syncing a thousand indexes' files, which is where the next profile goes. In CPU the 392
µs per document saved over Tantivy is 109 CPU-hours per billion small-index documents.

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
and no mapping; opens are 12 to 40% faster at the median. (Corrected under F5: B's first-use
column above was timed after open-many's segment count had loaded the segments; measured
right, F1's first use is 33 to 50 µs, so alone it makes an open plus its first use 8 to 10%
slower than the base, failing this rule's last clause until F5.) After the first use B holds
what A held. Descriptors are unchanged (2): the directory and the owner lock, which F1 does
not touch.

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
the syncs); F5's four-way run below measures macOS on a quieter host, and FreeBSD's is at the
end of this note.

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
to 15% faster with the same bytes and first uses; keep on Linux. On macOS (F5's four-way run
below, where F2 and F4 are one step and F2 changes no open): opens 1.28 to 1.29 times faster,
an open plus its first use 1.22 to 1.24 times; on FreeBSD (the end of this note) 1.11 to
1.12 times and 1.05 to 1.07. **F4 kept on Linux, macOS, and FreeBSD.**

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

Change (`index.hh`): recovery passes the scan's sizes (from the directory, or from opening
the file where an IO cannot answer, as F4 left it) to `load_files`, whose cold path then opens
nothing; F1's footer read at open goes.

Procedure: the tests (index, lifecycle, media, faultfs, reclaim, multisegment) green, their
open-time expectations moved to first use where the footer is what they damage; `strace -c`;
then open-many warm with first use, five rotated rounds at 1, 10, and 100 documents, over four
binaries in one run: the base (`1af9172` with the probes), F1 (`2d3316a` with `5836d34`'s
count), F4, and F5; cold after.

Keep if F5's opens are faster than F4's with first uses no slower; then also report, per
size, whether F1 and F5 together meet F1's rule (open plus first use no more than the base's).

Result on the gate VM (2026-10-18; raw: `~/bench/results/si-f5-warm2`, and the same run with
a base that times no first use in `si-f5-warm`; a = the base `1af9172` with the first-use
probe, b = F1 `2d3316a` with `5836d34`'s count, c = F4 `05ae80d`, d = F5 `e3e5f2e`; five
rotated rounds of 10,000 indexes after a warm-up that touches every segment; index-size's
two-thread A/B and the researcher's workload held three of the eight cores throughout):

| Documents | Open p50 per round, a / b / c / d (µs) | First use p50, a / b / c / d | Heap per idle index, a / b / c / d |
|---:|---|---:|---:|
| 1 | 92-98 / 75-84 / 65-69 / 63-65 | 10 / 33 / 32 / 33 µs | 7,267 / 5,410 / 3,242 / 3,242 B |
| 10 | 90-99 / 79-81 / 67-69 / 59-63 | 10 / 35 / 35 / 34 µs | 7,496 / 5,410 / 3,242 / 3,242 B |
| 100 | 101-111 / 73-80 / 63-68 / 57-62 | 10 / 50 / 49 / 48 µs | 8,629 / 5,410 / 3,242 / 3,242 B |

Each step's time over the next one's (median of the rounds' ratios, 95% bootstrap interval;
above 1 is faster), for the open alone and for an open plus its first use:

| Documents | Step | Open p50 | Open plus first use |
|---:|---|---:|---:|
| 1 | F1 (a over b) | 1.134x [1.119, 1.227] | **0.896x** [0.880, 0.962] |
| 1 | F2 and F4 (b over c) | 1.224x [1.119, 1.292] | 1.158x [1.071, 1.216] |
| 1 | F5 (c over d) | 1.047x [1.000, 1.063] | 1.021x [0.990, 1.073] |
| 1 | all (a over d) | 1.446x [1.431, 1.508] | 1.073x [1.052, 1.096] |
| 10 | F1 | 1.212x [1.139, 1.241] | **0.922x** [0.877, 0.964] |
| 10 | F2 and F4 | 1.179x [1.145, 1.194] | 1.118x [1.086, 1.126] |
| 10 | F5 | 1.079x [1.063, 1.136] | 1.062x [1.062, 1.097] |
| 10 | all | 1.540x [1.429, 1.678] | 1.103x [1.020, 1.172] |
| 100 | F1 | 1.387x [1.363, 1.486] | **0.924x** [0.901, 0.968] |
| 100 | F2 and F4 | 1.176x [1.088, 1.231] | 1.104x [1.060, 1.142] |
| 100 | F5 | 1.105x [1.048, 1.172] | 1.047x [1.037, 1.093] |
| 100 | all | 1.790x [1.726, 1.879] | 1.103x [1.028, 1.121] |

`strace -c`: 51,140 system calls per 1,000 opens at F4, 44,135 at F5 (`openat` 7 to 6,
`fstat` 16 to 13, `geteuid` 6 to 5, `pread64` 4 to 3, `close` 7 to 6). The first run
(`si-f5-warm`) agrees: F5's open 1.070x, 1.049x, and 1.119x at 1, 10, and 100 documents.
**F5 kept on Linux**: opens 5 to 11% faster than F4's, first uses unchanged.

Cold (2026-10-18, gate VM; raw: `~/bench/results/si-cold`; the page cache dropped before every
run, three rotated rounds of 10,000 opens; a = F4, b = F5, c = F7, run after F7 landed):

| Documents | Open p50 per round, F4 / F5 / F7 (µs) | F5 over F4 | F6 and F7 over F5 | F7 over F4 |
|---:|---|---:|---:|---:|
| 1 | 1,616, 1,195, 1,173 / 838, 834, 831 / 662, 644, 644 | 1.433x [1.411, 1.928] | 1.290x [1.266, 1.295] | 1.855x [1.821, 2.441] |
| 100 | 1,020, 1,009, 1,008 / 830, 823, 834 / 645, 643, 648 | 1.226x [1.209, 1.229] | 1.287x [1.280, 1.287] | 1.569x [1.556, 1.581] |

Cold, each file an open no longer opens is a disk read it no longer waits for: F5 opens 1.23
to 1.43 times faster than F4 and F7 1.29 times faster again; a cold open now takes about
645 µs, against Lucene's 1,472 to 2,365 µs and Tantivy's 1,968 to 4,491 µs in M1.

On macOS (2026-10-18, the development Mac, APFS; the same four binaries built with Apple
clang, the base and F1 with the Mach counters; `.scratch/f5-mac` in the worktree; 10,000
indexes of the macOS fixtures, five rotated rounds after a warm-up that touches every segment):

| Documents | Open p50 per round, a / b / c / d (µs) | First use p50, a / b / c / d | Heap per idle index, a / b / c / d |
|---:|---|---:|---:|
| 1 | 288-306 / 252-283 / 194-224 / 164-191 | 0.2 / 42 / 43 / 45 µs | 4,898 / 3,161 / 2,920 / 2,920 B |
| 100 | 271-279 / 236-244 / 184-193 / 156-164 | 0.1 / 50 / 51 / 55 µs | 6,454 / 3,156 / 2,917 / 2,918 B |

| Documents | Step | Open p50 | Open plus first use |
|---:|---|---:|---:|
| 1 | F1 | 1.078x [1.018, 1.215] | **0.938x** [0.889, 1.052] |
| 1 | F2 and F4 | 1.290x [1.256, 1.421] | 1.241x [1.202, 1.345] |
| 1 | F5 | 1.161x [1.025, 1.199] | 1.112x [1.012, 1.163] |
| 1 | all | 1.624x [1.589, 1.815] | 1.306x [1.266, 1.449] |
| 100 | F1 | 1.143x [1.123, 1.176] | **0.946x** [0.932, 0.972] |
| 100 | F2 and F4 | 1.281x [1.258, 1.322] | 1.217x [1.200, 1.255] |
| 100 | F5 | 1.181x [1.128, 1.209] | 1.111x [1.086, 1.130] |
| 100 | all | 1.718x [1.676, 1.789] | 1.278x [1.264, 1.319] |

F4 and F5 save more here, as their hypotheses expected, since a file open costs more on
macOS. F5 is a win that loses a little elsewhere: its first use is 2 to 4 µs slower than F4's
(medians 43.1 against 44.8 µs at one document, within the rounds' spread; 50.7 against 55.0
µs at a hundred, in every round), so the rule's "first uses no slower" fails on macOS at a
hundred documents. The first use runs the same code in both; what differs is that F4's open
had already opened the segment file in this process, so its first use found the kernel's
vnode and name cache entries active. That cost moved, it was not added: F5's open is 23 to
29 µs faster and an open plus its first use 11% faster at both sizes. **F5 kept on macOS**,
the clause's miss stated; the lead may rule otherwise. F1 alone is 5 to 6% slower on an open
plus its first use here too, and the four together 1.28 to 1.31 times faster than the base.

A correction to F1: its first use is 33 to 50 µs, not the 10 to 27 µs its table shows (those
loaded the segment before the clock; see F4's result), so taken alone F1 makes an open plus
its first use 8 to 10% slower than the base, and fails the third clause of its own rule; the
memory and open-time clauses hold. What made it fail is the second read of each segment
file (the footer at open, the whole file at first use), which F5 removes. F1 to F5 together
meet F1's rule at every size (open plus first use 7 to 10% faster than the base) while an idle
open index holds 3.2 KB of heap instead of 7.3 to 8.6 KB, no mapping, and no segment page,
and opens 1.45 to 1.79 times faster. F1 stays as the base F5 needs; the lead is told.

## F6: a cold segment holds no inline parsed state

Hypothesis: each open segment keeps its parsed form in a `std::optional<segment>`, whose
storage is inline whether or not the segment was ever read: 800 B with libc++ (`sizeof`), in
a 3.2 KB idle open index. Held behind a pointer allocated on first use, a cold segment keeps
8 B instead, so an idle open index of one segment holds about 0.8 KB less (a quarter), and
one of several segments that much per segment; the first use pays one allocation, well under
a microsecond.

Change (`index.hh`): `open_segment`'s parsed state is a `std::unique_ptr<const segment>`.

Procedure: the tests green; open-many warm with first use, F5 against F6, five rotated rounds
of 10,000 indexes at 1, 10, and 100 documents on the gate VM and macOS.

Keep if the heap per idle open index falls by about the segment's size with opens and first
uses no slower; remove otherwise.

## F7: open reads no retained manifest when nothing can be obsolete

Hypothesis: after F5 an open still reads each retained predecessor manifest (7 of its 44
system calls with one predecessor) and caches its file list and the head's (about 350 B of
heap), only so retention can tell which files no kept manifest names. When every file the
scan found is a kept manifest or named by the head, nothing can be obsolete whatever the
predecessors name, so retention has nothing to do and needs neither list; the next retention
that can find something (after a commit or with an orphan on disk) reads them as today. The
open then makes 37 system calls, holds about 350 B less, and opens about 5% faster.

What it changes: a kept predecessor that cannot be read is reported (`retention_blocked()`)
by the first retention that has something to remove, not by an open that has nothing to.

Change (`index.hh`): `retain()` returns before reading any list in that case, so an open
caches neither the predecessors' lists nor the head's.

Procedure and rule: as F6's, F6 against F7, plus `strace -c`.

### F6 and F7 on the gate VM (2026-10-18)

Raw: `~/bench/results/si-f67-warm2` (a = F5 `e3e5f2e`, b = F6 `24eecc4`, c = F7 `6429e00`;
five rotated rounds of 10,000 indexes after a warm-up that touches every segment). A first
run (`si-f67-warm`) agrees except that its first round of a, at every size, ran while the host
was still loaded from before the hold (p50 273 to 537 µs) and was rerun.

| Documents | Open p50 per round, a / b / c (µs) | First use p50 per round, a / b / c (µs) | Heap per idle index, a / b / c |
|---:|---|---|---:|
| 1 | 56-62 / 55-63 / 49-58 | 29-34 / 30-34 / 30-33 | 3,242 / 2,443 / 2,060 B |
| 10 | 56-59 / 55-59 / 50-54 | 31-32 / 32-39 / 32-35 | 3,242 / 2,443 / 2,060 B |
| 100 | 56-62 / 55-63 / 49-56 | 43-47 / 45-51 / 45-49 | 3,242 / 2,443 / 2,060 B |

| Documents | Step | Open p50 | Open plus first use |
|---:|---|---:|---:|
| 1 | F6 (a over b) | 0.984x [0.903, 1.127] | 0.978x [0.944, 1.093] |
| 1 | F7 (b over c) | 1.122x [1.078, 1.240] | 1.089x [1.062, 1.150] |
| 10 | F6 | 1.036x [0.966, 1.073] | 1.000x [0.957, 1.023] |
| 10 | F7 | 1.078x [1.019, 1.157] | 1.056x [1.035, 1.108] |
| 100 | F6 | 1.016x [0.952, 1.036] | 0.991x [0.912, 1.020] |
| 100 | F7 | 1.118x [1.058, 1.286] | 1.058x [0.990, 1.213] |

F6 takes 799 B off every idle open index (25%) at no measurable cost in time: opens and opens
plus first uses within the noise, first uses 0 to 2 µs higher at the medians (the first run:
1 µs in every round at 1 and 10 documents), which is the 800 B allocation moving from the open
to the first use. **F6 kept on Linux**, that last microsecond stated against its rule's
"first uses no slower". F7 opens 8 to 12% faster and holds 383 B less (16%) with first uses
unchanged; `strace -c` counts 37,118 system calls per 1,000 opens (44,135 at F5). **F7 kept on
Linux.** After F1 to F7 an idle open index holds 2,060 B of heap on Linux, no mapping, and no
segment page, and opens in 49 to 58 µs warm.

On macOS (`.scratch/f67-mac`, the same three binaries built with Apple clang; 1 and 100
documents; the first run after the warm-up pass, a's first round, was again slow, 385 to 449
µs, and is left out of the medians below):

| Documents | Open p50, a / b / c (rounds 2-5, µs) | First use p50 median, a / b / c | Heap per idle index, a / b / c |
|---:|---|---:|---:|
| 1 | 152-163 / 152-160 / 126-132 | 40.0 / 41.4 / 40.2 µs | 2,917 / 2,090 / 1,821 B |
| 100 | 152-157 / 150-153 / 126-129 | 53.4 / 53.5 / 53.2 µs | 2,912 / 2,090 / 1,822 B |

F6: 827 B less per idle open index (28%), opens unchanged, first uses within 1.4 µs. F7: opens
1.199x [1.196, 1.212] and 1.205x [1.166, 1.211] faster, an open plus its first use 1.15x and
1.14x, 269 B less, first uses unchanged. **F6 and F7 kept on macOS.** An idle open index on
macOS now holds 1.8 KB of heap and opens in 126 to 132 µs warm, against the base's 4.9 KB and
288 to 306 µs.

## M1b: indexes open to write

The baselines' rows above are readers. Procedure: `many open-writers` (Tantivy: a reader and a
one-thread writer at its 15 MB floor; Lucene: a writer with `SerialMergeScheduler` and a
near-real-time reader from it) over the one- and 100-document fixtures, 1,000 indexes, warm,
three rounds; strata's open-many is already this open.

Result on the gate VM (2026-10-18; raw: `~/bench/results/si-m1b`; strata at F5; the same
host load as F5's run):

| Engine, documents | Open p50 (3 rounds) | Anonymous per open index | File-backed | Descriptors | Mappings |
|---|---|---:|---:|---:|---:|
| strata, 1 | 56, 58, 58 µs | 3,256 B | 0.9 KB | 2 | 0 |
| Tantivy, 1 | 5,032, 2,962, 2,899 µs | 2,619 KB | 26 KB | 1 | 31 |
| Lucene, 1 | 4,457, 3,163, 3,014 µs | 158 to 165 KB (Java heap 28.5 KB) | 5.7 KB | 1 | 1 |
| strata, 100 | 59, 62, 58 µs | 3,256 B | 1.0 KB | 2 | 0 |
| Tantivy, 100 | 7,578, 2,969, 2,892 µs | 2,619 KB | 81 KB | 1 | 31 |
| Lucene, 100 | 5,395, 3,483, 3,218 µs | 163 to 166 KB (Java heap 29.5 KB) | 76 KB | 1 | 1 |

Open to write, the fastest baseline (Tantivy's open, Lucene's memory) opens 50 times slower
than strata and holds 49 times its anonymous bytes; Tantivy's writer holds 2.6 MB and 31
mappings (its threads' stacks and arenas), so a host reaches the default `vm.max_map_count`
at about 2,100 writable Tantivy indexes. strata still holds one descriptor more than either:
the directory handle its I/O keeps, beside the owner lock all three hold.

## F1 to F7 on FreeBSD

Each item must not lose on FreeBSD either. Procedure, on the FreeBSD 14.4 VM (UFS, 4 vCPUs,
4 GB): M0's fixtures (10,000 indexes of one and of 100 documents) built once (the items do
not change the format); four binaries, each the branch's `strata_engine` (whose open-many
reads FreeBSD's holdings through `sysctl`: `KERN_PROC_VMMAP` resident pages split by vnode
backing, its entry count, and `KERN_PROC_NFDS`) over the library at the base (`1af9172`), F1
(`2f9be5d`), F2 (`c907d3d`), and F4 (`05ae80d`); open-many warm with first use, five rounds,
the order rotated. Each item is judged against its predecessor by the same rule as on Linux.

Result (2026-10-18; the FreeBSD VM idle otherwise; the libraries at the base, F1 (`2d3316a`),
F4, F5, F6, and F7 under one `strata_engine` built with FreeBSD's clang; two warm-up passes per
size; raw: `/root/si/res-open` on the VM, `.scratch/bsd-open` in the worktree):

| Documents | Open p50 per round, base / F1 / F4 / F5 / F6 / F7 (µs) | First use p50 | Heap per idle index |
|---:|---|---|---|
| 1 | 69-72 / 48-51 / 43-46 / 40-42 / 39-47 / 36-40 | 9 / 32 / 32 / 33 / 33 / 33 µs | 5,185 / 3,200 / 2,963 / 2,963 / 2,098 / 1,819 B |
| 100 | 79-84 / 48-49 / 43-45 / 40-43 / 39-40 / 36-40 | 9 / 42 / 42 / 43 / 43 / 42 µs | 6,726 / 3,208 / 2,972 / 2,971 / 2,105 / 1,827 B |

| Documents | Step | Open p50 | Open plus first use |
|---:|---|---:|---:|
| 1 | F1 | 1.408x [1.380, 1.440] | **0.964x** [0.963, 0.988] |
| 1 | F2 and F4 | 1.116x [1.065, 1.163] | 1.068x [1.025, 1.095] |
| 1 | F5 | 1.071x [1.049, 1.122] | 1.028x [1.026, 1.053] |
| 1 | F6 | 1.024x [0.872, 1.051] | 1.000x [0.951, 1.041] |
| 1 | F7 | 1.108x [1.000, 1.175] | 1.043x [1.000, 1.110] |
| 1 | all | 1.865x [1.725, 1.919] | 1.127x [1.068, 1.147] |
| 100 | F1 | 1.667x [1.612, 1.714] | 0.989x [0.967, 1.022] |
| 100 | F2 and F4 | 1.114x [1.089, 1.140] | 1.047x [1.046, 1.070] |
| 100 | F5 | 1.075x [1.023, 1.125] | 1.036x [1.000, 1.049] |
| 100 | F6 | 1.026x [1.000, 1.103] | 1.012x [1.000, 1.049] |
| 100 | F7 | 1.083x [0.975, 1.111] | 1.038x [1.000, 1.064] |
| 100 | all | 2.194x [2.000, 2.333] | 1.128x [1.085, 1.177] |

Every item holds on FreeBSD as on Linux and macOS: F1 alone again makes an open plus its first
use slower (3.6% at one document, within the noise at a hundred), and the stack is 1.13 times
faster than the base on it, with opens 1.87 to 2.19 times faster; F5's first use is 1 µs above
F4's at the medians, F6's within the noise. An idle open index on FreeBSD holds 1.8 KB of
heap (the base's 5.2 to 6.7 KB), no mapping, no segment page, and 2 descriptors (the six-way
run's counter read none, fixed in `4a41ed5`; F7 rerun with it counts 2).

### Where the rest of an idle open index goes (after F7)

Of the 2,060 B on Linux (1.8 KB with libc++), the `index` object itself is 1,312 B (1,096 B
with libc++): the options' copy 232 B, the manifest 216 B, and about 650 B of writer and
maintenance state an idle index never uses (the buffer, the version maps, the undo log, the
reclaim and pending queues, counters). Outside it: the file-size map (one node per file, about
400 B for five files), the manifest's segment list, the cold segment (about 150 B), and the
owner lock. Under 1 KB therefore needs the object split into an idle core and state allocated
at the first write or merge, which crosses the writer paths perf/inverter is changing, or
closing idle indexes behind handles (F3): both are design steps, not one more lazy field.
