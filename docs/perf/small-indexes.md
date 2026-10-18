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
