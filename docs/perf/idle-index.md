# What one idle open index costs

Question: what does an open, idle index cost in strata (memory, descriptors, mappings, threads,
wakeups, open time), how far are we from millions of open indexes per host, and what removes the
cost. Landscape proposal D1 (`design/landscape`, `docs/design/product-landscape.md`). Main
`98ce743`, branch `perf/idle-index`. Harness: `bench/idle_index/idle_index.cc` (`idle_index`;
`idle_index_count` adds a counting `operator new` with allocation sites).

## Method

- An index is the document layout on `strata::core` (keys plus stored values; "Q%07d" keys, 100-byte
  values), committed, then closed. `build ROOT COUNT DOCS` makes one real index and clones its files
  (APFS `clonefile`; ext4 copies), `COUNT/1000` per group directory. The files are real index files; the
  clones share one uuid, which `open` does not cross-check.
- `open ROOT COUNT shared query idle SECS` opens them all in one process on one thread, each with a
  `PosixIO`, `byte_budget` and a reader handle on one `reader_registry` (a host's shape; `bare` omits the
  registry and handle), then idles, runs one `get` per index twice (first and second query), and closes.
- Meters (per index = delta over the open count): resident, dirty footprint (macOS `phys_footprint`;
  Linux `Private_Dirty`), anonymous and file-backed resident, descriptors, mapped regions, threads, CPU and
  context switches over the idle window, system-wide wired memory (macOS) or `/proc/meminfo` slab (Linux).
- macOS: the development Mac (x86_64, 16 CPUs, 32 GiB, APFS, Release, LLVM clang with libc++), under
  `quiet exclusive`. Linux: the strata VM (x86_64, 8 vCPUs, 16 GiB, ext4, clang 18 with libstdc++, glibc),
  `QUIET` exclusive on the host; "cold" is the guest page cache dropped (the host still caches the disk).
  One run per cell; the latency columns move by 2x between runs on macOS (other agents' load), so read
  memory columns as exact and latencies as ranges. Counts of 1k, 10k, 100k open; past that see Limits.

## Table: per open idle index (shared-reader host shape)

macOS, one process, dirty = private anonymous footprint (not reclaimable); file-backed is clean page cache
mapped by the segment; "after 1 query" is resident after one `get` per index.

| docs | open indexes | dirty B | resident B | file-backed B | after 1 query: resident B | fds | mappings | open p50 / p99 µs | first query p50 / p99 µs | close p50 µs |
|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---:|
| 0 | 1,000 | 3,936 | 4,096 | 160 | 4,141 | 1 | 0.01 | 556 / 1015 | 0.2 / 0.8 | 4.1 |
| 0 | 10,000 | 3,657 | 3,673 | 16 | 3,704 | 1 | 0.00 | 431 / 848 | 0.1 / 0.6 | 3.2 |
| 0 | 100,000 | 3,594 | 3,595 | 2 | 3,620 | 1 | 0.00 | 415 / 787 | 0.2 / 0.7 | 3.8 |
| 1 | 1,000 | 6,414 | 10,707 | 4,293 | 10,772 | 1 | 1.02 | 934 / 1189 | 1.2 / 2.3 | 12.5 |
| 1 | 10,000 | 5,912 | 10,028 | 4,116 | 10,060 | 1 | 1.00 | 899 / 1378 | 1.0 / 2.0 | 12.5 |
| 1 | 100,000 | 5,875 | 9,973 | 4,098 | 9,998 | 1 | 1.00 | 526 / 932 | 1.0 / 2.1 | 13.4 |
| 10 | 1,000 | 6,291 | 10,584 | 4,293 | 10,641 | 1 | 1.02 | 1009 / 1387 | 1.4 / 1.9 | 12.1 |
| 10 | 10,000 | 5,923 | 10,039 | 4,116 | 10,071 | 1 | 1.00 | 991 / 1393 | 1.0 / 1.6 | 12.9 |
| 10 | 100,000 | 5,875 | 9,973 | 4,098 | 9,998 | 1 | 1.00 | 542 / 1204 | 1.2 / 2.2 | 12.8 |
| 100 | 1,000 | 6,296 | 14,684 | 8,389 | 18,850 | 1 | 1.02 | 557 / 816 | 3.9 / 5.6 | 12.5 |
| 100 | 10,000 | 5,929 | 14,141 | 8,212 | 18,267 | 1 | 1.00 | 503 / 883 | 3.6 / 24.8 | 12.6 |
| 100 | 100,000 | 5,891 | 14,085 | 8,194 | 18,206 | 1 | 1.00 | 515 / 896 | 4.7 / 6.4 | 13.2 |
| 1,000 | 1,000 | 6,689 | 19,173 | 12,485 | 27,435 | 1 | 1.02 | 1177 / 1862 | 6.1 / 8.9 | 13.6 |
| 1,000 | 10,000 | 6,259 | 18,567 | 12,308 | 26,791 | 1 | 1.00 | 1089 / 1644 | 6.2 / 9.4 | 12.7 |
| 1,000 | 100,000 | 6,213 | 18,503 | 12,290 | 26,720 | 1 | 1.00 | 592 / 1227 | 7.3 / 10.2 | 14.0 |
| 1 | 126,507 (the limit) | 5,874 | 9,972 | 4,098 | 10,001 | 1 | 1.00 | 511 / 1104 | 0.9 / 1.8 | 12.9 |

Per-index memory is flat from 1k to 126k open: the 1k rows are 5 to 10% higher only because one-off
process state is divided by fewer indexes. Memory does not depend on document count up to 1,000 (dirty
+0.4 KB); the file-backed bytes do (the segment pages an open and a query touch).

Linux (same harness, libstdc++/glibc; 100k rows for 100 and 1,000 documents were 30k to bound the ext4
copies). Dirty is 8.1 KB per 1-document index, 8.4 KB at 1,000, 5.6 KB at 0 (no segment, no mapping):

| docs | open indexes | dirty B | resident B | file-backed B | mappings | open p50 / p99 µs, warm | open p50 / p99 µs, cold | first query p50 / p99 µs (cold) |
|---:|---:|---:|---:|---:|---:|---|---|---|
| 0 | 100,000 | 5,576 | 5,583 | 7 | 0 | 53 / 232 | 714 / 1838 | 0.3 / 0.7 |
| 1 | 10,000 | 8,089 | 12,251 | 4,162 | 1 | 70 / 260 | 498 / 1019 | 1.3 / 1.7 |
| 1 | 100,000 | 8,088 | 12,191 | 4,103 | 1 | 70 / 281 | 522 / 2124 | 1.3 / 1.8 |
| 1 | 300,000 | 8,088 | 12,186 | 4,098 | 1 | 115 / 1528 | n/a | 1.3 / 3.6 |
| 10 | 100,000 | 8,104 | 12,207 | 4,103 | 1 | 70 / 275 | 582 / 1417 | 1.5 / 2.1 |
| 100 | 30,000 | 8,120 | 20,432 | 12,311 | 1 | 72 / 292 | 496 / 956 | 2.3 / 3.0 |
| 1,000 | 30,000 | 8,440 | 104,187 | 95,747 | 1 | 75 / 295 | 794 / 4008 | 2.8 / 5.3 |

Bare (no reader registry or handle), 10,000 open: macOS dirty 2,146 (0 docs), 4,387 (1), 4,366 (10), 4,388
(100), 4,721 (1,000); Linux 4,073 (0), 6,473 (1), 6,825 (1,000). The registry and the per-index reader handle
cost 1.5 KB on macOS (1.6 KB Linux).

Other meters:

- Threads: 0 per index (1 total) at every count; idle 3 to 8 s with up to 300,000 open: 0 voluntary
  context switches, 1 to 24 involuntary, CPU 0.0000 to 0.0001 s (an earlier idle row of 0.17 s at 100k was
  the harness's own region walk, not the engine; fixed). No timers exist: the index source has no timer or
  thread of its own; a merge runs on the host's executor or the index's inline one.
- Descriptors: 1 per index. The directory descriptor also holds the `flock` owner lock
  (`acquire_directory_owner`), so there is no second one. No descriptor is held for the segment: it is
  mapped, then the descriptor closed.
- Kernel memory held per index: macOS wired +630 to 675 B (1 document), +356 B (0 documents), measured at
  126k and 252k open; Linux slab +64 B (SUnreclaim +59 B) at 300k, which undercounts: the clones' dentries
  and inodes were already cached by the build, and a cold pinned directory costs about a dentry plus an
  inode.
- First query (warm): 1 to 7 µs; close: 3 to 14 µs.

## Open and reopen latency

- Linux warm open: 53 to 105 µs p50, mostly 70 (p99 230 to 330 µs), flat from 1k to 100k open and in document count.
  That is inside the working target of the small-indexes note (under 100 µs). 300k open p50 115 µs, p99
  1.5 ms (the p99 and a 217 ms max at 300k are the kernel growing its tables).
- Linux cold (guest cache dropped): 0.5 ms p50 for 1 to 100 documents, 0.8 to 1.6 ms at 1,000 (the segment
  is read through); p99 1 to 4 ms. The first query after a cold open costs 1.3 to 3 µs because the open
  already read and verified the segment's pages.
- macOS warm open: 0.4 to 1.1 ms p50, 6 to 15 times the Linux figure. The syscall count is the same (see
  below), so it is the price of an APFS/XNU open, flock, directory scan and mmap. macOS cold cannot be forced
  (`purge` needs root); the table is warm.
- An open is about 54 syscalls (`strace -c`, 1,000 bare opens of 1-document indexes, Linux): 20 `fstat`, 8
  `openat`, 8 `close`, 7 `geteuid`, 2 each of `getdents64`, `pread64`, `flock`, `fcntl`, 1 `mmap`, `munmap`,
  `fstatfs`. At 45 to 55 µs each under strace; unstraced the whole open is 70 µs, so the open is
  syscall-bound, and 27 of the 54 are repeat `fstat`/`geteuid` calls.
- Reopen: close then open again is the same cost as the first warm open (the open has no state beyond what
  it reads); closing is 3 to 14 µs. Nothing is cached across a close.

## Where the bytes go

Counting `operator new` with the allocation site of each live allocation, 200 indexes of 1 document, shared
(macOS, libc++; `idle_index_count sites`). Heap 5,507 B in 35 allocations (dirty 5,874: 7% allocator and
page slack). The bare heap is 4,006 B in 27 allocations; 0 documents, bare: 1,870 B in 10; the heap is the
same at 100 documents (5,541 B).

| piece | B | allocations |
|---|---:|---:|
| the `index` object itself (options copy, manifest, writer and maintenance state, buffer, queues) | 1,320 | 1 |
| the open segment: its object 936, decoded table of contents 512, residency spans 114, reader state 150, segment wrapper 80, mapping 48, others 50 | 1,890 | 14 |
| the reader publication (shared only): the `view` 920, publication 208, reader state 136 + 128, segment-view list 48 + 32 + 24 | 1,496 | 8 |
| manifest and file maps: three map nodes 192, segment list 120, name lists 160 | 472 | 6 |
| merge machinery (inline executor and its group, reclaim queue) | 200 | 3 |
| the `PosixIO` and the directory lock | 96 | 2 |

Outside the heap: 1 descriptor, 1 vnode for the directory and 1 for the mapped segment (macOS), 1 mapping
(4.1 KB resident, clean, for a 1-document segment; 8 KB at 100 documents; 12 KB at 1,000 on macOS, 96 KB on
Linux where fault-around maps neighbouring pages), and the kernel's table entries (above). The reader
registry is per host (a fixed array), not per index. There is no per-index cache, no thread and no timer.

The write buffer is empty after a commit and costs nothing idle; an uncommitted index adds its memtab rows
(`docs/status/write-buffer-reads.md`: 2.65 KB empty, 5 to 6 KB at 1 to 10 buffered documents on macOS),
which this table does not include because the harness's indexes are committed.

## Limits: what bites first, and what strata does

| limit | where | measured | strata today |
|---|---|---|---|
| vnode table (macOS `kern.maxvnodes` = 263,168 on this 32 GiB Mac; root-only to raise) | each open index pins its directory vnode and, when it has a segment, the mapped file's vnode | `Too many open files in system` (ENFILE) at **126,507** open 1-document indexes and at **252,492** open 0-document indexes (2 and 1 pinned vnodes each, plus about 10k used by the system) | nothing: no handle cache; fails at open with the raw errno |
| `vm.max_map_count` (Linux stock 65,530; Elasticsearch asks 262,144; this VM was 1,048,576) | one mapping per index that has a segment | at the stock value the open failed at **65,491** indexes, reported as `Cannot allocate memory` from the directory scan. At 1,048,576, 300,000 opened. A 0-document index has no mapping | nothing: the mapping lives as long as the index is open |
| descriptors: 1 per index; macOS `kern.maxfilesperproc` 1,048,576 (root), Linux `fs.nr_open`/`ulimit -n` 1,048,576 on this VM (default hard limit differs by distro) | the directory descriptor that holds the lock | not reached (the vnode table is hit 8x earlier on macOS); 300,000 held on Linux; 1,048,575 is the ceiling on both | `RLIMIT_NOFILE` is the host's to raise |
| address space | segment mapping rounds to a page; vsize 12 to 120 KB per index (document count) | 14 KB per 1-document index (macOS); 1M is about 14 GB of a 47-bit space | not a limit |
| kernel per-mapping cost | one `vm_map_entry` (macOS) or VMA (Linux) | macOS wired +630 B at 126k; Linux slab +64 B (undercounted, see above) | not a limit below the counts above |
| user memory | dirty 5.9 KB (macOS) or 8.1 KB (Linux) per index | 1M would need 5.9 GB or 8.1 GB dirty plus 4.1 GB of clean pages that the OS can drop. Extrapolated, not run: macOS cannot open 1M (vnode table), and the Linux VM has 16 GiB | host's budget |

What would stop a million on each platform, in order:

1. macOS: vnodes, at 126k to 252k. Not tunable without root. Strata cannot hold more than that open at
   all; a closed-handle tier is the only answer.
2. Linux stock: `max_map_count`, at 65k, until the host raises the sysctl; then descriptors, at 1,048,576,
   until the host raises `fs.nr_open`; then memory (8 GB per million).
3. Per-index costs decide the density after that; a closed index costs nothing but its directory entry.

The three standing answers to these (none built): lazy segment open, so a never-queried index maps
nothing; a closed-handle LRU (small-indexes F3: parked by the lead as host policy, measured at 2.1x the
all-open mean access on Linux and 23x on macOS for a cache of a tenth); and a shared owner lock per
partition rather than per index (so no per-index descriptor or vnode). Only the first is a memory cut;
the other two remove the descriptor and vnode ceilings.

## Top three cuts

Estimates are heap bytes from the table above (macOS libc++); Linux has the same structure at 1.4 times
the bytes. "Shared" is the host shape.

1. A cold segment holds nothing until first used (small-indexes F1, F5, F6 re-done on main; they were
   measured on the pre-split base and never landed). Removes the open segment 1,890 B (34% of the shared
   5,507 B), the 4.1 KB of file-backed pages, the mapping (so the 65k Linux limit no longer applies to idle
   indexes and the macOS vnode ceiling doubles to about 250k), and an `mmap` plus five syscalls from the
   open. The cost moves to the first use: 33 to 50 µs on the old base. Estimate: dirty 5.9 KB to 3.9 KB
   (shared, macOS), 8.1 KB to about 5.5 KB (Linux). The reader's view must hold a cold segment as a
   reference.
2. Publish the reader view at the first acquire, not at `reader()`. The 1,496 B (27%) of the shared host
   shape is an eagerly built `view` and its state for an index no reader has touched; an idle index would
   hold the 136 B reader state. The first acquire builds the view (a few µs). Estimate: dirty 5.9 KB to
   4.4 KB, and cumulative with cut 1: 2.3 KB.
3. An idle core for the `index` object: 1,320 B, of which about 650 B is writer and maintenance state an
   idle index never uses (the small-indexes F7 analysis on the old base; this needs recounting on main's
   write-buffer fields), plus the merge machinery 200 B (F2: none until the first merge). Estimate −0.85
   KB (15% of the shared heap). With cuts 1 to 3: shared dirty about 1.4 KB on macOS (heap about 1.3 KB),
   about 2 KB on Linux; the bare floor is today's 0-document index minus the same pieces.

Descriptor, vnode and mapping ceilings are separate from these: they need the closed-handle tier or the
shared lock, not smaller objects.

Worth in money: at 1M open indexes a host holds 8.1 GB dirty (Linux); the three cuts save about 6 GB, or
roughly $24 a month per million indexes at an assumed $4 per GiB-month of cloud RAM (the price is an
assumption, not a project figure). The point is density: a 64 GiB host holds about 8M such indexes today
and about 30M after, if the descriptor and mapping ceilings are lifted.

Prototype: none. All three change structures under the write lease and the shared-reader publication that
the open code reviews cover (`index.hh` 4,568 lines; cut 1 is the largest change in the small-indexes
branch), so none is a cheap prototype, and an A/B of a half-built version would not say what it would
land at. Cut 1 is the one to build.

## Against the field

The field's figures are the landscape note's. Ours are one-document or empty indexes in the document
layout on `strata::core` (keys plus stored values). **They do not include prospect's text and vector
sections, so they are not yet hybrid-index figures**: a search layout adds term and posting sections per
segment, and the per-index cost with them is not measured here (Next).

| system (cited ceiling) | strata now (measured) | after the three cuts |
|---|---|---|
| Elasticsearch, about 1,000 shards per node | **clearly ahead**: 65,491 open on stock Linux, 300,000 on a raised sysctl, 126,507 on this Mac; 65 to 300 times the count, at 6 to 8 KB each | ahead, same ceilings; about 2 KB each |
| Weaviate, 18 to 19k tenants per node | **ahead**: 3.4 times on stock Linux (65k), 6.7 times on the Mac (126k), 16 times with the sysctl raised (300k). Their tenants hold vector indexes and ours do not, so the ratio is an upper bound on a hybrid comparison | ahead; a million per host reachable on Linux with the sysctls raised (extrapolated: 2 GB at 2 KB each), not on macOS without the handle tier |
| Turbopuffer, 250M+ namespaces (hosted, closed) | **not comparable and behind**: that is a hosted fleet total, mostly cold on object storage, not open per host. Strata has no cold-handle or object-storage tier, and the largest we opened per host is 300,000 | still behind: 250M needs the closed-handle tier and a remote tier; the cuts do not change that |

Honest summary: on open-and-resident indexes per node we are clearly ahead of Elasticsearch and ahead of
Weaviate today, with margins that depend on the platform's mapping and vnode ceilings more than on our
bytes; "millions per host" is not demonstrated (300k on Linux, 126k on macOS, the rest extrapolated), and
the hybrid per-index cost and the cold tier are the two things the positioning rests on that are still
unmeasured.

## Axes

- Time: open 53 to 85 µs warm on Linux (0.5 ms cold), 0.4 to 1.1 ms on macOS; first query 1 to 7 µs;
  close 3 to 14 µs.
- Memory: 5.9 KB (macOS) or 8.1 KB (Linux) dirty per 1-document index in the shared shape; 2.1 and 4.1 KB
  bare 0-document; plus 4.1 KB or more of reclaimable file-backed pages.
- Disk: a 1-document index is 3 files and 738 bytes (2 files, 78 bytes at 0 documents; 108 KB at 1,000);
  with filesystem blocks that is 12 to 16 KB each on APFS/ext4. Cloning 1.1M of them took 198 s and 1.2 GB
  and 4.4M inodes of metadata on APFS (the data blocks are shared).
- Indexing/throughput: not touched; nothing was written but the fixture.

## Next

1. Build cut 1 (cold segments) on main with its A/B; then cuts 2 and 3.
2. Measure the per-index cost with prospect's search layout (text and vector sections): the hybrid claim.
3. Decide the closed-handle tier or the shared partition lock; it alone lifts the vnode/descriptor ceilings.
4. Run 1M on Linux with `vm.max_map_count` and `fs.nr_open` raised, on a VM or host with 32 GiB.
5. Cold macOS opens need `purge` (root); the table is warm.
