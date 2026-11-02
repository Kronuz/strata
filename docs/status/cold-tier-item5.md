# cold-tier item 5: demote(), promote(), map_owed

> Status: BUILDING (2026-10-30) the developer's ledger for `docs/design/cold-tier.md` step 1, item 5
> (D3, D10, I5, I8; CT-R22, CT-R25, CT-R26 and C7 of `docs/reviews/cold-tier-design.md`, re-check 4).

- Design: `docs/design/cold-tier.md` revision 3 on `design/cold-tier` (strata:72a77f3 folds in CT-R22,
  CT-R25, CT-R26; its K5 is in `docs/status/cold-tier.md` there). Neither file is on this branch.
- Branch / worktree: `phase/cold-tier-item5` from main strata:4324896, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-item5`.
- State: built and proved; awaiting its one review (a durability and concurrency change).
- Done:
  - strata:72a77f3 (on `design/cold-tier`) the design and K5 text.
  - strata:3918fdc `demote()`, `promote()`, `promote_owed()`, `verdict::map_owed`, the owed bit
    (CT-R22), the atomic flag and the debug assertion (CT-R26), `unleased_maps()` (CT-R23's count),
    `map_losses()`, `views_held()` by segment id; `test/warm_test.cc`; the IO wrapper of
    `cold_segment_test` moved to `test/cold_hooks.hh`.
  - The note `docs/mechanisms/warm-indexes.md`, and `defer-io.md` gains `map_owed`.
- Next: the review; then item 6 waits on K4, K7 and K3 as the design says. Step 2 adopts
  `promote_owed()` at the wake and handles CT-R23 (C8).
- Blocked on: nothing. No fuzz target: item 5 adds no decoder or parser.

## What was built, and where it differs from the text

- `demote()` has no separate `free_views()`: `publish_point()` already scans the registry after a
  publication, so a second call is an equivalent mutant. K5.4's "skip `free_views()`" is the scan
  in `publish_point()` (M4b).
- The flag is set only when some twin can still map (not when every record carries the marker, or
  there are none), and only under `defer_io`. `promote()` with the flag clear returns at once, also
  on a demoted index without `defer_io` (there a read maps on demand and nothing is owed).
- The owed bit is cleared by an `apply` that returns anything but an owed answer, or throws, and by
  `close()`; `refresh()` and `commit()` do not clear it (the design says why; M10a shows the
  bound breaks otherwise). This refines CT-R22's wording ("cleared by ... `refresh()`, `commit()`").
- `views_held()` matched records by pointer (the design cites it as by id); it is now by segment id,
  which `heal()`'s replaced record also needed (M4c).
- Added: `unleased_maps()` (input maps on a thread holding no lease) and `map_losses()`.

## K5, rule by rule

Tests are in `test/warm_test.cc`; every mutant is one or two exact edits in `.scratch/mutants.py`,
built and run against `warm_test` in the RelWithDebInfo build (`-O2 -g`, asserts live); results in
`.scratch/results/M*.txt`. All 30 died. Options: `eager` is `eager_segments`; `defer` is `defer_io`
(which forces eager at open).

| Rule | Test (options) | Mutants (result) |
|---|---|---|
| 1 predicate | `predicate` (eager; defer off and on): buffered, pending merge (`stepping_executor`), unsynced, each alone; the snapshot of records, generation, digests, flag and bit is equal after a refusal; defer: an apply after a refusal is accepted with `calls()` 0; a closed index throws `index_fenced` from both | M1a buffered, M1b merge, M1c unsynced clause dropped (killed); M1d, M1e skip `live()` (killed); M1f flag set before the predicate (killed). No "closing" clause: `close()` fences |
| 2 every record twinned | `twin_every_record` (eager off, no defer): precondition 2 records unbodied, a late map through the old view, no record in `segs_` bodied | M2 twin only bodied records (killed). Equivalent under eager, so not run there |
| 3 twin fields | `twin_fields` (eager; and eager off for the marker and facts): `deletes_dirty` kept and the delete holds after commit and reopen; marker kept (`needs_heal()`); bits kept (`get` and the new view); facts of a reader-mapped body kept | M3a `deletes_dirty`, M3b marker, M3c bits, M3d facts (killed) |
| 4 publication and freeing | `publication_and_free` (eager): generation +1, live mappings 0 (wrapper count, retired views included), guard across `demote()` released on another thread then `reclaim()`, `views_held().bytes == 0`, no reader | M4a no forced publication, M4b no registry scan, M4c `views_held` by pointer (killed) |
| 5 invisible | `invisible_to_reads` (eager, defer): position, digests, contents equal before, after `demote()`, after `promote()`, with an uncommitted delete | M5 bits built empty (killed) |
| 6 `map_owed` | `owed_answers` (eager, defer; control defer off): replace-by-key and delete-by-docid answer `map_owed` with `calls()` 0, nothing applied, then `promote()` and the same position accepted with `calls()` 0; defer off: accepted and maps | M6a no check (killed; the debug `no_input_map_scope` assertion also fires), M6b flag never set, M6c `map_owed` without `defer_io` (killed) |
| 7 the answer ends | `answer_ends` (eager, defer): flag cleared; `demote()` between `promote()` and the retry refused; faultfs `ENOMEM` inside `promote()` (bodies kept, flag set, unfenced, next apply `map_owed`, second `promote()` completes); marker on a first map, `promote()` completes, apply throws `corrupt_segment` with `calls()` unchanged; a twin carrying a marker; flag clear: `promote()` makes no calls | M7a flag left set, M7b no owed bit, M7c cleared on a host error, M7d rethrows a marked record, M7e ignores the flag (killed) |
| 8 close with a posted delete | carried to step 2's sweep (the cold predicate is not in item 5) | not run |
| 9 T5 | `sanitizer_run`: forced overlaps (a rendezvous of 4 readers on a retired view's record, `promote()` racing a reader held inside `map()`), then two 120-round runs on `work_pool_executor` (defer off and on): 3 readers with guards parked and released on another thread, merges, commits, `reclaim()`, `demote()` on quiesce, the host answering owed answers. Counters printed and asserted at least 1 each | M11c install losses uncounted (killed). ASan+UBSan and TSan: below |
| 10 the turn's bound | `turn_bound` (flush and map in both orders, the adversary demotes at every retry) and `turn_bound_drain` (map then drain): at most one of each answer, at most 3 retries; the exact orders asserted | M10a `refresh()` clears the bit, M10b the bit never ends, M10c `drain_owed` ends it, M10d `flush_owed` ends it (killed) |
| 11 reads after `promote()` | `reads_after_promote` (eager, defer): a lease-less read before maps and `unleased_maps()` rises; after, `get`, a view `get`, a scan and a miss make `calls()` 0 and the count stays; the assertion at `apply`'s check | M11a `promote()` clears the flag with the last record unmapped (killed by the assertion), M11b unleased maps uncounted (killed) |

`default_path_unchanged` (defer off, no `demote()`) is in the suite and passes; `demote()` and
`map_owed` do not touch it. Not killed by a CHECK but by an abort: M3c, M5, M6a, M6b, M7c, M7d
(before the test change that wraps `promote()` in `run`), M11a; each also fails a CHECK or the debug
assertion first.

## Crash points

`crash_points` (defer off and on): a workload with commits, `demote()` between them (twins standing
for an uncommitted delete), `promote()`, a merge and a flush, cut by `faultfs::power_off_after(n)`
at every one of its 66 mutations and recovered with `crash(n)`. `demote()` makes no device call and
`promote()` no mutation (asserted on every run). After each cut: the contents are the last
acknowledged commit's or the in-flight commit's, every record has a body, `needs_heal()` and
`promote_owed()` are false, and the next apply is accepted with `calls()` 0. 66 of 66 points, both
modes, on rel, ASan+UBSan and TSan.

## Evidence

Compiler LLVM (`/usr/local/opt/llvm`), x86_64 macOS; builds in `.scratch/{rel,asan,tsan}` (CI's
flags: rel `-O2 -g` without NDEBUG; ASan+UBSan `-O1 -fno-sanitize-recover=undefined
-D_GLIBCXX_ASSERTIONS`; TSan `-O1 -g`).

- rel: ctest 61 of 61 (`.scratch/ctest-rel.log`).
- ASan+UBSan: 61 of 61, zero reports (`.scratch/ctest-asan.log`); `warm_test` counters: 4 late maps,
  470 install losses, 1 promote raced, 114 demotions, 58 owed promotes.
- TSan: 61 of 61, zero reports (`.scratch/ctest-tsan.log`); `warm_test` counters: 4 late maps, 435
  install losses, 1 promote raced, 114 demotions, 58 owed promotes.
- rel with `STRATA_EAGER_SEGMENTS=0`: 61 of 61 (`.scratch/ctest-rel-lazy.log`).
- `cpp-libraries/tools/preflight.sh . mac`: mac-hardened ok, mac-asan-ubsan ok
  (`.scratch/preflight-mac.log`). Linux ok, FreeBSD ok (the gate VMs).
- CI: pushed; the lead watches it.
