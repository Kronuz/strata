# Code review: cold-tier step 1, items 1 and 2

- Date: 2026-10-27.
- Branch: `phase/cold-tier-step1`, head strata:40fadb9 (merge of main strata:eb87572). `git rev-parse origin/phase/cold-tier-step1`
  equals the developer worktree's `HEAD`: no unpushed commit.
- Reviewed: `git diff eb87572...origin/phase/cold-tier-step1` (18 files, +1,331/-147; `include/strata/core/index.hh` +484,
  `test/cold_segment_test.cc` +703), against `docs/design/cold-tier.md` revision 2 and its review with both re-checks and the
  CT-R5 re-check (`design/cold-tier`), the ledger `docs/status/cold-tier-step1.md`, and `docs/mechanisms/segment-records.md`.
- Reviewer worktree: `~/code/Kronuz/strata-wt/review-step1` (detached at 40fadb9). Probes, harness, mutants and logs are in its
  `.scratch/` (listed at the end). Not committed, not pushed.

## Verdict

**APPROVE WITH CONDITIONS.** No blocker. The record/body split, the compare-exchange install, the marker, owner-only facts,
D9's map points and failure rules, `prepare_commit()`, `merges_skipped()` and D11's open check do what the design says, and every
probe I wrote against them passed under ASan+UBSan and TSan, with the CT-R5 rollback now on the branch. Two things stop it
being a plain approval: the test for the marker's second race is absent, so the compare-exchange in `fail()` (the line I1
rests on) is not pinned by any test (CR1); and the ledger ticks K1 on a substitute for T6 without saying the schedule model is owed
(CR4). Both are small and mechanical.

## Conditions (one fix round)

1. **CR1**: add the marker-loses-to-body case (probe PG, about 25 lines) to `cold_segment_test`, and show that mutant R1 dies.
2. **CR2**: add the lazy-install-then-marked-record commit case, and show that mutant R3 dies.
3. **CR3**: answer the merge-scratch fence in the design (a D9 row) and the ledger's "Open", with the one-line fix owed before
   step 2 sets any map cap; no code needed to land.
4. **CR4**: edit the ledger's K1 line to say T6's 1,000-schedule model is owed to the gate and that the rendezvous tests stand in.

Everything else is a non-blocking note or a follow-up.

## Findings

### CR1. The marker's compare-exchange in `fail()` is not pinned by any test (medium; condition)

- `index.hh:2272-2277` (`open_segment::fail`): `compare_exchange_strong(expected, failed_marker(), ...) || expected == failed_marker()`,
  else return the body that won. This is the one line that keeps I1 when a failing reader races a good one: a reader whose two
  attempts both fail must not overwrite a body another reader installed.
- The ledger says `marker_race` covers "a good body that loses to the marker". That is the other direction (`map_body`'s
  compare-exchange against a marker). The direction where the *marker* loses to a body has no test.
- Mutant R1 (`.scratch/mutants_rev.py`): `fail()` stores the marker unconditionally. `cold_segment_test` under ASan with
  `detect_leaks=1`: **SURVIVED**. The whole 55-test suite never reaches it either.
- Probe PG (`.scratch/probes/probe_lazy.cc`, `pg_marker_loses_to_body`): thread X holds a damaged first map (the hook's `arm_hold`),
  thread Y maps clean and installs, then X's second attempt is damaged. Unmutated: X returns Y's body, the record stays bodied and
  not failed, `needs_heal()` false, one mapping lives; 5 of 5 PASS under ASan and TSan. With R1 compiled in
  (`probe_lazy_asan_R1`): ASan reports `3458 byte(s) leaked in 17 allocation(s)`, the record is marked failed over a live body
  (the body leaks and `bodied()` goes false). So the probe kills the mutant and the test suite does not.
- Fix: put PG in `cold_segment_test.cc` (it uses only the existing `hooks`: `arm_hold`, `wait_held`, `damage_left`, `release`) and
  add R1 to `mutants.py`.

### CR2. A marked record whose facts are known is not tested at commit (low; condition)

- `index.hh:4220-4230` (`prepare_commit`; the arm is `:4225`): `else if (s->deletes_dirty || s->failed()) { (void)s->seg(); }`. The `failed()` arm is what
  makes "a marked record throws on every commit until `heal()`" (CT-R19) true for a record whose facts are already known.
  A record has facts and no body only after a lazy `install()` (`index.hh:1471-1480`, the lazy branch of `install()`).
- Mutant R3 (drop `|| s->failed()`): **SURVIVED** `cold_segment_test`. `commit_prepare` marks a record whose facts are unknown, so it goes
  through `learn_facts()` and never reaches this arm.
- Fix: one case: lazy `install()` into a target, damage the first map of one copy, `commit()` throws `corrupt_segment`, `heal()`, commit
  succeeds. Add R3 to the mutants.

### CR3. `merge_scratch::bytes()` still fences on a host `map()` error: not a blocker (medium for step 2, none for landing)

The developer left this open ("Open", ledger). I ruled on it.
- `index.hh:2900-2905` (`:2902`): `mapped_ = f_->map()` is a map of a file the merge just wrote (an output map in D9's sense), unguarded and with
  `ctx_.io->open_existing(name_)` as its fallback. A host error (`ENOMEM` at `vm.max_map_count`, `EMFILE`) escapes `run_merge`'s try into
  `job.error`, and publication fences the index: the failure D9 removes for the segment outputs.
- It is **not** a blocker for this branch:
  1. It is on `main` today, unchanged by the branch.
  2. It is unreachable in strata's own layout: `index.hh:2950-2961` (`by_key` at `:2952`) builds a scratch only when `!by_key`
     (`segment_policy<Layout>::key_order` false), and the kv layout is key-ordered. It fires only for prospect's document layout with a
     reordering merge or an out-of-docid-order source.
  3. `eager_segments` is the default, so nothing about the host's map pressure changes until step 2.
  4. D9's table does not list it, and K2 ticks D9's table (it is complete against that table; the table is what is short).
- It must be closed before step 2, whose caps put the process near `vm.max_map_count` on purpose, and T5's "faultfs `map()` failures
  on merge outputs" cannot be claimed for prospect's layout until it is. Fix: `map_output(*f_)` and `read_all(*f_)` through the file it
  holds (the two lines `streamed_segment` already uses). For the architect: add the row to D9 ("merge scratch: output, keeps the
  bytes"). I could not probe it from strata: the kv layout never builds a scratch (the check is in `run_merge`); a probe needs
  prospect's layout with `reordering` on.

### CR4. K1 is ticked with a substitute for T6, and the ledger does not owe the rest (low; condition, ledger text only)

- K1 asks for "T6 over the install under TSan" (1,000 seeded schedules on `inline_executor`, the shared-reader T2a shape). The
  ledger: "T6's 1,000 seeded inline-executor schedules are not built: the deterministic rendezvous plus the threaded
  `readers_and_owner` stand in for step 1", and ticks `[x]`.
- The substitute is sound for the four named mutants (M1 to M4 die; I spot-ran M2 and M12, below). But the tick hides a gate input:
  edit K1's line to "[x] with T6's schedule model owed to step 2's gate", so the lead's T5/T6 list is accurate.

### CR5. CI never runs the lazy suite, and the library reads an environment variable (low; follow-up before item 6)

- `.github/workflows/ci.yml` has no `STRATA_EAGER_SEGMENTS=0` job (`grep` finds the name nowhere in `.github`, `CMakeLists.txt`
  or `test/kv_test.hh`). CI is green on all eight jobs at 40fadb9 (`gh run 40569849826`: linux-asan-ubsan, linux-x86_64-clang,
  linux-tsan, linux-arm64-gcc, macos-x86_64, macos-arm64, freebsd-x86_64, linux-x86_64-gcc), but those run the eager default plus
  `cold_segment_test`. The lazy suite exists only in the developer's and this review's local runs (55 of 55 each, below).
- Item 6 flips the default; the lazy ASan and TSan jobs must exist first. `eager_segments_default()` (`index.hh:350`) makes a
  production process's behaviour depend on `getenv`; it should leave the header when item 6 lands (tests then set the option).

### CR6. A scan maps every segment to test the key range (low; architect)

- `index.hh:1990` (`scan_segments`): `sv.seg()->get<keys_plugin>()` for every segment, then `k.max_key() < from` /
  `k.min_key() >= *to` to skip. D9's table and prose say "a scan maps every segment whose key range it reads (`:1918`)"; the range
  lives in the body, so a narrow scan of a 10-segment warm index maps all ten. Behaviour is correct, and T2 will show the cost. If
  min and max key were facts on the record (owner-written, 2 strings), a narrow scan would map only overlapping segments. Design
  question for step 2, not a defect here.

### CR7. The debug "no input map" assertion covers `guarded()` only (nit)

- `index.hh:2280` message says "inside guarded() or a chore" and D9 says "Debug builds assert both" (destructor/`noexcept` chore as
  well). Only `guarded()` sets `no_input_map_scope` (`index.hh:2458`). I traced `run_chores`, `~hold`/`settle_storage`, `free_views` and the
  destructors: none reaches `seg()` today, so there is no live defect; the instrumentation is narrower than its message.

### CR8. Promote the apply-midway and prepare probes into the suite before item 6 (follow-up)

- The branch has no test of `apply()` meeting a host error at a first map mid-transaction (D9 row "apply() stage 1: rolled back, to the
  host"), the one the review asked to be checked now that CT-R5 is on the branch. The lazy ctest run exercises `apply_atomic_test`'s
  corrupt-section case only.
- Probe PB (`pb_apply_midway`): a five-op transaction (add, replace, erase, add, set_meta) on a 5-segment lazy index, a map-error burst
  (`ENOMEM` and `EMFILE`; two consecutive failures, because `read_checked` retries once) injected at the Nth map for N = 0..8, with and
  without a live reader. 36 cases, 20 of which threw out of `apply()`; in every one the index equalled a twin that never saw the
  transaction (contents, digest, metadata), and the retry and a commit converged to the twin's state. ASan+UBSan and TSan clean.
- Probes PC and PC2: a host error in commit's prepare (keyless documents, so the prepare is the first map): nothing durable, nothing
  fenced, generation unchanged, a second write and a commit succeed, reopen equals. After the failed commit the reader's published view is
  still the pre-flush one (4 segments against the owner's 5; after `refresh()` it is 5, as the design says, "unpublished until the next
  `publish_point()`") and a keyed write made afterwards is visible to a live reader exactly as after `refresh()` (PC2). So "as `refresh()`
  leaves it" holds except that `refresh()` publishes at its end and a failed prepare does not; the design says so.

### CR9. IO thread-safety is assumed, not stated (note for step 2)

- Readers now call `ctx->io->open_existing` (`index.hh:2283`) from any host thread beside the owner's `create_exclusive`/`replace`/`remove`.
  completion's `IO` (`completion/io.h:135`) states no concurrency contract; `PosixIO` (`openat` on the directory descriptor) and faultfs are
  safe in practice and TSan finds nothing. The catalog (step 2) should state the contract it relies on.

## Checks asked for

1. **Builds.** Two separate trees, each with its own dependency builds (`FETCHCONTENT_SOURCE_DIR_*` pointed at the developer's
   read-only pinned checkouts, whose commits I compared with `CMakeLists.txt`: 11 of 11 equal; the libraries' objects are built
   in the review's own `_deps`): `.scratch/asan` (Debug `-O1`, `-fsanitize=address,undefined -fno-sanitize-recover=undefined`,
   libc++ extensive hardening, `-fsanitize` on C too) and `.scratch/tsan` (RelWithDebInfo `-O1 -g -fsanitize=thread`).
2. **Suite results.**
   - ASan+UBSan, `detect_leaks=1`: eager 55 of 55, lazy (`STRATA_EAGER_SEGMENTS=0`) 55 of 55.
   - TSan, `halt_on_error=1`: eager 54 of 55 and lazy 54 of 55 at `ctest --timeout 900`; the one miss each time is `format14`, a timeout
     (the developer's TSan run shows it at 996 s; this host ran at load 8 to 11). Rerun alone with `--timeout 3000`: eager passes in
     1,114 s and lazy in 1,111 s. So TSan is 55 of 55 in both modes, zero reports.
3. **Install race and marker lifetime against I1.** Read: `map_body` (`index.hh:2279-2293`) installs by compare-exchange from null (acq_rel /
   acquire), the loser's `unique_ptr` destroys only its own body; `fail()` as in CR1; `~open_segment` frees a body, never the static marker
   (mutant R5, which deletes the marker, is killed by ASan); `seg()` takes an acquire load. Probes: PG (above); PE.
4. **Readers against the owner on facts.** Facts are plain fields written only by `adopt()` (before a record is shared) and
   `learn_facts()` (owner, `prepare_commit`); readers read none. TSan on the branch's `facts_race` and on PE found nothing; M4 (a
   reader's map writes them) is reported by TSan, per the developer's log.
5. **PE: threads and faults together.** `pe_stress`: for 8 cycles per round, a lazy reopen, 4 reader threads (get and a full scan), an
   owner doing replace/commit/heal with background merges, map-error bursts, `EMFILE` on open, and a permanently damaged segment for
   half of each cycle then transient. ASan: 8 rounds, corrupt_segment thrown 2,957 times, markers present at the end of 16 cycles,
   111 heals, 0 wrong reads, 0 unexpected exceptions, every cycle reopened eager to the owner's digest. TSan: 4 rounds, 2,141
   corrupt_segment, 11 markers, 44 heals, same zeros.
6. **Reader against `reclaim()`** (PA): a guard holds a view of unmapped records; the owner erases a segment's every document, commits
   and reclaims four times at `reclaim_step` 4,096; the guard then maps the dropped segment's file for the first time and reads it. Passes
   (ASan, TSan). Reason, read at `retain()` (`index.hh:4384-4386`): a view's files are referenced by segment id, whatever its bodies.
7. **`close()` with a guard held** (PD): a guard reads lazily after `close()`; `try_finish_close()` completes after the guard goes.
8. **Fallback open with prepare inside open** (PH): a lazy fallback open succeeds, and with `EMFILE` on open or a map burst it fails with
   the host error and the directory is intact (an eager fallback open afterwards works). ASan and TSan.
9. **Mutants.** The developer's `.scratch/mutants.py` runs 15 mutants of `index.hh` in place (restored in `finally`), each rebuilt and run
   against `cold_segment_test` only; the logs (`mutants-rel.log`, `mutants-tsan*.log`, `mutants-m14.log`) show M1 to M15 killed, with M14 killed
   only after the test armed `open_existing` too. All four K1 mutants are present. I spot-ran **M2** (store without compare-exchange: killed,
   `install_race` line 273) and **M12** (host error not dropping the round: killed, lines 591 to 600) on the review's ASan tree: both killed.
   Eight mutants of my own (`.scratch/mutants_rev.py`): R5 and R8 killed; **R1 and R3 survive and are real gaps (CR1, CR2)**; R2 (a lost fail
   still throws), R4 (no `dead == doc_count` skip in the prepare), R6 (scheduling does not set `damaged` beside the marker) and R7 (heal's
   fresh record loses `facts_known`, the prepare relearns) survive as equivalents: same observable behaviour.
10. **K-criteria against evidence.** K1: components present, T6 substitute (CR4). K2: every D9 row read against the code; the tests cover
    open, map, and check failures at reads, scheduling, prepare, outputs and install (`host_error_in_reads`, `damaged_first_map`,
    `commit_prepare`, `output_maps`, `scheduling`, `install_records`); gaps are CR2, CR3 (not a table row), CR8. K4 (part): `open_checks`, missing and
    resized files, lazy and eager, with the manifest's messages. K7: the listing is accurate to the sites I grepped in this repository (prospect is
    not here). K8: CI green on eight jobs at the reviewed commit; local sanitizer runs above; the lazy suite is not in CI (CR5).

## Calibration checks applied

- Mechanisms twice: no thread, queue, cache or encoder is added; `load_context`, a thread-local depth counter and the atomic counters are
  the only new state. None duplicates a Kronuz library.
- CI on every platform for the reviewed commit: done above (eight jobs green at strata:40fadb9; no lazy job).
- Baselines: the branch cites no measured baseline ("Numbers: none"; the ledger says functional claims only).
- Unpushed commits: `HEAD` equals `origin/phase/cold-tier-step1`.
- Limits: none added.
- Mechanisms a fix relies on: CT-R5's rollback is in the branch (the merge brought `apply()`'s undo log); PB exercises it.

## Non-blocking notes

- `heal()` does not map a record nobody has read (`index.hh:940`), so a segment damaged on disk and never read is not reported; this is D9's
  "bodied and failed records only", stated in the comment and the mechanism doc.
- The `scheduling` branch that skips a corrupt source can leave a one-source "merge" when two were asked; it rewrites that segment. Same shape
  as the existing `s->damaged` skip.
- `segment-records.md` and the ledger are accurate to the code I read; the ledger counts 54 tests and the tree now has 55 (the merge brought
  `apply_atomic`).
- Hot path: `seg()` adds one acquire load per call and `segment_view` now holds a record pointer; not measured here (the lead's T1/T6).

## Probes, harness, mutants (in `~/code/Kronuz/strata-wt/review-step1/.scratch/`)

- `cfg.sh asan|tsan`, `build.sh`, `runctest.sh asan|tsan eager|lazy`, `f14.sh`, `buildprobe.sh`: configure and build with separate
  dependency builds, run the suites, build a probe against a flavour's flags.
- `probes/wrap.hh` (the developer's faultfs wrapper plus `fail_map_after`, `fail_burst`), `probes/probe_lazy.cc` (scenarios `pa`, `pb`, `pc`, `pc2`,
  `pd`, `pe N`, `pg`, `ph`), `probes/probe_lazy_asan`, `probes/probe_lazy_tsan`, `probes/probe_lazy_asan_R1` (R1 compiled in).
- `mutants_rev.py BUILD_DIR [names]` (R1 to R8; run from the worktree root; the file is restored).
- Logs: `ctest-asan-{eager,lazy}.log`, `ctest-tsan-{eager,lazy}.log`, `f14-tsan-*.log`, `*.build.log`.

Not run: Linux, FreeBSD and arm64 locally (CI green on them, eager only); the 1,000-schedule T6 model (not built by the branch); benchmarks and
the T1 rows (the lead's); prospect's and Xapiand's call sites (K7 is a listing); the twelve damage cases (K4, item 4, not on this branch);
a probe of `merge_scratch` (needs prospect's layout); `demote()` (item 5, not on this branch).
