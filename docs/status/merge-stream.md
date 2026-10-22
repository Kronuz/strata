# merge-stream
- Task: a merge's working memory bounded by a few blocks and the key builders' state, not by the merged segment (explorer: docs/perf/bigmem.md, explore/bigmem 7eb2c11). Segment bytes identical to main's; no format change.
- Branch / worktree: perf/merge-stream from main 9ba0659, at /Users/kronuz/code/Kronuz/strata-wt/merge-stream
- State: done, pushed, not merged. Evidence in docs/perf/merge-stream.md.
- Done:
  - Stream the stored section: `stored_builder::stream_to` writes each block's frame through the section stream; flushes' finish() uses the same frame writer. test/merge_stream_test.cc pins merged-file digests of five shapes from main's whole-section merge and bounds the merge's budget peak.
  - Key-ordered merge plan: numbered straight from the order (no docid-sorted copy, no refs copy), plan charge returned, keys builders sized up front.
  - Evidence (macOS x86_64): Release ctest 38/38; ASan+UBSan 38/38; TSan see the perf doc. big1 and fits1 A/A + A/B (six trials, strict hold): peak RSS 334.6 -> 159.4 MiB (fillrandom), 364.8 -> 192.0 (readrandom), 674.6 -> 230.4 (ycsb_a 1 KiB); budget peak 96 -> 38, 106 -> 50, 210 -> 11 MiB; throughput flat or up; fits1 RSS 61 -> 49, 69 -> 55, 312 -> 190; digests identical.
- Next: the key builders' transients (fusefilter build copy and 16-byte peel pairs; termdict block sink) are library changes; Linux/FreeBSD runs; a mapped-pages residency measure.
- Blocked on: nothing.
