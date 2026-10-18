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

## M2: many indexes writing at once

Aggregate throughput and CPU per document with N indexes writing at once, one thread each,
for N = 8 and N = 1,000 small indexes on 8 cores, against the fastest baseline given the
same cores.

Each fix after these (lazy open, idle close, shared machinery, a small-segment format profile)
is written here before it runs; format changes go to format 13's designer as notes.
