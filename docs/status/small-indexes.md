# small-indexes

- Design: docs/design/small-indexes.md (a note, not yet a design); experiments in
  docs/perf/small-indexes.md, each written before it runs.
- Branch / worktree: perf/small-indexes at /Users/kronuz/code/Kronuz/strata-wt/small-indexes,
  from origin/main at 6e9ba58, with perf/inverter merged at 339acf7 (ad12899).
- State: implementing (F1 and F2 kept; M1's fixtures building on the gate VM, then its timed opens
  under a short strict hold; M2 next).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Worktree and branch; perf/inverter merged for its index path (ad12899).
  - M0 to M2 written before they run (ca4e8b6).
  - M0's tools (strata_engine build-small, open-many; the attribution build) and M1's
    (bench/engines/tantivy-0.26-st/src/bin/many.rs, lucene-10.4.0-st/src/main/java/Many.java).
  - M0 on the gate VM, 10,000 indexes open at once: 7.3 to 13.5 KB anonymous and 4.7 to 109 KB
    file-backed per idle open index, 2 descriptors, 1 mapping, open 83 to 149 us warm (p50) and
    1 to 3.4 ms cold; heap attributed (the segment's parsed state 1.8 to 3.0 KB, the per-index
    inline executor 1.4 KB, the index object 1.3 KB, an empty deque 0.7 KB).
  - F1 (2f9be5d): open reads no segment until it is used. Per idle open index 7.3-8.6 KB of
    heap to 5.4 KB, 4.7-81 KB of file-backed pages to 0.1 KB, one mapping to none; warm opens
    12-40% faster; cold no slower within the noise, 2.3x faster at 100 documents; first use
    at most 3 us more. Kept. A semantic for the architect: a pinned section damaged with the
    footer whole fails at first use (corrupt_segment, which heal() reports), not at open.
  - F2 (c907d3d): no merge machinery until the first merge (the group and inline executor,
    and a std::list for the reclaim queue): 5.4 KB to 3.2 KB per idle open index, opens
    unchanged. Kept.
  - M1 for Tantivy at one document: 12.9 KB of heap, 24.9 KB file-backed, no descriptor,
    six mappings, warm open p50 122 us, cold 3.1 ms.
  - Notes N5 to N8 for format 13's designer (small segments) in the perf note.
- Doing: M1's fixtures (Tantivy and Lucene, 1, 10, 100 documents, 10,000 indexes each) under
  a light hold; then their opens and strata's under a strict hold of a few minutes.
- Next:
  1. M1's opens (strict hold, minutes); the fastest baseline per row.
  2. M2: N = 8 x 100k and N = 1,000 x 100 indexes writing at once on 8 cores, every engine.
  3. F3, the next per-index cost: the two descriptors (the owner lock and the directory's)
     or idle close behind handles (the note's direction 2); the choice goes in the perf note
     with its numbers, and a public component goes to the architect first.
  4. M0 on macOS (open-many's Mach counters) and FreeBSD.
- Blocked on: nothing. For the architect (via the lead): F1's deferred verification of pinned
  sections (above).
- Findings folded in: none yet.
- Last verified: 2026-10-18. Release builds of strata_engine (macOS, Apple clang; the VM,
  clang 18) with build-small and open-many working; the baseline tools built on the VM.
