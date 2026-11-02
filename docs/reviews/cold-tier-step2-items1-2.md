# Code review: cold tier step 2, items 1 and 2

Reviewer: code reviewer (fresh context). Date 2026-11-02.
Branches: `phase/cold-tier-step2-item1` strata:456d8e4; `phase/cold-tier-step2-item2` strata:8f0b47d.
Design: `design/cold-tier` revision 6 (D3, D6, D10; S1 to S3). Not re-run: the developers' mutants, ASan,
UBSan, TSan and preflight, as asked.

## Verdicts

| Item | Verdict |
|---|---|
| 1. Index prerequisites (S1, S2) | **APPROVE WITH CONDITIONS** (S2-B1) |
| 2. Counted IO and caps (S3) | **APPROVE TO LAND**, with S2-N1 to N5 as follow-ups; S2-N1 is for the architect before item 3 uses the cap |
| Together | They compose. Land item 1 after S2-B1; item 2 may land first or second. |

## Blockers

### S2-B1 (item 1): moving an index while its lease is held leaves the shared holder word held forever

`owner.hh` (c2a6580): the moved-to `write_lease` binds the shared word in `load_context`, and the
moved-from lease rebinds to its own `own_`. A `lease_scope` or `chore_scope` taken before the move keeps
a pointer to the *old* lease object, so its release (`owner.hh` `release()`, `~chore_scope`) now stores 0
into the old lease's `own_`, not into the shared word the scope acquired. The shared word keeps the moving
thread's token for the life of the index. Before this change the moved lease started free and the old
scope released the old word; the class comment (`owner.hh:73-75`, "A moved lease starts free, as the
moving holder still holds the old one until its scope ends") and the move constructor's own check
(`h == lease_token()` is allowed) describe exactly this supported case, and no test covers it
(`unleased_threads_and_moves` moves with no scope open).

Probe `.scratch/review-step2/probes/lease_probe.cc`, built in `.scratch/review-step2/b-asan`:

- `lease_probe 1`: `lease_scope` on `a`, `m.emplace(std::move(a))`, scope ends:
  `prereq_lease(m).held_here()` is 1 on the owning thread (expected 0).
- `lease_probe 2`: the same with `chore_scope`: held_here 1. A chore-bit word makes every other thread's
  `lease_scope` park in `holder().wait(h)` (`owner.hh`, `lease_scope`) for good; I read this, did not run it.
- `lease_probe 4`: after the move-under-lease, another thread calls `m->commit()`:
  `Assertion failed ... owner_break owner.hh:55`. Same result on a `-DSTRATA_LEASE_CHECKS=0` build
  (`b-nochk`; `owner_break` is not gated by the flag). In a build where `owner_break` is a no-op the
  second writer simply runs unleased, and `unleased_maps()` is wrong for it.

Fix (small): have `lease_scope` and `chore_scope` remember the `word*` they acquired (not the lease), so a
scope releases the word it took whatever the lease later rebinds to; add a test that moves under a
`lease_scope` and a `chore_scope` and then takes the lease on another thread. Update the class comment.
Condition for landing item 1: that fix, the test, and `lease_probe 1`, `2`, `4` rerun clean.

## Item 1: what I probed

1. **Holder word lifetime and atomics against a record outliving its index.** `load_context` is made once
   per index (`index.hh:1797`, the only `make_shared<load_context>`), records hold it by `shared_ptr`
   (`open_segment::ctx`), and `map_body` reads `ctx->lease` through the word alone, never through the
   `write_lease`. A reader on another thread therefore never touches `w_`, the one non-atomic pointer the
   move rewrites. Held. The UAF the developer found is the only lifetime hazard I could construct.
2. **Moving a lease while another thread reads the word.** `lease_probe 3` under TSan
   (`b-tsan`): 4 reader threads mapping demoted records through `reader().acquire()` while the owner
   moves the index 300 times and demotes: zero reports, `unleased_maps()` rose to 169. Clean.
3. **Two indexes sharing a `load_context`.** Not reachable: one `make_shared` per index, merge outputs
   and `load_files` use the owning index's own `load_`. Nothing to fix; no test needed.
4. **S1 under `STRATA_LEASE_CHECKS=0` and `=1`.** `index_prereq_test` passes on `b-nochk` (checks off) and
   on the merged ASan and TSan builds (checks on). `lease_scope`/`chore_scope` update the word in both.
5. `close_ready()` clauses match D10 (warm predicate, nothing uncommitted, collect, no posted delete,
   empty queue, no debt); `collect_deletes()` in a predicate is documented as the one side effect.

## Item 2: what I probed

Probe `.scratch/review-step2/probes/budget_probe.cc` (`budget_probe N`, ASan build).

1. **Reservation leaks on paths the model skips.** Read every owner in `counted_io.hh`: each reservation
   is a local RAII object until it is moved into the file, mapping, owner, cursor or IO, and the moves
   are noexcept. An exception between `take()` and ownership (the `make_shared<resource_reservation>`
   argument, `make_unique<file>`, `make_shared<mapping>`, `new counted_io`) unwinds the locals; the inner
   kernel object is owned by a `unique_ptr`/`shared_ptr` that also unwinds. `PosixIO`'s constructor
   failing after the directory reservation: `budget_probe 5` (no such directory) and `budget_probe 6` (a
   real `EMFILE` from `setrlimit(RLIMIT_NOFILE, 3)`): both throw and `used(D)` and `used(V)` are 0
   afterwards. No leak found.
2. **External cap recovery oscillation.** It follows the design: after 60 s of quiet, every `tick()`
   raises the cap by an eighth of the gap (`budget_probe 4`: from 100 to cleared in 42 ticks after the
   quiet period). So the cap climbs back within one to two minutes and the next `EMFILE` lowers it
   again: a sawtooth with a period of about 60 s plus the climb, one kernel failure (a user-visible error
   for each concurrent in-flight call) per cycle while the external user stays. It does not oscillate
   within a cycle: the re-derivation never overwrites the lowered value, and a higher later failure does
   not raise it (both tested). See S2-N1 and S2-N4.
3. **Allocation tests skipped under TSan.** `allocation_failures()` and the `alloc_failures > 0` check
   are compiled out (`counted_io_test.cc:837`, `STRATA_NO_ALLOC_HOOK`). `stress()` still runs under TSan
   with kernel failures injected, and runs with allocation failures under ASan (merged build, passes).
   What TSan never sees is a concurrent `bad_alloc` unwinding through `open()`/`map()`. The unwinding
   code is the same single-threaded RAII plus relaxed atomic add/sub, so I rate the residual risk low
   (S2-N5). The 21 mutants and ASan stand in for it.
4. `reserve()` CAS loop, hard-cap arithmetic, `before_`, move semantics, close ordering
   (`close_inner`, vnode shared with mappings, member order so the descriptor closes before its
   reservation returns) read correct. The base `IO` virtuals not overridden (`make_closed_file`,
   `*_into`, `scan_directory_into`) throw `logic_error`, so no uncounted open can come through them.

## Together

- `git merge-tree` of each branch with `main` (dc4436d) is clean. Merging the two branches (scratch worktree
  `.scratch/review-step2/merged`, merge staged, not committed) conflicts only in `CMakeLists.txt` (two
  adjacent added test blocks: keep both) and `docs/status/cold-tier-step2.md` (both branches add it:
  the lead merges the two ledgers by hand). No code conflict; `index.hh`/`owner.hh` (item 1) and the new
  headers (item 2) do not overlap.
- Merged tree, ASan+UBSan RelWithDebInfo (`b-asan`, `-O1 -fno-sanitize-recover=undefined
  -D_GLIBCXX_ASSERTIONS`), full suite: **64 of 64 passed** (`ctest-asan.log`). TSan on the merged tree:
  `index_prereq_test`, `resource_budget_test`, `counted_io_test` and `lease_probe 3` pass with zero
  reports; the full TSan suite was not run (the developers did, per branch).

## Non-blocking notes

- **S2-N1 (architect, before item 3's sweep uses it): the external cap has no floor.** `note_failure`
  lowers the cap to `res.before()`, which can be 0: `budget_probe 2` (`soft(D)=0`, `over_soft()` true with
  one descriptor held) and `budget_probe 6` (a real `EMFILE` from the first `PosixIO` the process opens
  gives `external=0 soft=0`). A soft cap of 0 asks the sweep to evict everything for the quiet period
  plus the climb (about 60 s and 42 passes), though the hard cap and D6's "fewer than 64 indexes" rule
  say the budget must always leave room for `min_indexes`. Suggest flooring the external cap at
  `min_indexes` (or the architect rules that zero is intended). Also: a single transient `ENOMEM` on maps
  does the same for maps.
- **S2-N2: the counts' lifetime is a silent contract.** A reservation keeps raw pointers to the
  `resource_counts` of its catalog and slot (and to the budget). `budget_probe 3` (ASan): counts freed
  while a mapping from the IO is still held, then the mapping dies: heap-use-after-free in
  `resource_reservation::release()`. D I7 says a slot outlives what reaches it, so this is a contract,
  not a bug today, but neither header states it and it is the same hazard item 1 hit. Document it in
  `resource_budget.hh`, and add a debug assertion in `~resource_counts` that every count is 0.
- **S2-N3: `RLIM_INFINITY` overflows the cap arithmetic.** `derive_descriptors` computes `avail * 75`
  with `avail` near 2^64 when `get_limits()` returns `~0`: `budget_probe 1` gives `soft(D)=184467440737094747`
  and `hard(D)=184467440737094542`, i.e. soft above hard. Both are astronomically large, so there is no
  harm on a real system (the raise path makes the limit finite), but the comment in `resource_system`
  advertises `~0` as a supported value. Divide before multiplying or clamp to `max_files`.
- **S2-N4: `tick()` and `note_failure()` race on `external_`.** `tick()` loads `e`, computes, then plain
  `store`s `e + up` (or `unlimited` on a limit change) while `note_failure` lowers by compare-exchange
  without `mu_`; a failure landing between the load and the store is overwritten (and `external_at_` is
  stored after the CAS, so `tick` can read the old time with the new value). Cost: one extra failure
  before the cap is lowered again, only if a failure falls exactly at the end of a quiet period, which is
  when the sawtooth makes one most likely. Reading only; not reproduced. Use a compare-exchange in
  `tick()`.
- **S2-N5: nothing runs the concurrent allocation-failure path under TSan** (see Item 2, 3). If a
  TSan-safe injection point matters, fail through a wrapper `Mapping` constructor rather than global
  `operator new`.
- `managed_capabilities()` of the inner IO is not forwarded by `counted_io`, so a backend that reports
  capabilities appears to report none through the wrapper. Forward it unless that is intended.
- Linux `process_maps` is the line count of `/proc/self/maps` once at construction and is never
  re-read; later growth of the process's own mappings is caught only by the external cap's `ENOMEM`.

## Probes and artifacts (kept in `.scratch/review-step2/` of item 2's worktree)

- `probes/lease_probe.cc` (cases 1 to 4), `probes/budget_probe.cc` (cases 1 to 6).
- `cfg.sh` (configure a build of the merged tree), `build_nochk.sh` (checks-off build and run).
- `merged/` (detached worktree: item 2 head with item 1 merged, conflicts resolved, uncommitted; the probe
  targets are appended to its `CMakeLists.txt`), builds `b-asan`, `b-tsan`, `b-nochk`, logs
  `build-asan.log`, `ctest-asan.log`, `cmake-*.log`.

## Not run

Item 1's and item 2's mutants and preflight (developers' proof, as asked); the full TSan suite on the
merged tree; Linux and FreeBSD (CI); the real `vm.max_map_count` accounting (`/proc/self/maps`) on
Linux; a reproduction of the S2-N4 race; the chore-bit hang of S2-B1 (read, not run); benchmarks.
