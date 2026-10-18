# small-indexes

- Design: docs/design/small-indexes.md (a note, not yet a design); experiments in
  docs/perf/small-indexes.md, each written before it runs.
- Branch / worktree: perf/small-indexes at /Users/kronuz/code/Kronuz/strata-wt/small-indexes,
  from origin/main at 6e9ba58, with perf/inverter merged at 339acf7 (ad12899).
- State: implementing (F1, F2, F4, F5 kept on Linux and macOS; M2 measured; F6 and F7
  built, their A/Bs next; FreeBSD fixtures building).
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
    node budget by about 0.6 GB: noted for perf/inverter and D17's accounting), disk 9% under
    Tantivy and 2.5% over Lucene; 1,000 x 100 at once, 4.52x Tantivy [4.32, 5.63] and 10.56x
    Lucene, 23% of Tantivy's peak RSS; strata half system time there (10 fsyncs per index
    created and committed once).
  - F6 (24eecc4): a cold segment's parsed state behind a pointer (800 B inline before); F7
    (6429e00): open reads no retained manifest when nothing can be obsolete. Both green on
    the full macOS suite; A/Bs next.
  - Notes N5 to N8 for format 13's designer (small segments) in the perf note.
- Doing: F6 and F7's open A/B (gate VM, then macOS), FreeBSD's fixtures and its A/B.
- Next:
  1. FreeBSD: base, F1, F4, F5, F6, F7 in one rotated run (the procedure is in the perf note).
  2. M2's per-index creation cost: 70 system calls and 10 fsyncs per index created and
     committed once; the baselines' counts beside it; group commit across indexes is the
     note's direction 5 (a design question, the architect's).
  3. The second descriptor: the directory handle the completion library's PosixIO keeps
     (path-based I/O, or the owner lock taken on the directory handle itself); both change
     a safety or compatibility property, so they go to the architect as options first; and
     idle close behind handles (F3 as a component, the architect's first).
- Blocked on: nothing. For the architect (via the lead): F1's and F5's moved checks: a segment
  whose pinned sections (F1), footer, or document count (F5) disagree with the manifest at the
  right size now fails at first use as corrupt_segment, which heal() reports, not at open; a
  missing segment, one of another size, or disagreeing live docs still fail the open.
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
- Last verified: 2026-10-18. macOS: Release build and all 34 tests green at e3e5f2e. Gate VM:
  strata_engine at F5 built (clang 18); the baselines' many tools rebuilt with open-writers.
  CI: b11c5cc green on all eight jobs; 7f7f791 dispatched (run 39123392035).
