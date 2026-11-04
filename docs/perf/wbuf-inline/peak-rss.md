# wbuf-inline: why peak resident bytes rose in flush_cadence

> Status: DONE (2026-11-04), performance engineer, branch perf/wbuf-inline at strata:7791520.

The RSS rise is freed memory the macOS allocator keeps resident, not working memory: the physical
footprint peak overlaps main's in every row, and `MallocSpaceEfficient=1` removes the whole rise.
Beneath it the branch's heap peak is 0.7-5.4 MiB higher than main's, and the budget misses
1.2-2.8 MiB of strata's heap at the peak where main over-charged. Two small changes bring that to
0.2-0.35 MiB at every size measured, on macOS and Linux, keeping the per-record saving.

## Question

`flush_cadence` (document layout, 16 B keys, 400,000 key-ordered writes, one flush thread, a
16 GiB budget so no pressure, `buffer_share` 8 or 32 MiB): peak RSS at 500 B values went from
74-80 MiB on main to 95-110 MiB on the branch, with merges on or off; the kv driver's peak is
unchanged. The branch charges a record its stride, so a flush holds 1.20x (500 B) to 1.71x (100 B)
more records.

Hypothesis H1: bigger flushes need bigger flush working memory (sort, segment writer, key filter,
term dictionary), and the peak is that memory.

## Method

The probe (`flush_cadence.cc`, built by `build_cadence.sh` against either tree) reports exact peaks:
`ru_maxrss`; the physical footprint peak (`task_vm_info.ledger_phys_footprint_peak`: dirty and
compressed pages, what macOS charges a process); the live heap peak (a global operator new/delete
counter); and `byte_budget::peak()` (what the buffer and the flush charge). `CADENCE_SITES=1`
snapshots the live allocations of 4 KiB or more at the heap peak by call site; `CADENCE_VMMAP=file`
writes `vmmap -summary` as the run ends. Heap, budget and record counts are deterministic (5 trials
identical); RSS and footprint vary, so 5 interleaved trials of main (A), main again (A/A) and the
branch (B) under `quiet strict wbufrss` (`peak-rss-ab.sh`, raw `peak-rss-ab-mac.txt`, table by
`peak-rss-summarize.py`). macOS 15 x86_64, -O2 -g (-O3 for the timings). Index digests identical in
every arm.

## Attribution (macOS, MiB, ranges over 5 trials)

| Value, share, merges | Arm | Records/flush | Max RSS | Footprint peak | Heap peak | Budget peak | Heap - budget |
|---|---|---:|---:|---:|---:|---:|---:|
| 100 B, 8, on | A | 32,264 | 72.8-90.3 | 17.8-24.8 | 17.7 | 15.8 | 1.92 |
| | A/A | 32,264 | 71.8-85.0 | 17.8-20.7 | 17.7 | 15.8 | 1.92 |
| | B | 55,189 | 66.3-79.5 | 21.3-23.2 | 18.4 | 16.1 | 2.25 |
| 500 B, 8, on | A | 12,711 | 86.8-93.1 | 21.5-25.7 | 11.5 | 10.8 | 0.62 |
| | A/A | 12,711 | 81.5-88.4 | 19.4-25.5 | 11.5 | 10.8 | 0.62 |
| | B | 15,197 | 127.8-144.8 | 17.1-22.9 | 15.0 | 11.1 | 3.84 |
| | A, `MallocSpaceEfficient=1` | 12,711 | 57.3-60.7 | 13.7 | 11.5 | 10.8 | 0.62 |
| | B, `MallocSpaceEfficient=1` | 15,197 | 56.9-57.0 | 15.3 | 15.0 | 11.1 | 3.84 |
| 500 B, 8, off | A | 12,711 | 55.5-62.9 | 18.3-22.1 | 11.5 | 10.8 | 0.69 |
| | A/A | 12,711 | 60.1-64.9 | 16.0-20.5 | 11.5 | 10.8 | 0.69 |
| | B | 15,197 | 70.5-106.4 | 19.3-24.1 | 15.0 | 11.1 | 3.89 |
| 500 B, 32, on | A | 50,841 | 90.8-106.7 | 46.1-50.9 | 38.0 | 39.9 | -1.81 |
| | A/A | 50,841 | 83.9-93.2 | 45.3-48.9 | 38.0 | 39.9 | -1.81 |
| | B | 60,788 | 168.2-189.1 | 47.3-49.5 | 43.4 | 41.0 | 2.43 |
| | A, `MallocSpaceEfficient=1` | 50,841 | 51.6-55.5 | 39.5 | 38.0 | 39.9 | -1.81 |
| | B, `MallocSpaceEfficient=1` | 60,788 | 54.7-55.0 | 43.1-43.2 | 43.4 | 41.0 | 2.43 |

What the rows show:

1. **The RSS rise is clean, freed pages.** RSS rose 40-80 MiB while the footprint peak overlaps
   main's in every row. The `vmmap` at the end of a 500 B, 8 MiB run without merges
   (`peak-rss-vmmap.txt`) puts it in the medium malloc zone: resident 8.6 + 20.9 MiB (in use +
   empty) on main against 23.7 + 59.2 MiB on the branch, dirty 7.8 against 7.5 MiB. Those are freed
   lane chunks (1 KiB doubling to 1 MiB, so the 128 KiB-1 MiB ones land in the medium zone) that
   libmalloc marks reusable and leaves resident: counted in RSS, not in the footprint, and taken back
   by the kernel under pressure. On main the 500 B stored values were separate tiny-zone blocks; the
   branch moved them into the lane's chunks. `MallocSpaceEfficient=1` returns freed pages at once,
   and the branch's RSS falls to main's (56.9-57.0 against 57.3-60.7 MiB; 54.7-55.0 against
   51.6-55.5). `rss-transient.md` found the same mechanism for the flush and merge buffers.
2. **H1 is removed as the cause of the RSS rise, and holds only for a small part of the heap.** The
   heap peak rose 3.5 MiB (500 B, 8 MiB), 5.4 MiB (500 B, 32 MiB) and 0.7 MiB (100 B); under
   `MallocSpaceEfficient=1` the footprint follows it (+1.6 and +3.6 MiB). At the peak
   (`peak-rss-sites.txt`, 500 B, 8 MiB) the frozen buffer is 9.0 MiB of lane chunks on the branch,
   against 1.0 MiB of chunks and 6.4 MiB of small allocations (the documents) on main: the branch's
   buffer really holds more records within the same nominal share, because main over-charged them.
   The flush's own structures (sort vector, writer, docids, key builder) grow with the 1.20x records,
   about 0.5 MiB together: that is H1, and it is small. The rest is the file sink's gather string
   caught in a regrow, 3.0 MiB in the branch's run against 1.5 MiB in main's.
3. **The budget no longer bounds the heap as tightly.** Of heap minus budget, 1.06 MiB is the
   probe's own value generator (the `main` site), so strata's uncharged heap at the peak went from
   -2.9 to 0.9 MiB on main (over-charged: `doc_bytes` adds 96 B a document on top of the maps it
   also charges) to 1.2-2.8 MiB on the branch. Two pieces:
   1. The lane's open chunk. The document layouts' estimate sums strides, but the lane holds its
      chunks: at 32 MiB, 33.0 MiB allocated against 32.0 MiB charged. Design D3 says chunks are
      "charged to the node budget as they are taken", and the inverting layouts charge
      `lane.bytes()`; the document layouts do not. Up to one 1 MiB chunk, or half the lane while it
      is under 2 MiB. It is per buffered index, not per flush.
   2. `file_sink`'s gather string, which is not charged and regrows by doubling: up to a 2 MiB
      buffer plus the 1 MiB it copies from. It predates the branch; main's over-charge hid it.

## The fix measured

Two changes, as patches beside this note, prototyped on a copy of the branch's headers
(`.scratch/fix-src`, `.scratch/fix2-src`), not committed as code:

1. `charge-lane.patch`: the buffer's charge is the lane's chunks plus what whole records hold beyond
   the lane, for every layout. `buffered_bytes()` adds `b.lane.bytes()` for the document layouts as
   it does for the inverting ones; `b.bytes` keeps only `doc_bytes - stride` for whole records, so an
   inline record costs its share of the lane and nothing more; `holds_rejected()` also looks at the
   lane, since an inline-only rejected buffer now leaves `b.bytes` at 0. Three lines and two comments.
2. `file-sink-reserve.patch`: `file_sink` reserves its 1 MiB gather once. An append never passes it
   (a write that would is drained first or passed through), so it never regrows. One line.

Same probe, 5 interleaved trials for the first (`peak-rss-fix.sh`, `peak-rss-fix-mac.txt`), one for
both (its columns are deterministic):

| Value, share | Arm | Records/flush | Heap peak | Budget peak | Heap - budget |
|---|---|---:|---:|---:|---:|
| 100 B, 8 | A main | 32,264 | 17.7 | 15.8 | 1.92 |
| | B branch | 55,189 | 18.4 | 16.1 | 2.25 |
| | F charge-lane | 55,173 | 18.4 | 17.1 | 1.25 |
| | F2 both | 55,173 | 18.4 | 17.1 | 1.26 |
| 500 B, 8 | A | 12,711 | 11.5 | 10.8 | 0.62 |
| | B | 15,197 | 15.0 | 11.1 | 3.84 |
| | F | 15,185 | 15.0 | 12.1 | 2.84 |
| | F2 | 15,185 | 13.5 | 12.1 | 1.40 |
| 500 B, 32 | A | 50,841 | 38.0 | 39.9 | -1.81 |
| | B | 60,788 | 43.4 | 41.0 | 2.43 |
| | F | 58,862 | 42.2 | 40.8 | 1.40 |
| | F2 | 58,862 | 42.2 | 40.8 | 1.40 |
| 1024 B (whole), 32 | A | 28,340 | 37.4 | 36.6 | 0.78 |
| | B | 28,340 | 37.4 | 36.6 | 0.78 |
| | F | 27,545 | 37.5 | 36.6 | 0.95 |
| | F2 | 27,545 | 36.5 | 36.6 | -0.03 |

With both, heap minus budget is 1.26-1.40 MiB at every inline size, 1.06 MiB of it the probe's
generator: strata's uncharged heap at the peak is 0.2-0.35 MiB, a constant, not a share of the
buffer. The per-record saving stays: 500 B flushes hold 15,185 records against main's 12,711 at
8 MiB (1.19x) and 58,862 against 50,841 at 32 MiB (1.16x, from 1.20x: the 1 MiB chunk is 3% of
32 MiB); 100 B stays at 1.71x. Whole 1024 B records flush 2.8% sooner (27,545 against 28,340
records), their open chunk charged too. Digests identical in every arm. The reserve also lowers the
heap peak of a flush by 1.0-1.5 MiB, on main as well.

## The other axes

1. **Time** (`peak-rss-time.sh`, -O3, wall seconds of the whole probe, median [min, max] of 5
   interleaved trials, `peak-rss-time-mac.txt`): the fix ties the branch: 100 B, 8 MiB 1.21
   [1.20, 1.46] against 1.23 [1.18, 1.55]; 500 B, 8 MiB 2.10 [2.08, 2.26] against 2.11
   [2.10, 2.34]; 500 B, 32 MiB 1.73 [1.70, 1.83] against 1.75 [1.69, 1.99]. Against main the
   branch is 1.15x faster at 100 B, a tie at 500 B, 8 MiB (main 2.16 [2.09, 2.27]), and 1.11x
   slower at 500 B, 32 MiB (main 1.58 [1.56, 1.64]), merges included; a probe, not a gate row.
2. **Disk**: same digests; segment counts move with the records per flush above.
3. **Throughput**: the kv rows (`ab2-*-report.txt`) predate this note. The fix adds `lane.bytes()`
   (a loop over its 12 chunk blocks) to each charge, as the inverting layouts already pay. Not
   measured on the kv driver.

## Linux

Ubuntu 24.04 in the local QEMU VM (glibc, clang, `tools/vm/aws-linux.sh run`, no AWS;
`peak-rss-linux.sh`, raw `peak-rss-linux.txt`), 5 interleaved trials of main (A), main again (A/A),
the branch (B) and the branch with both patches (F2), under the script's light hold: the heap,
budget and record columns are deterministic and RSS varied by at most 0.4 MiB, so no strict hold
was needed for them. No footprint counter on Linux.

| Value, share | Arm | Records/flush | Max RSS | Heap peak | Budget peak | Heap - budget |
|---|---|---:|---:|---:|---:|---:|
| 100 B, 8 | A / A/A | 30,394 | 38.3-38.7 | 16.4 | 14.9 | 1.45 |
| | B | 55,189 | 27.5-27.7 | 18.4 | 16.1 | 2.27 |
| | F2 | 55,173 | 28.3-28.4 | 18.4 | 17.1 | 1.24 |
| 500 B, 8 | A / A/A | 12,410 | 48.2-48.4 | 12.5 | 10.8 | 1.72 |
| | B | 15,197 | 50.6-50.7 | 14.9 | 11.1 | 3.83 |
| | F2 | 15,185 | 50.8-51.0 | 13.5 | 12.1 | 1.39 |
| 500 B, 32 | A / A/A | 49,637 | 52.3-52.5 | 39.3 | 39.8 | -0.48 |
| | B | 60,788 | 52.1-52.3 | 43.4 | 41.0 | 2.41 |
| | F2 | 58,862 | 50.6-50.7 | 42.3 | 40.8 | 1.42 |
| 1024 B (whole), 32 | A / A/A | 27,963 | 172.5-172.7 | 37.3 | 36.6 | 0.74 |
| | B | 27,963 | 172.1-172.2 | 36.9 | 36.6 | 0.36 |
| | F2 | 27,545 | 170.6-170.9 | 36.6 | 36.6 | 0.00 |

glibc reuses the freed chunks here, so RSS tracks the heap and the rise is not there: the branch
is 2.4 MiB above main at 500 B, 8 MiB (its heap peak's rise), level at 32 MiB, and 11 MiB smaller
at 100 B. The budget gap is the same as on macOS (branch 2.3-3.8 MiB, both patches 1.2-1.4 MiB, of
which the generator is 1.06 MiB), so the uncharged open chunk is the code's, not the allocator's;
F2 is not worse than the branch on RSS anywhere (0.7 MiB above it at 100 B, below elsewhere). Main
holds fewer records per flush here than on macOS (30,394 against 32,264 at 100 B): its
`doc_bytes` charges `sizeof(document)` and the part sizes, which differ between the standard
libraries; the branch's strides do not.

## Verdict

1. The RSS rise is acceptable as it stands: it is macOS-only (Linux shows none), clean pages the
   allocator returns under pressure; the physical footprint does not move, and
   `MallocSpaceEfficient=1` removes it.
2. The budget's bound slipped by up to 4.2 MiB a buffered index (500 B, 32 MiB: from 2.9 MiB
   over-charged to 1.4 MiB under). The lane's open chunk is uncharged against design D3's rule;
   `charge-lane.patch` charges it without giving back the per-record saving. It belongs with the
   branch, with `defer_io_test`'s identity hash and any flush-count checks updated (the 1024 B
   cadence moves 2.8%); not run against the test suite here.
3. `file-sink-reserve.patch` is independent of the branch (main has the regrow too); it can land
   on its own, with a check that the gather's capacity stays at 1 MiB.
4. Scale: the open chunk is per buffered index (up to half a small lane, 1 MiB above 2 MiB), so
   charging it matters most for the many-indexes workload, where millions of small buffers each
   hold one; the sink's buffer is per flush in progress. Neither is a CPU cost worth converting.

## prospect

`prospect/test/index_test.cc:1456` (the 200 KiB budget case, run as `index` and `index_mapped`)
writes 4,000 records and checks `a.segments().size() > 2 && b.segments().size() > 2` at line 1469.
The branch's inverting records hold the key alone (about 40 B against 80 B whole), so 4,000 writes
flush too seldom. The companion change is the one strata's copy got: 16,000 writes with the same
checks, landed with prospect's repin to a strata that has the branch. `charge-lane.patch` leaves
the inverting charge as it is, so it does not move that count.
