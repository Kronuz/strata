# History rewrite

On 2026-10-16 this repository's history was rewritten to strip tool
trailers from commit messages. File contents did not change, apart from
pins and citations of other Kronuz libraries that moved to their own
rewritten SHAs, but every rewritten commit has a new SHA. Commit messages
were updated by the rewrite; the gate reports, reviews, and perf notes
under `docs/` still cite the old abbreviated SHAs listed here.

| Old | New | Date | Commit |
|-----|-----|------|--------|
| `bd15441` | `e8db5af` | 2026-10-11 | Add the golden segment fixture and the segment fuzz target |
| `03e2544` | `9bdb146` | 2026-10-12 | Architecture: why nested disjunctions use no WAND (review C7) |
| `24429ec` | `efd8b6e` | 2026-10-12 | Gate report: term dictionary breakdown and target (review C11) |
| `2542867` | `b68c9e9` | 2026-10-12 | Validate the docids section and expose docid(ord) |
| `2731bb2` | `5d1639d` | 2026-10-12 | Bound sums conservatively; harden TOC, stored, and writer limits |
| `2e9cb57` | `785a85e` | 2026-10-12 | Score Lucene BM25 by one-byte norms |
| `3ec6de8` | `1182de6` | 2026-10-12 | G5 run: resumable query and indexing trials of four engines |
| `474b29c` | `231f564` | 2026-10-12 | Phase 3 design draft: index lifecycle |
| `4e756bc` | `5bdb5fe` | 2026-10-12 | State the fill window contract: at most 4,096 ordinals |
| `4edad0a` | `f574e54` | 2026-10-12 | Gate report: resolutions of the review's blocking items |
| `500ef45` | `7a30208` | 2026-10-12 | gate.py: medians, bootstrap intervals, raw ratios, Xapian rows |
| `523438b` | `34ba600` | 2026-10-12 | Advance small disjunctions without a heap |
| `5ec6c0d` | `ffc5733` | 2026-10-12 | Speed up phrases over dense terms |
| `5fa66a4` | `8d8b889` | 2026-10-12 | Check pruned top-10 in parity too |
| `63a5339` | `19c557b` | 2026-10-12 | Close the second re-check's findings: G7, G8, G1, C4, C12 |
| `64623d0` | `53cbd3a` | 2026-10-12 | Fit postings in budget: narrow skip offsets, compact pair lists |
| `662558f` | `dba8d01` | 2026-10-12 | Test that identical documents keep the smallest ordinals |
| `69813ba` | `0e525ec` | 2026-10-12 | Make level-1 skip data fixed-width (format 10) |
| `6ad6ca3` | `a0dcfa4` | 2026-10-12 | strata_engine: dictstats, the term dictionary's bytes by part |
| `771634f` | `83eac31` | 2026-10-12 | Close the gate review's reader-safety and phrase-bound findings |
| `7f48e42` | `30a73a7` | 2026-10-12 | Test concurrent reads of one segment, clean under TSan |
| `9204851` | `2ea10e3` | 2026-10-12 | Phase 2 gate report |
| `9dba054` | `d9e70bd` | 2026-10-12 | g5.sh: remove a make-built index before rebuilding it |
| `b2ffe24` | `e2a2ee5` | 2026-10-12 | Revise the Phase 3 design for the architect's review |
| `cb01c65` | `e786506` | 2026-10-12 | gate.py: one pipe floor across trials |
| `d87783a` | `eba5043` | 2026-10-12 | PLAN: ratchets on all four axes, with a considerable margin |
| `d885f7a` | `f953df2` | 2026-10-12 | PLAN: strata does not extract ledger (Phase 3 design review D20) |
| `e059d64` | `7861018` | 2026-10-12 | Fuzz segments through recomputed checksums |
| `e073258` | `c615938` | 2026-10-12 | Add NEAR: terms within a window, in any order |
| `e0d5497` | `c8966ef` | 2026-10-12 | PLAN: beat every baseline, distributed-first, test parity |
| `e5fafec` | `d187fe2` | 2026-10-12 | Close the re-check's findings: skip sums, positionless phrase terms |
| `ea5dc10` | `c7e9659` | 2026-10-12 | quiet_trials: default load limit 6 |
| `eb9452f` | `ba1af70` | 2026-10-12 | PLAN: track and minimize memory; no garbage collector |
| `f1adaf9` | `f7d3b53` | 2026-10-12 | bench engine: serve without a build step |
| `03645e7` | `f822323` | 2026-10-13 | Bulk top-k for conjunctions of terms |
| `1ecf85b` | `6d03bf0` | 2026-10-13 | Review: Phase 3 lifecycle rows (REJECT, L1-L4) |
| `28c7175` | `2a920d4` | 2026-10-13 | Dictionary keys coded over the segment's alphabet |
| `28e2362` | `90399a9` | 2026-10-13 | L2: reject wrapping position indexes; reach or_bitset's checks |
| `405fbc0` | `e222f06` | 2026-10-13 | Merge policy: Lucene's TieredMergePolicy contract |
| `41204b6` | `f6b8f6e` | 2026-10-13 | index: drop dead segments at commit; free files in bounded steps |
| `48b2154` | `f0c8958` | 2026-10-13 | L4: storage admission covers what it admits |
| `4e9f8d9` | `00e253b` | 2026-10-13 | Term dictionary: offsets from restart runs, pairs only with skip blocks (P12) |
| `5035146` | `033ba2b` | 2026-10-13 | Gate report: Phase 3 lifecycle rows |
| `5a8b5e3` | `a0f2173` | 2026-10-13 | Index files: manifests, live-docs, and metadata generations |
| `5e6b247` | `65563c0` | 2026-10-13 | C13: reach both impact-count sub-conditions; coverage as a script |
| `6d52f79` | `8302cc9` | 2026-10-13 | index: merge at commit under auto_merge |
| `79f7dcc` | `01128e2` | 2026-10-13 | Storage admission: byte_budget for storage, no_space verdicts |
| `7ca96ad` | `9b901ed` | 2026-10-13 | index_test: close the index before its budget leaves scope |
| `80eadc2` | `e07bd91` | 2026-10-13 | Gate report: termdict and hash-map bytes per key as measured |
| `814fc71` | `7bd84ec` | 2026-10-13 | Count conjunctions with membership probes (Track P7) |
| `84d3fbe` | `e794bd0` | 2026-10-13 | C13: resealed corruption tests; drop provably redundant checks |
| `852a154` | `b5ecace` | 2026-10-13 | Pin bitpack fb68e7f: its NEON path builds with Apple clang |
| `8ddff6f` | `ca0b008` | 2026-10-13 | P12: alphabet-coded dictionary at 0.992x Lucene |
| `90b23aa` | `38af747` | 2026-10-13 | merge policy: rewrite an oversized segment alone once its deletes pass the limit |
| `a3b6f18` | `01ade4b` | 2026-10-13 | fuzz: keep the termdict block-offset input |
| `b4f8a3b` | `b6c658f` | 2026-10-13 | Retention removes manifests durably first; T2 compares every digest |
| `c266040` | `51b1783` | 2026-10-13 | L1: install validates a checkpoint the way recover validates the head |
| `c3a9387` | `8da9768` | 2026-10-13 | Gate report: the G5 run, fuzz hours, memory and disk |
| `c48fada` | `b6fae01` | 2026-10-13 | recover --to commits the chosen manifest as a new generation |
| `c8f932a` | `af7ca05` | 2026-10-13 | verify: ordinals with ties as sets, under both scoring schemes (C14) |
| `ccb4fb1` | `69eb033` | 2026-10-13 | T1 at log positions; create survives power loss |
| `cf206b2` | `a685d90` | 2026-10-13 | fuzz: stop at the first UBSan finding |
| `d2fe970` | `c525354` | 2026-10-13 | Record each section's verification region size (P15) |
| `d41c957` | `2e83c51` | 2026-10-13 | Pin termdict 0297799: offset checked before the pointer |
| `f4f950f` | `cef3fa7` | 2026-10-13 | Gate report: termdict lookup percentiles |
| `00011c5` | `1cc9416` | 2026-10-14 | review: Track C fifth revision check (CHANGES REQUIRED, M8 and M9) |
| `0547f87` | `b869ade` | 2026-10-14 | gates: C1's reruns at consensus faef7ad; the M1 retired |
| `05df531` | `1147075` | 2026-10-14 | index: media faults heal or fail closed, never answer wrong |
| `06c96c7` | `c10c50a` | 2026-10-14 | Track C: progress and condition 17's text |
| `0c62284` | `f198e15` | 2026-10-14 | review: Phase 3 lifecycle re-check (REJECT) |
| `148eebb` | `f5f7c7b` | 2026-10-14 | tools: strata_engine reads stdin in large reads, not through std::cin |
| `1616e52` | `c6dc20c` | 2026-10-14 | T1: release in the workload, recovery cut at every mutation, work after it |
| `2e1f3c3` | `ca5bdac` | 2026-10-14 | review: Track C fourth revision check (CHANGES REQUIRED, M4's remainder) |
| `30781a0` | `de01626` | 2026-10-14 | bench: IResearch and PISA join the baselines; design text for L5-L8 |
| `31e0e1f` | `e694eb1` | 2026-10-14 | tools, test: coverage branches from llvm-cov's JSON, lines from lcov |
| `33cf29f` | `cbc8387` | 2026-10-14 | Gate report: Phase 3 lifecycle rows for the re-check |
| `34b21ef` | `4ecc039` | 2026-10-14 | review: Phase 3 lifecycle second re-check (REJECT) |
| `35989ce` | `eeaee9c` | 2026-10-14 | coverage: count on evidence; drop reader.hh's dead code |
| `470c6d3` | `75c118f` | 2026-10-14 | review: Track C design revision check |
| `4fad12e` | `4517373` | 2026-10-14 | Track C: record the liveness fixes and T3/T4 harnesses |
| `57a667f` | `f9f72d3` | 2026-10-14 | design: D11's plan for the Worker on the shared journal |
| `5dfe415` | `e968fbf` | 2026-10-14 | tools: drop terms over 245 bytes, as the other engines do |
| `6462402` | `3257590` | 2026-10-14 | index: drains keep held reservations and free bytes durably (L5, L8) |
| `64e7d12` | `53e281e` | 2026-10-14 | gates: Phase 3 lifecycle report at 2ab03e9 for the second re-check |
| `703d6e8` | `e6bc3f5` | 2026-10-14 | tools: coverage unioned over every test binary |
| `7046856` | `ddd42fa` | 2026-10-14 | docs: C1's response to the fifth revision check; D11's block manifests |
| `713c938` | `3ea6c6e` | 2026-10-14 | gates: Phase 3 lifecycle report at 547b4b8 for the third re-check |
| `79f7258` | `b98785d` | 2026-10-14 | review: Track C design second revision check |
| `7f8d7eb` | `23a4a9a` | 2026-10-14 | design: Track C, the consensus gaps before Phase 3R |
| `8059f37` | `226acf9` | 2026-10-14 | gates: C1's T8 robustness row |
| `83b4306` | `8e4d304` | 2026-10-14 | Phase 3 report: state that the coverage table is one run |
| `85b3cdc` | `bd7dbd2` | 2026-10-14 | index: L8's remainder; conditions 15, 16, and 17 |
| `86b50a5` | `2ab03e9` | 2026-10-14 | index: close the re-check's L1, L5, L6 and conditions 2, 9, 10 |
| `898432c` | `92d549f` | 2026-10-14 | Track C: what the coordinated DTC2 commit still waits for |
| `9128a07` | `a9887e8` | 2026-10-14 | review: Track C third revision check and C1 as built (CHANGES REQUIRED) |
| `914db86` | `e337612` | 2026-10-14 | index_test: reach every check that binds a file to its manifest entry |
| `92d8c6d` | `c18f447` | 2026-10-14 | gates: Phase 3 lifecycle report at bd7dbd2 for the fourth re-check |
| `9b33127` | `5b99909` | 2026-10-14 | Track C: T2's full budget passes |
| `a10df59` | `1dcd944` | 2026-10-14 | files: a live-docs bitset sized without wrapping (review L7) |
| `a1ac27b` | `6a2b97d` | 2026-10-14 | Track C: record C1's progress in consensus |
| `a669716` | `f81d705` | 2026-10-14 | Track C: the journal conditions from the third revision check |
| `a99ff98` | `5e0e6d0` | 2026-10-14 | docs: D3-D5 and T5 after the re-check; the gate VM's discard setting |
| `ad16376` | `0d34513` | 2026-10-14 | tools: strata_engine indexes through strata::index, and serves its segments |
| `b3f0e52` | `0029b17` | 2026-10-14 | tools: STRATA_BUDGET_MB, the node indexing budget (D17) |
| `b493296` | `547b4b8` | 2026-10-14 | index: report a blocked retention; conditions 2, 13, and 14 tested |
| `b500843` | `d86e160` | 2026-10-14 | design: Track C third revision (R8's journal points, R22, text) |
| `bc10295` | `8f6028d` | 2026-10-14 | index: count each segment's deletes; no bitmap for a segment without |
| `bd66be1` | `1d9e182` | 2026-10-14 | review: Track C design (APPROVE WITH REQUIRED CHANGES) |
| `cae2923` | `25e2a63` | 2026-10-14 | review: Phase 3 lifecycle third re-check (REJECT on L8's remainder) |
| `cd81685` | `42f41da` | 2026-10-14 | design: Track C D10 as built in consensus afd456d |
| `d3dc7da` | `e9f129d` | 2026-10-14 | index: one batched sync for a commit's data files |
| `d7a4f7d` | `d931188` | 2026-10-14 | tools: coverage_union.py selects headers by path fragment |
| `d8d1f5a` | `c670309` | 2026-10-14 | design: Track C second revision for R1-R20 |
| `d9c4b4c` | `ed38bb7` | 2026-10-14 | Track C: record RFTSNP02 and the decoder fuzzer |
| `f181d3f` | `451c798` | 2026-10-14 | Track C: design text for the C1 review's changes |
| `f9f7aeb` | `0404540` | 2026-10-14 | Track C: C1 gate report with T3 and T4 so far |
| `fd1ba2c` | `7cf9dcd` | 2026-10-14 | design: Track C revised for R1-R15 |
| `2e774db` | `65f67a2` | 2026-10-15 | bench: overnight gate runs and a merged strata engine |
| `2fc2a0b` | `f3a34a6` | 2026-10-15 | disjunction: sum scores in clause order, not bound order |
| `422c227` | `483da0b` | 2026-10-15 | index: charge the flush's memory to the indexing budget |
| `5288ea5` | `a8d0fda` | 2026-10-15 | docs: Phase 4 design revision 2, for revision check 1's N1-N9 |
| `5d324ab` | `70eca5b` | 2026-10-15 | index: fence on a segment order regression at commit (S1) |
| `6022d98` | `b26cfe3` | 2026-10-15 | docs: layers design revision 1, answering R1-R11 |
| `9abf71c` | `6d8ded9` | 2026-10-15 | docs: layers design, the lifecycle core and section plugins |
| `b124431` | `4fdea76` | 2026-10-15 | docs: Phase 4 design revision 1, for the review's R1-R20 |
| `c963cfa` | `94fffcc` | 2026-10-15 | search: a one-segment snapshot takes the single-segment path |
| `e8accae` | `03d0783` | 2026-10-15 | gates: Phase 3 queries, 16-segment strata against merged baselines |
