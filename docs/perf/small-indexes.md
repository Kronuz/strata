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
