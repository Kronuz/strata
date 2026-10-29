# strata review: gate 7a (strata behind Xapiand's engine seam at rf = 1)

- Date: 2026-10-29.
- Reviewed: Xapiand master `Xapiand:3bc11f31f` (local, not pushed), pinning prospect `prospect:828cd4a`, which pins
  strata `strata:16e9054` (code-identical to strata main `strata:4a9f52e`; `git diff 16e9054 4a9f52e -- ':!docs'` is
  empty) and completion `completion:03cef1d` (the resolved `_deps` heads). The gate report
  `Xapiand:docs/gates/phase7a.md` (first run, Rerun, Closing run), `docs/perf/t11/README.md`,
  `docs/perf/t11-barriers/README.md`, `docs/status/gate7a-fixes.md`. `Xapiand:7125dfc28` (the closing run) differs from
  `3bc11f31f` only in the gate report; `Xapiand:8f6332a01` (the Rerun) differs in `src`, `tests`, `harness` and
  `CMakeLists.txt` only by the prospect pin.
- Raw results rescored: `Xapiand-wt/gate7a-close/.scratch/{h1,h2}` (closing, 20 runs) and
  `Xapiand/.scratch/gate7a-rerun/{t11,t11b}` (Rerun, 20 runs), `Xapiand/.scratch/gate7a-rerun/t6`.
- Checked against: `strata:docs/design/xapiand-on-strata.md` section 5 (T1 to T11), rulings R-P2 and R-RM (lines
  363-379), PLAN "Every gate" (`docs/PLAN.md:255-266`) and line 3; `key-field-security.md` (APPROVE),
  `commit-barriers-durability.md` (REJECT, then re-check: CB-1, CB-2, C1 closed), `Xapiand:docs/reviews/strata-fields-code.md`
  (APPROVE WITH CONDITIONS), `calibration.md`.
- Checked by: one Release build of the exact commit (Apple clang, LTO on, `-DBUILD_TESTS=ON`) in
  `Xapiand-wt/review-gate7a/build`; `ctest -j4`: 28 of 28 pass (25.5 s, `.scratch/ctest.log`);
  `harness/engine_e2e.sh` with newman 6.2.3: Xapian 566/566, strata 566/566, 335 requests, 0 regressions, 0 status
  differences (`.scratch/e2e/`); `bodydiff.py` 5 of 335; `search_order_diff.py` 92 of 92 identical;
  `harness/removed_features_check.sh` 10 of 10 ok; `harness/engine_migration_check.sh` PASS. Probes, all kept in
  `Xapiand-wt/review-gate7a/.scratch/`:
  - `t11-rescore/rescore.py`: reruns `profile_bulk.sh`'s own analysis (read-only) on every raw `sample.txt`, as is and
    with device-flush and lock waits treated as blocked; output `closing.txt`, `rerun.txt`.
  - `t11-rescore/drop_invalid.py` (`drop_invalid.txt`): wall samples per profile; the estimators without truncated
    profiles.
  - `t11-rescore/leak.py`, `leaves.py`: engine frames classified outside the engine rows; leaf frames under `commit_files`.
  - `probes/refusals.sh` (`refusals.txt`): the same requests on a Xapian and a strata node.
  - `probes/t9_versions.sh` (`t9_versions.txt`): which documents change `_version` in T9, on which engine.
  - `probes/metrics_names.py`: the `:metrics` series of both engines' E2E bodies.
  - Not run: benchmarks (no T11 timing; rescoring only), T8 (`engine_crash_check.sh`), T10, ASan/UBSan/TSan builds,
    fuzzing, coverage, Linux, FreeBSD, macOS arm64, power loss.

## Verdict

**REJECT: every measured row passes and reproduces, and T11 passes more honestly than the report says; two items
block: strata still refuses three user-reachable features against R-RM, and T10 and PLAN's every-gate checks are not
accounted for at this commit.**

What holds, and reproduces:

- T6 at the exact commit: 566 of 566 on both engines, 0 regressions, 0 HTTP status differences (`.scratch/e2e/diff.txt`).
- T7: the 5 body differences are the ones listed (`phase7a.md:52-58`): a generated `_id`, three 404 wordings, and
  `:metrics` counters. The two `:metrics` bodies carry the same 196 series on both engines (`metrics_names.py`), so only
  values differ. Hit order, scores, `total` and `count` identical on all 92 responses with hits.
- T9: PASS reproduces; D4 is real and on Xapian's side (F5), with a note.
- T11: the committed JSON is reproduced exactly from the raw samples (gate estimator 0.387x, sample count 0.375x,
  critical path 0.226x, whole-process CPU 0.673x, pairs 0.646 to 0.716). With device-flush waits removed from both
  engines the CPU row is 0.357x (pairs 0.325 to 0.401), and 0.414x on the Rerun's own raw data (F1). Strata's engine
  frames that the harness files under "Data record build" (host `finalise_write`, zstd) are symmetric with Xapian's
  (1123 against 1540 samples in one pair, `leak.py`), so the rows compare like with like.
- Unit tests: 28 of 28. The removed features answer as removed on both engines (ELITE_SET, `_terms` over text, INFO term
  lists).

What blocks: L1 (F3), L2 (F6).

## Findings

**F1 (condition). The T11 CPU estimator counts device-flush wait as CPU, which biases it against strata; the report
reads the bias backwards.** `profile_bulk.sh:108` treats only condition, kevent, select and sleep leaves as idle; a
thread blocked in `__fcntl(F_FULLFSYNC)` or `fsync` counts as busy, in the engine row and in the denominator. Under
`commit_files` the leaves are `__fcntl` 153, `fsync` 41, `__openat` 20, `pwrite` 16 of 233 samples (`leaves.py`, h1
strata_1). Xapian runs `DB_NO_SYNC`, so the inflation is strata's alone. Rescored with `__fcntl`/`fsync`/`fdatasync`
leaves as blocked (`rescore.py`, `nosync`):

| Run | Gate estimator, as committed | Flush wait excluded | Sample count, wait excluded |
|---|---|---|---|
| Rerun (`8f6332a01`) | 0.515x (FAIL) | 0.414x, pairs 0.398 to 0.442 | 0.347x |
| Closing (`7125dfc28`) | 0.387x | 0.357x, pairs 0.325 to 0.401 | 0.339x |

So the Rerun's FAIL was the estimator, not the engine: on-CPU, strata's engine share was already under 0.5 before the
commit barriers. The closing report says the opposite: "passes on the gate's estimator because it counts device-flush
wait as engine CPU" (`phase7a.md:251`). Counting the wait makes strata look worse; the row passes despite it. The
barrier change's real effect is the one `t11-barriers/README.md` states (17% less wall, flat CPU). The critical-path
row (0.226x) legitimately includes the wait and passes either way. Fix: correct `phase7a.md:243-252` in place; give
`profile_bulk.sh` a "blocked in a device flush" class (leaf `__fcntl`, `fsync`, `fdatasync`, excluded from busy and
reported as its own line), so the T11 CPU row measures CPU on every platform, and say so in the row's procedure.

**F2 (condition). A truncated profile is kept and called unaffected.** Hold 1's `xapian_1` profile has 1208 wall
samples where the other 19 have 5362 to 5835, at 10.1 busy cores (`drop_invalid.txt`): `sample(1)` caught about a
fifth of the load. Its engine share feeds the gate estimator, so "the gate's estimator uses the smoke CPU and is not
affected" (`phase7a.md:240-241`) is wrong; the run is invalid. Without it: 0.384x gate estimator, 0.354x sample count,
0.208x critical path: the verdict does not move. Fix: mark the run invalid in the report (as `t11-barriers/raw/INVALID.md`
does) and have `profile_bulk.sh` fail a profile whose wall samples fall below half the expected window.

**F3 (blocking). Three user-reachable features are still refused on strata, against R-RM.** R-RM (owner, 2026-10-28):
"No point in having features that are 'disabled' or 'refused'"; the fix round's rule was "removed, not refused ... no
refusals" (`docs/status/gate7a-fixes.md:7-9`). `probes/refusals.sh` on the exact commit:

| Request (`SEARCH /bank/` text query) | Xapian | strata |
|---|---|---|
| `?nearest` | 200, 178 hits | 400 "does not search similar documents yet" |
| `?fuzzy` | 200, 192 hits | 400, same |
| `?collapse=gender` | 200, 2 hits | 400 "does not search collapsed results yet" |

The refusals are `src/engine/strata/search.cc:656-664`. The docs suite has no example of these query parameters
(`git grep` of `docs/src/content`), so T6 cannot see them. Two smaller remnants of the same kind:
`percent_threshold`/`weight_threshold` are plumbed from `handler.cc:1104-1105` to both engines as constant 0, and strata
refuses a nonzero value (`search.cc:662-664`), a disabled feature with no way to enable it; `SegmentDocView::serialise`
exists only to throw (`src/engine/strata/doc_view.cc:35-39`; its only caller path is the Xapian-only remote protocol,
D11). Collapse reads a value slot, which strata keeps (P1), so it looks servable naturally; similar documents expand terms
from the relevant documents, which needs term lists (R-P2), so it looks like a removal. Both readings are the owner's to
confirm. Fix: per feature, serve it on strata or delete it from master on both engines (query parameters, request
fields, engine code, docs, tests), with the owner's ruling recorded where it is not obvious; delete the threshold
plumbing; extend `removed_features_check.sh` with each removed parameter.

**F4 (condition). The R-RM deletion removed the suite's only checks of what terms the schema generates, and left two
harnesses vacuous.** Between `9fa890ecc` and `3bc11f31f`, 193 removed lines in `docs/src/content/docs/tests` read
`.terms` (arrays, objects, data types, namespaces, `define-prefix`, `define-slot`, `uuid-field`), and nothing replaced
them: `define-prefix.md` no longer checks that a custom prefix yields `bar` terms `Sand Sbar Sfoo`, only that two values
exist. `harness/terms_diff.py:73-77` and `harness/vocab_diff.py:29-30` dump `INFO`'s `terms`, which is now absent on both
engines (`removed_features_check.sh`: "INFO has no terms"), so both write `null` for every document and any two builds
diff equal; `docs/dev/testing.md:115-125` still presents them as the text-analysis differential. Term generation is host
code shared by both engines, so this does not weaken T6's engine comparison; it removes the regression net for analysis
and schema term generation on both. Fix: before the next gate, replace each removed term assertion with a search that
needs the term (e.g. `SEARCH bar:foo` finds the document), and port `terms_diff.py`/`vocab_diff.py` to a term dump that
still exists (the `engine::Document` the census build sees, or the strata check tool), or delete them and their
`testing.md` section.

**F5 (note). T9's D4 is an engine divergence on the same input, with its cause untraced.** `t9_versions.sh`: the source
has 1000 distinct ids; the Xapian dump has `_version` 1 everywhere; restoring that same dump gives `_version` 2 on
Xapian for ids 386, 387, 388, 389, 390 (consecutive) and 1 on strata. So strata is right and Xapian's restore writes a
run of five documents twice, which is consistent with D4's direction (`phase7a.md:22`). "Pre-existing" is not proven: no
pre-seam binary was run. Five consecutive ids point at a batch replayed by a retry in the shared restore pipeline
(`handler.cc:868-905`, `DocIndexer`), which strata survives only because its version stamping differs. Fix (on the
Xapian side, per "fix bugs in replaced code"): find the double write, and run the same control on the alpha.9 binary to
settle attribution.

**F6 (blocking). T10 and the every-gate checks are not accounted for at the gate's commit.** The design's gate table
has T10, "concurrent index and search on one strata shard is clean under ThreadSanitizer ... for a stated time"
(`xapiand-on-strata.md:319`); the gate report never mentions it, run or deferred. PLAN's every-gate list
(`PLAN.md:257-266`) asks for the tests under ASan/UBSan and TSan, one CPU-hour of fuzzing per parser or decoder added in
the phase (the phase added `fuzz_text`, `fuzz_strata_document` and `fuzz_strata_pattern`; `fuzz_body` changed with SR3), coverage,
and a scale statement. The only sanitizer evidence is on branches: ASan/UBSan 26 of 28 on `phase7a/gate-fixes` with a
RapidJSON UBSan abort (`gate7a-fixes.md:39-43`), unit-level TSan at S2 (`strata-engine-search.md:152-155`); none at
`3bc11f31f`, and the calibration row of 2026-10-29 says a local macOS TSan pass is not evidence for struct-copy races.
The S2 code review's CR6 question (search holds every shard lock; T10 replaces it with a snapshot) is also open. Fix:
run T10 (a TSan build of this commit under a mixed index and search load, Linux CI TSan where the class needs it) and the
ASan/UBSan ctest at this commit, record the fuzz hours per target, or have the lead record each item as deferred with a
reason, an owner and the phase it lands in. A recorded deferral closes this item; silence does not.

**F7 (non-blocking, report accuracy).** `phase7a.md:73` says the 576-to-566 assertion change "was not traced"; it was:
the R-RM deletions removed them (`gate7a-fixes.md:10`, this review's F4). `phase7a.md:257` "not rerun here: T6, T7, T9
and the unit tests": this review reran all four at `3bc11f31f` and they pass; the report can cite this file.

## Blocking items

- L1 (F3): `?nearest`, `?fuzzy` and `?collapse` are served by the strata engine or deleted from master on both engines,
  with the owner's ruling recorded for each; the threshold plumbing is deleted; `removed_features_check.sh` covers every
  deleted parameter, and `refusals.sh` shows no "does not search ... yet" answer for a request Xapian serves.
- L2 (F6): T10 and each every-gate item (ASan/UBSan, TSan, fuzz hours per target, coverage) are either run at the gate's
  commit with results in `phase7a.md`, or listed there as deferred with reason, owner and phase.

## Conditions

- C1 (F1): `phase7a.md`'s closing residuals state that flush wait inflates strata's estimator and that the on-CPU row
  is 0.357x (Rerun 0.414x); `profile_bulk.sh` reports device-flush wait as its own blocked line, not as busy.
- C2 (F2): the truncated `xapian_1` run is marked invalid; the harness rejects a short profile.
- C3 (F4): term-generation assertions restored as searches; `terms_diff.py`/`vocab_diff.py` ported or deleted with
  `testing.md`.
- C4 (F5): the Xapian restore double write traced, with the alpha.9 control.
- C5: T11 holds on macOS x86_64 APFS only, and the barrier path relies on `F_BARRIERFSYNC`, which Apple guarantees only
  on Apple SSDs (`commit-barriers-durability.md` C1, closed as wording). Linux, FreeBSD and arm64 T11 rows are owed
  before 7b's gate, as the cross-platform rule requires; carried, not blocking 7a.

## Re-check scope

The rechecker (high, code) or the lead: L1 by rerunning `probes/refusals.sh` and `removed_features_check.sh` on the fix
commit; L2 by reading the gate report's new section and the TSan/ASan logs it cites. Conditions are checked at gate 7b.
