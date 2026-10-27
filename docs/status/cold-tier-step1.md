# cold-tier step 1: record and body, D9's map points

> Status: BUILDING (2026-10-27) the developer's ledger for `docs/design/cold-tier.md` step 1,
> items 1 and 2, with D9's table and failure rules (K2).

- Design: `docs/design/cold-tier.md` revision 2 on `design/cold-tier` (strata:f5bd03d); review
  `docs/reviews/cold-tier-design.md` (preliminary, re-check 1, re-check 2). Neither is on this
  branch; read them in `strata-wt/cold-tier`.
- Branch / worktree: `phase/cold-tier-step1` from main strata:d0717a2, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step1`.
- Scope: items 1 (record, body, failed marker, compare-exchange install, owner-written facts,
  atomic counters, `options::eager_segments`) and 2 (`segment_view::seg()`), and item 3's D9
  table with its failure rules, `needs_heal()` over markers and the swallowed-round count, since
  K2 asks for them. D11's open-time name and size check and first-map checks are in because lazy
  segments need them to open at all. Out of scope: `demote()` (item 5), the twelve damage cases
  (K4), lazy as the default (item 6, waits for CT-R5), prospect's and Xapiand's call sites (K7).
- Default: `eager_segments` is on. `STRATA_EAGER_SEGMENTS=0` turns it off for a whole test run.
- State: built, awaiting the code reviewer (and CI, which the lead watches).
- Done:
  - strata:d0ab622 record and body, failed marker, compare-exchange install, owner-written facts,
    `seg()`, D9's map points and failure rules, `prepare_commit()`, `merges_skipped()`,
    `needs_heal()` over markers, open's name and size check, `options::eager_segments`,
    `test/cold_segment_test.cc`, tests adapted. Later commits on the branch: a comment fix, the
    M14 kill, this ledger and `docs/mechanisms/segment-records.md`.
- Next: the code reviewer; K4's twelve damage cases with lazy segments (step 1, item 4); item 5
  `demote()`; item 6 once CT-R5 is on `main`; prospect's and Xapiand's call sites (K7 below).
- Blocked on: nothing.

## Done criteria carried from the design (ticked with evidence; never edited)

- [x] **K1.** Record and body, compare-exchange install, a loser that destroys only its own body;
  T6 over the install under TSan, with the mutants "body cleared in place", "body stored without
  compare-exchange", "loser reads the body it destroyed" and "facts written by a reader's map"
  (CT-R17).
- [x] **K2.** D9's table in code, each map point's failure rule with a faultfs-injected test (open,
  `mmap` and check failures): no fence for host errors, damaged-and-skipped merge sources,
  commit's prepare leaving the index as `refresh()` leaves it and unfenced. Flush and merge output
  maps under faultfs `map()` failures keep the bytes and never fence (`output_map_probe` as the
  oracle). A damaged segment's first map inside commit's prepare: `corrupt_segment`, unfenced,
  `needs_heal()` true, a commit after `heal()` succeeds (CT-R19). An `EMFILE` at scheduling:
  `refresh()` succeeds, the swallowed-round count rises, the next round merges (CT-R20).
- [x] **K4** (the part item 1 touches). A missing or resized manifest-named file fails the open (D11).
- [x] **K7** (listing only). prospect's construction sites and Xapiand's seam, with the pin bump
  that adopts `seg()`.
- [~] **K8** (the part this branch can show). ASan, UBSan and TSan clean, with eager on and with lazy
  forced on; CI green on every platform (the lead watches CI).

## Evidence

Builds under `.scratch/{rel,asan,tsan}` (LLVM clang, CI's flags: rel `-O2 -g` without NDEBUG so
asserts are live; ASan+UBSan Debug `-O1`; TSan `-O1 -g`), 54 ctest tests each. Caveat: the three
builds share one FetchContent base dir, so the dependency libraries (lz4, zstd, compressors) were
rebuilt in turn by whichever build ran last; a flavor mismatch fails to link, and every binary
linked, but an ASan or TSan binary may have linked an uninstrumented dependency. Core logic is
header-only and always instrumented.

- **Suite**: eager and lazy (`STRATA_EAGER_SEGMENTS=0`) pass 54 of 54 on rel, ASan+UBSan (detect_leaks)
  and TSan (`halt_on_error=1`), zero reports (`.scratch/ctest-*-{eager,lazy}.log`). Lazy runs
  needed tests that assert an open fails on a segment's content to set `eager_segments`
  (`index_test`: the footer flip, the condition-15 loading reads, the `expect` table;
  `install_errors_test`: its `base()`, which asserts an eager install's reopen and
  verified-in-memory behavior). Nothing else changed.
- **K1** (`cold_segment_test`: `install_race`, `marker_race`, `facts_by_owner`, `facts_race`,
  `readers_and_owner`). Threads are held inside `map()` until all have arrived, so every round has
  N-1 losers: all see one segment pointer, N maps happen, one mapping stays. Mutants, each built
  and run by `.scratch/mutants.py` (in place, restored), all killed:
  - body cleared in place (M1): `install_race`, pointer changes across a commit.
  - body stored without compare-exchange (M2): `install_race`, threads hold different segments.
  - loser reads the body it destroyed (M3): `install_race`, the read through the destroyed body fails.
  - facts written by a reader's map (M4): `facts_by_owner` (a record's facts known before the owner
    learned them); and under TSan eight data-race reports, reader's `map_body` write against the
    owner's `prepare_commit` read (`facts_race`); the unmutated TSan run is clean.
  The marker race (a good body that loses to the marker throws `corrupt_segment`, nothing stays
  mapped) is `marker_race`. T6's 1,000 seeded inline-executor schedules are not built: the
  deterministic rendezvous plus the threaded `readers_and_owner` stand in for step 1.
- **K2** (`host_error_in_reads`, `damaged_first_map`, `commit_prepare`, `output_maps`,
  `scheduling`, `install_records`, `open_checks`; failures injected by a wrapper over faultfs:
  `open_existing`, `map()`, one flipped bit in a mapping). Mutants killed: output map fences (M5,
  uncaught `ENOMEM` out of `refresh()`), swallowed round not counted (M6), prepare skipped (M7,
  fence "facts are unknown"), host error marks the record (M8), `needs_heal()` ignores markers
  (M9), prepare inside `guarded()` (M10, the debug assertion), a damaged source not skipped
  (M11), a host error not dropping the round (M12), open's size check removed (M13), the merge
  output reopening the file instead of reading through the one it holds (M14, after the test
  armed `open_existing` too), lazy install mapping (M15). The architect's `output_map_probe`,
  compiled against this branch: `refresh()` and `force_merge(1)` with the output map failing
  `ENOMEM` throw nothing and the next apply and commit succeed (on main: "durability failure",
  then "the writer is fenced"). CT-R19's commit after `heal()` uses a transient damage
  (two damaged maps, then clean), as the re-check 2 nit asks. Debug assertions for "no input map
  inside `guarded()`" are live in the rel build.
- **K4** (the part item 1 touches): `open_checks`, missing and truncated segment file, lazy and
  eager, `corrupt_index` with the manifest's messages. The twelve damage cases wait for item 4.
- **Fuzz**: no decoder or parser was added or changed; `open_body_of` is `load_segment_of`'s
  checks moved, run on the same bytes at first map instead of open.

## K7: call sites that adopt `seg()`

`segment_view::seg` is now `seg()` (a pointer return, so `v.seg()->core()`). prospect pins strata at
`75d1e7e` (`CMakeLists.txt:9`) and builds `segment_snapshot{s.seg, s.deleted, s.dead}` from
`ix.segments()`: `tools/parity.cc:232`, `tools/prospect_engine.cc` (`:229`, `:429`, `:494`, `:600`,
`:701`), `test/engine_query_test.cc:70`, `test/churn_test.cc:100`, `test/multisegment_test.cc:156`,
`test/reorder_test.cc:119`, `test/merge_test.cc:91`, `test/postings_test.cc:696`, and tests reading
`s.seg->` on a view (`churn_test.cc:109-110,248`, `merge_test.cc:318`, `lifecycle_test.cc:880`,
`reorder_test.cc:248`). Each becomes `s.seg()`. Xapiand has no `segment_view` or
`segments()` site at its local master `dada66d7a`; the pin bump to the strata commit that lands
this adopts `seg()`. Not changed here (prospect is its own repository).

## Open

- Lazy segments stay off by default: `apply()` can throw from a first map mid-transaction until
  CT-R5's rollback lands (item 6).
- `merge_scratch::bytes()` (a merge's scratch file map) still fences on a host error from `map()`;
  D9's table does not list it, and it is not tested here.
- `written_segment` (unused, private) was removed with the record split.
- Perf: not measured (functional claims only). The hot read path gains one acquire load per
  `seg()`; the lead measures T1/T6 against main.

## Log

- 2026-10-27: branch from main strata:d0717a2; configured rel, asan and tsan builds under
  `.scratch/`; implemented items 1 and 2 with D9's table; sanitizer and mutant runs above.
