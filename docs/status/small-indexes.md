# small-indexes

- Design: docs/design/small-indexes.md (a note, not yet a design); experiments in
  docs/perf/small-indexes.md, each written before it runs.
- Branch / worktree: perf/small-indexes at /Users/kronuz/code/Kronuz/strata-wt/small-indexes,
  from origin/main at 6e9ba58, with perf/inverter merged at 339acf7 (ad12899).
- State: implementing; the measure-first rows and F1 to F7 are done on Linux, macOS, and
  FreeBSD; the next costs are design questions for the architect (below).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Worktree and branch; perf/inverter merged for its index path (ad12899).
  - M0 to M2 written before they run (ca4e8b6); M2's configuration (8467c75): every engine's
    indexes share T4's 2,056 MB node budget, an N-th each.
  - M0's tools (strata_engine build-small, open-many; the attribution build) and M1's
    (bench/engines/tantivy-0.26-st/src/bin/many.rs, lucene-10.4.0-st/src/main/java/Many.java),
    with writer opens for M1b and FreeBSD holdings through sysctl (7f7f791).
  - M0 on the gate VM, 10,000 indexes open at once: 7.3 to 13.5 KB anonymous and 4.7 to 109 KB
    file-backed per idle open index, 2 descriptors, 1 mapping, open 83 to 149 us warm (p50) and
    1 to 3.4 ms cold; heap attributed.
  - F1 (2f9be5d): open reads no segment until it is used. Heap per idle open index 7.3-8.6 KB
    to 5.4 KB, file-backed 4.7-81 KB to 0.1 KB, one mapping to none, opens 13 to 39% faster.
    Corrected (6dae422 and this step): its first use is 33 to 50 us, not the 10 to 27 us first
    recorded (open-many's count had loaded the segments), so F1 alone makes an open plus its
    first use 8 to 10% slower than the base and fails its own rule's last clause; F1 to F5
    together meet it (7 to 10% faster than the base). Kept as the base F5 needs.
  - F2 (c907d3d): no merge machinery until the first merge: 5.4 KB to 3.2 KB per idle open
    index, opens unchanged. Kept.
  - M1 on the gate VM (6dae422): strata's warm open 2.07x Tantivy's, cold 1.5-1.9x Lucene's;
    a quarter of Tantivy's heap and none of its 25-84 KB of segment pages; no mapping against
    1 (Lucene) and 6 (Tantivy); loses on descriptors, 2 against none (the baselines measured
    as readers).
  - M1b, writable opens of 1,000 indexes: Tantivy 2.9-7.6 ms, 2.6 MB, 1 descriptor, 31
    mappings; Lucene 3.0-5.4 ms, 158-166 KB, 1 descriptor, 1 mapping; strata (F5) 56-62 us,
    3.3 KB, 2 descriptors, no mapping.
  - F3's trade measured on macOS and Linux (cache-sim): a cache of a tenth of the indexes
    costs 2.1x the all-open mean access on Linux and 23x on macOS, for an eighth of the
    descriptors and mappings.
  - F4 (05ae80d): sizes from the directory: opens 13.6 to 15.3% faster on Linux at the same
    bytes and first uses, 71 to 51 system calls per open. Kept on Linux.
  - F5 (e3e5f2e): an open opens no segment file: opens 5 to 11% faster than F4's on Linux,
    first uses unchanged, 51 to 44 system calls per open; the full suite green on macOS
    (34 tests). Kept on Linux.
  - F4 and F5 on macOS (a3e185d): F4 1.28-1.29x faster opens, F5 another 1.16-1.18x; F5's
    first use 2-4 us slower there (stated; the rule's clause misses at 100 documents), its
    open plus first use 11% faster. Kept on macOS.
  - M2 on the gate VM (6429e00's note): 8 x 100k at once, strata 1.40x Tantivy and 3.50x
    Lucene, 26% less CPU per document than Tantivy, peak RSS 23% above Tantivy's (over the
    node budget by about 700 MiB, Tantivy by 130: noted for perf/inverter and D17), disk 9% under
    Tantivy and 2.5% over Lucene; 1,000 x 100 at once, 4.52x Tantivy [4.32, 5.63] and 10.56x
    Lucene, 23% of Tantivy's peak RSS; strata half system time there (10 fsyncs per index
    created and committed once).
  - F6 (24eecc4): a cold segment's parsed state behind a pointer: 799 B less per idle open
    index on Linux (25%), 827 B on macOS, time unchanged within the noise (its first use up
    to 2 us, the allocation moved from the open; stated against its rule). Kept.
  - F7 (6429e00): open reads no retained manifest when nothing can be obsolete: opens 8-12%
    faster on Linux and 1.20x on macOS, 383 B less, 37 system calls per open. Kept.
  - F1 to F7 on FreeBSD (094d59f): every item holds; the stack opens 1.87-2.19x faster than
    the base with an open plus its first use 1.13x faster, 1.8 KB of heap per idle open
    index against 5.2-6.7 KB. FreeBSD's descriptor counter fixed (4a41ed5).
  - Where an idle open index stands after F7: 2,060 B of heap on Linux, 1.8 KB on macOS and
    FreeBSD (base 7.3-8.6 KB on Linux), no mapping, no segment page, 2 descriptors, warm
    open 49-58 us on Linux (base 92-111), 126-132 us on macOS (base 288-306).
  - M2's creation cost profiled: strata 70 system calls and 10 fsyncs per index created and
    committed once, Tantivy 590 and 19, Lucene 1,129 and 6 (strata's extra syncs are its
    directory syncs between steps).
  - Notes N5 to N8 for format 13's designer (small segments) in the perf note.
- Doing: nothing running; CI on 094d59f (run 39127562866).
- Next (each a design step, for the architect through the lead, with the numbers in the
  perf note):
  1. Split the index object into an idle core and state made at the first write or merge
     (about 650 B of the 1,312 B object an idle index never uses); it crosses the writer
     paths perf/inverter is changing, so it needs that branch's agreement first.
  2. Idle close behind handles (F3 as a component: its handle type, uncommitted work,
     threads): on Linux a cache of a tenth of the indexes costs 2.1x the all-open mean
     access, on macOS 23x.
  3. The second descriptor: the directory handle the completion library's PosixIO keeps
     (path-based I/O, or the owner lock taken on that handle); each changes a safety or
     compatibility property.
  4. Fewer syncs per small index (group commit across indexes, the note's direction 5, or
     create's syncs folded into the first commit's): durability ordering, D3's.
  5. Merge perf/inverter (at E33, a2dfbbd; one conflict, in remove_copy, which takes
     theirs) once its CI is green, then rerun M2 on it: M2 sits on 339acf7, and the
     inverter has since reported 3.34x Tantivy at one thread.
- Blocked on: the architect's rulings for every next step above (design questions, not this
  branch's to decide). Also for the architect (via the lead): F1's and F5's moved checks: a
  segment whose pinned sections (F1), footer, or document count (F5) disagree with the
  manifest at the right size now fails at first use as corrupt_segment, which heal()
  reports, not at open; a missing segment, one of another size, or disagreeing live docs
  still fail the open. F7's: an unreadable kept predecessor is reported by the first
  retention with something to remove, not by an open with nothing to remove.
- For the lead: F1 was kept on a first-use measurement that open-many's segment count had
  invalidated; corrected, F1 alone fails its rule's last clause (an open plus its first use
  8-10% slower on Linux, 5-6% on macOS, up to 3.6% on FreeBSD) and is kept as the base F5
  needs, since F1 to F7 together are 7-13% faster than the base on it on every platform.
  F5 (macOS) and F6 miss their "first uses no slower" clauses by 1 to 4 us, where the cost
  moved from the open to the first use rather than being added; both stated, both kept, and
  the lead may rule otherwise.
- Process: F4's local build and its tests (about 03:15 to 03:21) ran while index-size's strict
  hold index-size-e3c-freebsd (03:10 to 04:20) was live: a check that printed quiet's exit
  instead of stopping on it; the macOS fixture build of 100-document indexes (02:50 to 03:23,
  F_FULLFSYNC-bound, 55 s of CPU) also overlapped it. Its FreeBSD A/B rounds in that window may
  need a rerun (the lead decides). Builds now run behind `quiet check`, and this item's fixture
  builders on the gate VM are paused (SIGSTOP) during other agents' strict holds. On the gate
  VM, `research/measure.sh build out vm 3` (the scheduler researcher's `research_workloads`,
  pid 268527) has held one core at 100% since about 01:28 with no hold listed; every gate-VM
  number here since then ran beside it.
- Findings folded in: none yet.
- Last verified: 2026-10-18. macOS: Release build and all 34 tests green at 6429e00 (F7).
  Gate VM: strata_engine at F5, F6, F7 built (clang 18); the baselines' many tools rebuilt
  with open-writers. FreeBSD: strata_engine over six libraries built (clang). CI: b11c5cc
  green on all eight jobs; 7f7f791 run 39123392035 and 094d59f run 39127562866 running.
