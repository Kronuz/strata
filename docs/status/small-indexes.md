# small-indexes

- Design: docs/design/small-indexes.md, revised for the architect's completed review
  (docs/reviews/small-indexes-design.md at 5bb3797, SI-R15 to SI-R22); experiments in
  docs/perf/small-indexes.md, each written before it runs.
- Branch / worktree: perf/small-indexes at /Users/kronuz/code/Kronuz/strata-wt/small-indexes,
  from origin/main at 6e9ba58, with perf/inverter merged at 339acf7 (ad12899) and at b5f375c
  (8ade888, its last commit with green CI).
- State: designing (revised for the completed review; ready for re-check). Rulings: D1 and D2
  APPROVE WITH CONDITIONS; D3 REJECT as written, APPROVE WITH CONDITIONS as durability-modes'
  checkpoint batcher; Q1 yes, after D1; Q2 in SI-R11's first form. F1 to F7 kept as a set by
  the lead's ruling of 2026-10-18; F3 parked by the same ruling.
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
  - Cold opens (c6758d1): F5 1.23-1.43x faster than F4 cold, F7 another 1.29x; a cold open
    takes about 645 us on the gate VM (Lucene 1.5-2.4 ms, Tantivy 2.0-4.5 ms in M1).
  - perf/inverter merged at b5f375c (8ade888; one conflict, remove_copy, theirs taken; the
    full macOS suite green). M2b on it (d0fb8a2): 8 x 100k at once 1.92x Tantivy [1.85,
    2.07] with peak RSS 27% under Tantivy's (the overdraw M2 showed is gone); 1,000 x 100 at
    once 5.13x Tantivy [4.92, 5.69].
  - Notes N5 to N8 for format 13's designer (small segments) in the perf note.
  - The lead's rulings (2026-10-18): F1 to F7 kept as a set, F1 as F5's base, F5's and F6's
    first-use misses accepted; the checks moved to first use accepted on the condition that
    a segment damaged at its size fails every read path clean and heal() reports it; F7's
    report at the first retention pass accepted; F3 parked (a host policy, Xapiand's).
  - The condition's tests (8b834ee): one byte of the term index, the footer, postings, or
    stored records damaged at the segment's size, then a search, a get by key with its stored
    record, or a merge: each throws corrupt_segment or answers exactly, every path that reads
    the damage throws, and heal() reports the segment in all twelve cases.
  - The design note (6c234a9): D1 (a lazily made write_state, after perf/inverter lands),
    D2 (the owner lock on the directory handle, the old and new binaries' exclusion by the
    manifest's protocol field and the lock file's retirement), D3 (batched syncs over
    today's files first, a shared journal as its own design), with gate rows T1 to T10 and
    questions Q1 and Q2.
  - The lead's M2 rerun on a quiet host (si-m2d, strata at 8ade888 against Tantivy): 8 x
    100k at once 1.746x Tantivy [1.666, 1.895], 1,000 x 100 at once 5.724x [5.614, 5.863],
    Tantivy's anchor inside M2's spread.
  - The architect's review (interim 7c23560, completed 5bb3797) folded into the design
    (2026-10-19): D1 restated on main's members and split once (SI-R15, SI-R16, SI-R13); D2
    inside format 13 with refusal of network filesystems by type (SI-R17, SI-R18, SI-R2); D3
    restated as durability-modes' checkpoint batcher (SI-R19, SI-R20, SI-R7, SI-R8); D3-C as a
    create that writes nothing (SI-R22, SI-R11, SI-R21); Q1 and Q2 answered. No build or
    measurement for this step (docs only).
- Doing: nothing running; stopped at ready for the architect's re-check of SI-R15 to SI-R22.
- Next (the design's "First steps"):
  1. The architect's re-check of the revision (the lead runs it).
  2. D2's completion call (Kronuz/completion, additive; Detent links it): the directory-handle
     owner lock with the filesystem rule and the macOS barrier on the directory descriptor.
  3. D2 inside format 13 before the freeze (phase4/format13-on-main, with the format 13
     designer, C4), with D3-C's standalone create; T4, T5, T7, T11's standalone half.
  4. D1 on main (the buffer_state and owner_state split), once the lead sets done criteria.
  5. Q1 after D1; D3's batcher after durability-modes' re-check clears DM-R3 and completion's
     step 2 lands.
- Blocked on:
  - The re-check of this revision (the lead runs it).
  - D1: the lead's done criteria (review condition C1).
  - D3: durability-modes' re-check of DM-R3 (its step 5); completion's cross-IO directory
    barrier (its step 2) and a per-file sync call (additive).
  - For other designers, through the lead: durability-modes' D1 and "How small-indexes' D3
    relates" take SI-R19, SI-R20, and D3-C's journaled residue rule (C3); format 13's F20
    takes D2 (C4); Xapiand's host guide takes SI-R21 (C5).
- Process: F4's local build and its tests (about 03:15 to 03:21) ran while index-size's strict
  hold index-size-e3c-freebsd (03:10 to 04:20) was live: a check that printed quiet's exit
  instead of stopping on it; the macOS fixture build of 100-document indexes (02:50 to 03:23,
  F_FULLFSYNC-bound, 55 s of CPU) also overlapped it. Its FreeBSD A/B rounds in that window may
  need a rerun (the lead decides). Builds now run behind `quiet check`, and this item's fixture
  builders on the gate VM are paused (SIGSTOP) during other agents' strict holds. On the gate
  VM, `research/measure.sh build out vm 3` (the scheduler researcher's `research_workloads`,
  pid 268527) has held one core at 100% since about 01:28 with no hold listed; every gate-VM
  number here since then ran beside it. M2b's first run (strict hold si-m2b, 06:31 to 06:38)
  started while perf/inverter's light hold inverter-vm-keep2 (06:28 to 06:48, one thread,
  informational) was running: this item's eight- and thousand-thread trials and their 1M runs
  in /dev/shm perturbed each other (M2b's walls varied 2x; its rounds are discarded and rerun
  after their hold). Their keep2 rounds in that window may need a rerun (the lead decides).
  Strict holds here now wait for light holds too. The lead's M2 rerun (strict hold si-m2c,
  07:19 to 07:26) was perturbed again and discarded: a zstd build was running on the gate VM
  as it began, the scheduler research's light hold sched-vm-a (8 vCPUs) started at 07:22
  inside it, and an inverter build started at 07:26; its walls varied 2x and Tantivy's
  anchor ran at 14.9 to 26.2 s against M2's 10.0 to 11.1. Rerun in a quiet window.
- Findings folded in: SI-R15 to SI-R22 and the open SI-R7, SI-R8, SI-R11, SI-R12, SI-R13,
  SI-R14 (the design's "Findings' status"), in the revision commit below.
- Last verified: 2026-10-18. macOS: Release build and all 34 tests green at 6429e00 (F7).
  Gate VM: strata_engine at F5, F6, F7 built (clang 18); the baselines' many tools rebuilt
  with open-writers. FreeBSD: strata_engine over six libraries built (clang). macOS: the full
  suite green on the merge (8ade888). CI: 094d59f (F1 to F7) green on all eight jobs (run
  39127562866); d0fb8a2 (the merge) dispatched (run 39133459119).
