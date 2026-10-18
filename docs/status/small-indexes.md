# small-indexes

- Design: docs/design/small-indexes.md (a note, not yet a design); experiments in
  docs/perf/small-indexes.md, each written before it runs.
- Branch / worktree: perf/small-indexes at /Users/kronuz/code/Kronuz/strata-wt/small-indexes,
  from origin/main at 6e9ba58, with perf/inverter merged at 339acf7 (ad12899).
- State: implementing (M0 done on the gate VM; M1's baseline runs wait for the VM).
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
- Doing: M1 (Tantivy and Lucene) once the VM's other runs end.
- Next:
  1. M0 on the gate VM: 10,000 indexes at 1, 10, 100, 1,000 documents; then macOS.
  2. M1: builders and openers for Tantivy and Lucene; the fastest baseline per row.
  3. M2: N = 8 and N = 1,000 small indexes writing at once on 8 cores.
  4. The biggest per-index cost first: lazy open, idle close, shared machinery, or the
     small-segment format profile (notes to format 13's designer).
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-18. Release builds of strata_engine (macOS, Apple clang; the VM,
  clang 18) with build-small and open-many working; the baseline tools built on the VM.
