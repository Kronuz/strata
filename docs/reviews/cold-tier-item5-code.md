# Cold-tier step 1, item 5: code review

> Reviewer: code-reviewer (fresh context), 2026-11-01. Branch `phase/cold-tier-item5`, code
> strata:3918fdc, ledger and note strata:f88a3af and strata:cf9f834, against `design/cold-tier` at
> strata:72a77f3 (D3, D9, D10, I8, T5, T6, K5; CT-R22, CT-R25, CT-R26).

## Verdict: APPROVE TO LAND

No blocker. Five non-blocking notes (I5-N1 to I5-N5), none needs a fix round before landing.

## What I did

I read the whole `index.hh` diff (`demote`, `promote`, `promote_owed`, `turn_guard`, the `apply`
check, `twin()`, `map_body` counters, `views_held` by id, `close()`) against the design text, and
read `retain()`, `publish_point()`, `build_view()`, `heal()`, `install()`, `run_chores()` and
`writer_scope` for the paths that touch `segs_` or the flag. I wrote five probes (below) and ran them
under ASan+UBSan (asserts live) and TSan (three runs). I did not redo the developer's proof.

Probes, all kept in `.scratch/review-item5/` (`probe_test.cc`, `build.sh rel|asan|tsan`, binaries
`probe_asan`, `probe_tsan`): ASan+UBSan 0 failures, 0 reports; TSan 3 of 3 runs 0 failures, 0 reports.

| Probe | Question | Result |
|---|---|---|
| p1_persistent_error | `promote()` failing with EMFILE five times: flag, bit, next answer, then recovery | Flag stays set, bit stays set, each apply answers `map_owed` with no device call, `demote()` refused; once the error clears `promote()` completes and the same transaction is accepted. Consistent. |
| p2_skipped_clears_bit | An older (skipped) position applied between `map_owed` and the retry | Bit cleared, flag still set; after `promote()` a `demote()` is accepted and the retry answers `map_owed` a second time. See I5-N1. |
| p3_heal_after_demote | `heal()` after a damaged twin on a demoted defer_io index; partial `promote()` (EMFILE), `demote()` again, `promote()` | `heal()` replaces the marked record with a bodied one; `needs_heal()` false; the apply is accepted with `calls()` unchanged; the second demote after a partial promote and the final promote leave every record bodied. |
| p4_lazy_path | `eager_segments` off, `defer_io` off, merges on: demote before each of six applies with commits and merges, reopen, compare to a never-demoted twin | Same contents (`r4,r5,r2,r3`). The lazy path maps inside `apply` as designed, no assertion. |
| p5_threads | A reader thread acquiring views and mapping every record in a loop while the owner runs 200 rounds of promote, apply, commit, demote, reclaim with guards held across rounds; then `close()` and reopen with `defer_io` | No race report (TSan), no ASan report; reopened index has the flag clear. |

## The five questions

1. **The owed bit never cleared.** I found no path where it stays set with no way out other than
   the host's own contract.
   - The bit is set at one site (`index.hh:717`, the `map_owed` answer) and cleared by `turn_guard`'s
     destructor on every apply that does not answer an owed verdict, a throw included (`index.hh:2691-2702`),
     and by `close()` (`index.hh:652`). A reopened index is a new object: the flag and the bit live
     in memory only, and a defer_io open maps everything (`index.hh:594`, `4780`).
   - A failed `promote()` (EMFILE, ENOMEM): flag set, bit set, no fence, p1. A damaged segment is
     not a failure of `promote()`: the marker is taken, the `corrupt_segment` is caught
     (`index.hh:1089-1091`) and the record counts as done, so the flag clears and the apply meets
     the marker (ledger test `answer_ends`, my p3).
   - `install()` replacing `segs_` with the flag set leaves a flag that the next `promote()` clears
     with nothing to map (`index.hh:1620-1640`); harmless.
   - What remains is two host-contract cases, I5-N1 and I5-N2.
2. **`promote()` failure.** Flag and records are consistent: `promote_owed_` is stored false only
   after the loop (`index.hh:1093`), the maps made stay (a body installs once and is never
   cleared), nothing is fenced, nothing is published. The next `apply` answers `map_owed` from the
   flag alone, with `calls()` 0 (p1; ledger `answer_ends` case ends-c). `needs_heal` is honored:
   a marked record is skipped by `bodied()` false then `seg()` throwing `corrupt_segment`, the
   flag clears, `needs_heal()` reports it, `heal()` replaces it with a bodied record (p3), and the
   assertion at `index.hh:722` accepts `failed()`.
3. **Concurrency.**
   - `demote()` against a reader holding views: the twin is a new record, the old records stay in
     the views' `keep_`, a late map installs on the old record (`twin_every_record` shows it), and
     retention keeps the file while any view names it (`retain()`, the `for_each_view` loop at
     `index.hh:4700`). `twin()` writes facts only on the owner's side (`learn_facts()` on a bodied
     record) and no reader reads `structures`, `version`, `total_length` or `facts_known`
     (grep: only `commit` and `install`, owner-side). The bits are shared copy-on-write exactly as
     `build_view()` already shares them. p5 under TSan.
   - `promote()` against a merge publication or retention: both are under the lease, so they
     serialise; a merge worker's map of a source races `promote()`'s only through the install
     compare-exchange (`index.hh:2467`), counted in `map_losses()`. `run_chores()` schedules no
     merge, so `demote()`'s own scope exit cannot re-map what it just dropped.
   - `promote_owed()`: an atomic acquire load of a flag stored with release; a stale `false` on
     another thread is harmless (the next `apply` answers `map_owed` anyway) and a stale `true`
     costs one `promote()` that returns at once.
4. **`defer_io` off.** The default path differs by one plain `bool` store per `apply`
   (`turn_guard` constructor/destructor with `map_asked_` false), the `close()` reset, and in
   `map_body` one thread-local read plus a relaxed `fetch_add` on the lost-race path. `map_owed`
   and the assertion are both gated on `opts_.defer_io`. `demote()` without `defer_io` does not set
   the flag, so the next `apply` maps inside it through the lazy path, which p4 compares with a
   never-demoted index (identical contents, assertions live). The ledger's `default_path_unchanged`
   and the 61 of 61 under `STRATA_EAGER_SEGMENTS=0` cover the rest.
5. **Crash cuts.** `demote()` makes no device call and `promote()` makes no mutation (the ledger
   asserts both on every run of its 66-point `crash_points`), so no cut exists between `demote()`
   and its publication or between `promote()`'s maps and the flag clear that a recovery could
   see: both are in-memory states, and recovery opens from the last manifest with the flag clear.
   The one in-process failure I could construct, `publish_view` throwing `bad_alloc` inside
   `demote()`, leaves the twins in place, the flag set and the publication stale; every later
   call is consistent (see I5-N4).

## Other checks

- `twin()` copies every owner-written field of `open_segment` (`ref`, `deleted` shared, `dead`,
  `deletes_dirty`, `damaged`, facts, `claim`, the failed marker); `unsynced` is null because the
  predicate refuses unsynced records. `ctx` is shared, so retries and counters stay one set.
- `views_held()` by segment id is right for `heal()`'s replaced record and for twins.
- The `undo_` log indexes `segs_` by position, which a twin swap preserves.
- The ledger's deviation (the bit not cleared by `refresh()` or `commit()`) is the design's own
  text at `docs/design/cold-tier.md:152-156`, not a deviation from it; the ledger's "refines
  CT-R22's wording" is accurate against the review's wording only.
- `cold_segment_test` after the move of its IO wrapper to `cold_hooks.hh`: same 16 cases, 111 plus 1
  `CHECK`s against 112 (the one in the header is `fill`).

## Non-blocking notes

- **I5-N1 (design nuance, no fix).** A `skipped` apply ends the turn: `turn_guard` clears the bit on
  any outcome but an owed one, and `skipped` is returned before the `map_owed` check
  (`index.hh:711-713`). Probe p2: `map_owed`, then an older position (skipped), then `promote()`,
  then `demote()` accepted, then the same transaction answers `map_owed` again. The design's text
  says exactly this ("any apply that returns something other than ... clears it") and the host
  contract forbids applying a different position inside a turn, so only a host that replays old
  positions mid-turn can see it. Step 2's loop need not guard against it; if the architect wants the
  bound unconditional, `skipped` would set `turn.owed` when the bit is already set (one line).
- **I5-N2 (step 2).** A persistent non-corruption host error in `promote()` (the file removed
  under the index, a descriptor limit that never lifts) leaves the index answering `map_owed`
  forever and `demote()` returning false with no way to tell why (p1). That is D9's "the host's
  error, unchanged", so it is the host's policy; step 2's catalog should bound its retries and close
  the index. An accessor for the bit would let the host tell "refused for the turn" from "refused
  for a record".
- **I5-N3 (counter, step 2).** `unleased_maps()` tests `owner_thread().leases == 0`
  (`index.hh:2457`), a per-thread count over all indexes (`owner.hh:34`). A thread inside index A's
  lease that maps a record of index B is not counted. CT-R23's gate ("assert zero") is therefore
  weaker than its text on a host that reads one index from inside another's chore. Step 2 should
  count against the index's own lease (`lease_.held()`), or state the limit.
- **I5-N4 (exception safety).** `demote()` swaps `segs_` and sets the flag before `publish_point()`
  (`index.hh:1066-1070`). If the publication throws, the call throws with the index warm in the
  owner's view and mapped in the readers' (the published view keeps the old records until the next
  publication). Consistent and self-healing at the next `publish_point()`, but the caller cannot
  assume "threw means unchanged". Worth a line in the note.
- **I5-N5 (evidence).** The local TSan build line is `-O1 -g -fsanitize=thread ... -O2 -g -DNDEBUG`
  (`.scratch/tsan/CMakeFiles/warm_test.dir/flags.make`), so the assertions (the `map_owed`
  assertion, `no_input_map`) did not run under TSan, unlike the ledger's "asserts live" for the
  release and ASan builds. Not a defect in the code; the calibration table (2026-10-29) says Linux
  CI TSan is the authority for struct-copy races, and item 5 adds none I can find. Check Linux CI
  TSan is green before the merge. Lease-less owner reads (`get`, `scan`) on one thread against a
  `demote()` on another are the pre-existing contract and not guarded by the chore-bit check; the
  note could say so.

## Design questions for the architect

None that block. I5-N1 is the one place a stricter bound is a one-line change; I5-N3 belongs to
CT-R23 at step 2.

## Not run

- Linux and FreeBSD (the developer's preflight covers them; I did not rerun).
- The developer's 30 mutants and the 66-point crash cut (not redone, per the brief).
- Linux TSan (calibration 2026-10-29's authority for struct-copy races); my TSan was macOS x86_64.
- Release build with `-DNDEBUG` of the probes (the production configuration, where the
  `apply` assertion is absent).
- Benchmarks (none permitted).
