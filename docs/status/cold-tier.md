# cold-tier

> Status: HISTORY (2026-11-08): superseded by Xapiand's `.campaign/xapiand/` (`campaign map`); kept for its results and reasoning. Was: (2026-10-27) the designer's ledger for `docs/design/cold-tier.md`.

- Design: `docs/design/cold-tier.md` revision 2 (hot, warm, cold and remote tiers; the
  per-partition catalog; demotion by unmapped twins; D9 to D11 for step 1).
- Review: `docs/reviews/cold-tier-design.md` (preliminary, APPROVE WITH CONDITIONS; re-check 1,
  GO WITH CONDITIONS for step 1, C5 and C6 on revision 2's text).
- Branch / worktree: `design/cold-tier` from main `98ce743`, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier`; citations checked at main `8798fe1`, whose
  `include/` equals `98ce743`'s.
- State: designing (revision 2 ready for the rechecker on C5 and C6).
- Done:
  - strata:4bc1a06 revision 0.
  - strata:8459c7b the architect's preliminary review.
  - strata:e6bdf25 revision 1: folds in CT-R1 (D3), CT-R2 (D2, D9), CT-R3 (D10), CT-R14 (D11,
    Q3) and CT-R16 (the criteria below); records CT-R9 as N1; lists CT-R4, R6 to R8, R10 to R13 and R15 as open
    for step 2 and step 3; adds Q5. The architect's probes rerun under a light hold with output
    identical to theirs (`twin_race`, `quiescence_probe`, `apply_throw_probe`,
    `warm_bytes_probe`). No build or benchmark (design only).
  - strata:46ce154 the architect's re-check 1.
  - strata:e1bfb7f revision 2: CT-R2's output maps and `install()`'s copies in D9, the `:4032`
    citation; CT-R17 owner-written facts (D2); CT-R18 the twin's fields (D3); CT-R19 commit with a
    marked record, `needs_heal()`, the prepare's skips (D9, D11); CT-R20 the swallowed-round scope
    and count (D9); the marker's lifetime (D2); the prepare's outcome and place (D9); Q5 ruled (a)
    with its corrections and T2w; C6 in K1, K2, K5. Text only, no build.
- Next: a rechecker confirms C5 and C6 on revision 2's text (no new architect pass). Step 1's
  items 1 and 2 may start with `eager_segments` on by default.
- When the cold tier lands (owner, 2026-10-28): the hybrid-search go/no-go runs next. Rerun C1
  with strata included and 1,000-vector tenants, then whatever experiments show whether
  optimization can open a clear lead in idle cost and p99 over pgvector, Qdrant, LanceDB and
  Elasticsearch. The outcome is go (hybrid steps 1 to 4, `docs/design/hybrid-search.md` on
  `design/hybrid`) or dropping "best at AI retrieval" for good.
- Blocked on:
  - CT-R5's rollback (`fix/apply-atomic`) landing on `main`: lazy segments become the default only
    after it (step 1, item 6; K3).
  - The lead's restatement of T1 and T2 (CT-R9, CT-R11) before they gate; Q5's (b) decided on
    T2w's numbers before step 2.

## Done criteria for step 1

Each with its evidence on the branch, re-checked before landing.

- **K1.** Record and body, compare-exchange install, a loser that destroys only its own body;
  T6 over the install under TSan, with the mutants "body cleared in place", "body stored without
  compare-exchange", "loser reads the body it destroyed" and "facts written by a reader's map"
  (CT-R17).
- **K2.** D9's table in code, each map point's failure rule with a faultfs-injected test (open,
  `mmap` and check failures): no fence for host errors, damaged-and-skipped merge sources,
  commit's prepare leaving the index as `refresh()` leaves it and unfenced. Flush and merge output
  maps under faultfs `map()` failures keep the bytes and never fence (`output_map_probe` as the
  oracle). A damaged segment's first map inside commit's prepare: `corrupt_segment`, unfenced,
  `needs_heal()` true, a commit after `heal()` succeeds (CT-R19). An `EMFILE` at scheduling:
  `refresh()` succeeds, the swallowed-round count rises, the next round merges (CT-R20).
- **K3.** CT-R5's rollback on `main`, failing before and passing after; `apply_atomic_test`
  carries the probe's cases (each damaged section, eager and lazy) in build-test.
- **K4.** The twelve damage cases with lazy segments; a missing or resized manifest-named file
  fails the open (D11).
- **K5.** `demote()` twinning every record with D3's fields and forcing its publication, under
  D10's warm predicate; T6's mutants including "twin only bodied records", "twin drops
  `deletes_dirty`" (oracle: a reopen, CT-R18) and "close with a posted delete";
  T5 on one index with readers holding views across `demote()` and an index idle after posting
  deletes.
- **K6.** T1's step-1 rows: hot within 5% of today; demoted with its own target (N1); 1 and 10
  segments.
- **K7.** prospect's construction sites and Xapiand's seam listed, with the pin bump that adopts
  `seg()`.
- **K8.** CI green on every platform, ASan, UBSan and TSan.

## K7: who builds snapshots from `seg()`

`segment_view::seg()` maps a segment at its first call; with lazy segments the default (item 6),
every site below maps on the caller's thread the first time it reads a segment. Checked at
prospect main prospect:e8e5bd9 and Xapiand master Xapiand:6c554f728.

- **prospect** pins strata:3df7af4f. `include/prospect/` builds no snapshot: `searcher.hh`,
  `shards.hh` and `reorder.hh` take `segment_snapshot` and `merge_source` from their callers.
  The callers are its tools and tests, each `{v.seg(), v.deleted, v.dead, &ix.fields()}` over
  `ix.segments()`, or a direct `seg()->core()` read:
  - tools: `tools/prospect_engine.cc:215`, `:236`, `:437`, `:502`, `:608`, `:710`, `:809`;
    `tools/parity_terms.cc:119`; the README's example (`README.md:65`).
  - tests: `background_merge_test.cc:77-78`, `:625`; `churn_test.cc:102`, `:111-112`, `:250`;
    `engine_query_test.cc:71`; `fields_test.cc:68`, `:79-80`, `:168`, `:252`;
    `format13_test.cc:203`, `:639`, `:952`, `:958`, `:1034-1035`; `index_test.cc:72-76`, `:513`,
    `:672`; `lifecycle_test.cc:883`; `merge_test.cc:323`; `multisegment_test.cc:157`;
    `reorder_test.cc:257`; `shards_test.cc:536`; `values_test.cc:352-356`; `view_sources.hh:23`.
- **Xapiand's seam** pins strata:5d742b24 and prospect:850d4e8. Under a host it sets `defer_io`
  (`src/engine/strata/shard.cc:170`), which forces eager segments, so the default does not reach
  a server's indexes. The sites, all on the shard's `Viewed` or index:
  - `src/engine/strata/search.cc:669-671`: one `shard_input` per shard per search.
  - `src/engine/strata/shard.cc:337-338`: `verify_fields()` at open reads every segment's
    dictionary, so a lazy open there maps every segment anyway.
  - `shard.cc:549-552` (a document by docid), `:573-578` (`locate_key`), `:613-614`
    (`live_count`), `:1200-1202` (the statistics' bounds), `:1257-1258` (a scan's hits).
- **The pin bumps that adopt `seg()`**: prospect:e49453d (pins strata:c4ea4ce, every construction
  site calls `seg()`) and Xapiand:74d8236e5 (repins prospect:becfe3c; `search.cc` and `shard.cc`
  call `seg()`). Both current pins are past them. The lazy default reaches each at its next strata
  pin bump. prospect's suite at its pin with lazy segments for the whole run
  (`STRATA_EAGER_SEGMENTS=0`) passes 48 of 51: `format13_test.cc:595`, `:670`, `:777` and
  `index_test.cc:1427`, `:1823`, `:2093` expect the open to find segment damage or an injected
  `EIO`, which a lazy open leaves to the first map (D11). Lazy stays the default (the lead's
  ruling, 2026-11-10): prospect's pin bump, a prospect-side follow-up, changes those cases to
  expect the damage where lazy finds it, not to set `eager_segments`. A missing or resized file
  still fails the open, so `index_test.cc:1421` and "segment of another size" stay. The rest:
  - `format13_test.cc:595` (a segment with a bit outside the mask), `:670` (another identity) and
    `:777` (a rewritten version): the open succeeds; the first read that maps the segment (a `get`
    of its key, or `segments()[0].seg()`) throws `corrupt_segment` ("segment outside its
    manifest's structures or versions", "segment identity differs from its manifest entry", the
    version's message), and `needs_heal()` and `heal()` report the segment.
  - `index_test.cc:1427` (a footer byte at the file's size): the open succeeds; the first map throws
    `corrupt_segment` (the footer's checksum), and `heal()` reports the segment.
  - `index_test.cc:1823` (both loading reads fail with `error`): a lazy open makes no segment read,
    so the open succeeds. At the first map a host error (`EMFILE`, `ENFILE`, `ENOMEM`, `EACCES`)
    reaches the caller unchanged, marks nothing and reads once the IO recovers; `EIO` twice is the
    medium's: `corrupt_segment`, marked.
  - `index_test.cc:2093`, "another index's segment of the same size": the open succeeds; the first
    map throws `corrupt_segment` ("segment differs from its manifest entry"). "A resealed document
    count": the open still fails, now on the live-docs check ("live docs disagree with the
    manifest"), which runs at open, before the first map's document count.

## Log

- 2026-10-27: revision 0 drafted. Decisions D1 to D8, invariants I1 to I7, gate rows T1 to T7,
  questions Q1 to Q4. No build or benchmark run (design only).
- 2026-10-27: revision 1 after the preliminary review: D9 to D11, D3 twins every record, N1,
  K1 to K8, Q5; Q1 to Q4 marked ruled.
- 2026-10-27: revision 2 after re-check 1: C5's text (CT-R2 outputs, CT-R17 to CT-R20, the
  marker's lifetime, the prepare's wording, Q5) and C6's tests in K1, K2, K5.
