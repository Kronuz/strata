# strata review: the cold tier (preliminary design review)

- Date: 2026-10-27.
- Reviewed: `docs/design/cold-tier.md` revision 0 and its ledger `docs/status/cold-tier.md` at
  `4bc1a06` (branch `design/cold-tier`, HEAD equals `origin`); strata `main` `98ce743` for every code
  citation (`include/strata/core/index.hh`, `hazard.hh`, `owner.hh`, `files.hh`, `budget.hh`);
  `docs/perf/idle-index.md` at `2562e8f` (`perf/idle-index`, whose head `7d345db` changes only
  `CMakeLists.txt`); shared-reader revision 3 at `15b801a` and its review (`origin/design/shared-reader`);
  write-buffer D2 and D12 at `ae2cf70`; small-indexes F1 to F5 and the lead's 2026-10-18 rulings
  (`perf/small-indexes` `39ba962`, `docs/perf/small-indexes.md`, `docs/status/small-indexes.md`);
  completion's `PosixIO` at strata's pin `7734544`; prospect `9de01a1`.
- Checked against: ARCHITECTURE (`:6` the host reactor never blocks, `:146-151` recovery and checksums,
  `:179-180` overload shed, `:188-192` deferred frees on the owner); `limits.md` (the reclaim row: close
  waits for posted deletes); shared-reader D1, D4 to D7, D9, D11 and SR-D10; write-buffer D2 ("checkout
  never waits"); the lead's rulings on Q1 to Q4 (this review's prompt); `reviews/calibration.md` (every
  row applied: duplicate mechanisms, cited baselines without raw files, earlier rulings addressed to this
  subject, HEAD against origin, limits against their mechanism, each fix's mechanism found in the code).
- Checked by: reading every `index.hh` line the design cites and every owner read of a segment body;
  probes in `strata-wt/review-cold-tier/.scratch/ct-review/probes/` (worktree detached at `4bc1a06`),
  built against `98ce743` with Homebrew clang 23.1.2 on macOS x86_64 under light holds:
  - `twin_race.py` (`twin_race.out`): an explicit-state model of D3's twin swap against D2's lazy
    install, every interleaving, the design's rule and the fix.
  - `quiescence_probe.cc` (`quiescence_probe.out`, ASan and UBSan): part 1, `deletes_pending()` on an
    idle index whose deletes the executor finished; part 2, `close()` with files queued for reclaim.
  - `apply_throw_probe.cc` (`apply_throw_probe.out`, ASan and UBSan): `apply()` when a segment read in
    stage 1 throws.
  - `warm_bytes_probe.cc` (`warm_bytes_probe.out`): the heap per index that warm keeps, by piece,
    through `index_test_access`, 50 one-document indexes on `PosixIO`.
  - `zipf_policy.cc` (`zipf_policy.out`): hit ratios of LRU, one-bit CLOCK and S3-FIFO on T4's
    workload under the independent reference model, 20M requests after 5M of warm-up.
  - `build.sh`, `run.sh` rebuild and rerun them; `CMakeLists.txt` adds strata as a subdirectory, so no
    strata file is touched.
  - Not run: strata's test suites, TSan (no strata code changed: the probes test today's code and model
    the design), Linux, FreeBSD, arm64, any benchmark or timing, the idle-index harness.

## Verdict

**APPROVE WITH CONDITIONS: the lifetime rule (I1) is sound and the tiers are the right shape, but
step 1 needs five changes first (CT-R1, CT-R2, CT-R3, CT-R5, CT-R14), the catalog seven before step 2,
the sentinel one before step 3, and T1 to T4 do not yet test what they claim.**

What holds, and reproduces:

- I1 as a safety rule. A body is installed once by compare-exchange and never cleared, and a record
  dies only when its last `shared_ptr` drops, which happens under the lease: `segs_` edits, views freed
  after the registry scan (`index.hh:2479-2493`), merge jobs dropped by the owner (`:2965-2983`), the
  destructor (`:560-575`); `~open_segment` checks it (`:2109`). A record is reached only through a
  guarded view (`hazard.hh:119-130`), the owner's lease-less reads (which D5's slot word keeps off the
  sweep's chore), or a merge job that holds it, so no reader sees a mapping go. A reader that acquires
  during the swap holds either the old view (the scan keeps it) or the new one (the twins, mapped on
  demand); address reuse is harmless, as shared-reader D5 shows. What D3 gets wrong is the tier, not
  safety (CT-R1).
- I2. Twins name the same files, and `retain()` references files by name from `segs_`, pending merges'
  sources and every unfreed view (`index.hh:4145-4154`), so a twin swap queues nothing and no file a
  record names is truncated, mapped yet or not.
- I3. Within a process the slot table gives one slot per key and a slot is erased only after `close()`
  completes; the backstop is the directory lock, a `flock` on the IO's own directory descriptor
  (completion `posix.h:435-452`), per open file description, so a second instance in the same process
  gets `EWOULDBLOCK`. Demotion to warm keeps the instance; only cold to open makes one.
- Crash recovery from a half-demoted index is an ordinary open: demotion and `close()` write nothing
  (I2), and cold requires `applied_since_commit_ == 0` (`index.hh:4547`), so a crash anywhere in a
  transition recovers the last commit and drops nothing the host was told is durable.
- The lead's rulings Q1, Q2 and Q4 hold; Q3 holds with CT-R14's conditions (see "Rulings").

What blocks: CT-R1 to CT-R3, CT-R5 and CT-R14 before step 1's code; CT-R4, CT-R6, CT-R8, CT-R10, CT-R12,
CT-R13 and CT-R11's T3 and T4 before step 2; CT-R7 before step 3; CT-R9 and CT-R11's T1 and T2 before
the lead measures.

## Findings

Builds-on findings first: those that change what step 1 builds (the record, its accessor, `apply()`'s
contract, open's checks).

1. **CT-R1. D3 twins only records that have a body; an older view's reader maps the rest into the
   warm index (required, step 1).**
   - D3 "swaps every record whose body is set for a twin" (`cold-tier.md:83`). The records in `segs_`
     are the objects every view since their flush holds (`build_view` copies `segs_`'s pointers,
     `index.hh:2389-2392`). A record left in place because its body was null can still be mapped
     through D2's compare-exchange by a reader holding any older view, and its body then lives in a
     warm index's `segs_` until `close()`: counted in the catalog's maps, freed by no demotion.
   - `twin_race.out`: of the 10 interleavings of one lazy map with a two-record demotion, 6 leave a
     mapped record in `segs_` under D3's rule, among them the plain sequence "demote, then the reader
     maps" (`oorrr`); with every record twinned, 0.
   - With every record twinned, the mapping lands in the retired view's record instead (10 of 10) and
     goes at the first registry scan after the reader releases. Shared-reader D6 scans at the owner's
     publish, commit, `reclaim()` and close, none of which a warm index runs, so a warm slot can hold
     maps until it goes cold.
   - Fix: D3 swaps every record for a twin. Demotion is complete when its retired views are freed; the
     sweep runs the scan (`free_views()`) under `chore_scope` on warm slots with retired views, and
     their bodies count in the slot's maps (CT-R8). T6 gains the mutant "twin only bodied records".
   - `views_held()` matches records by pointer (`index.hh:1244-1250`), so after a swap it reports the
     old records' bytes as held only by views though the twins name the same files: match by id.

2. **CT-R2. "First use maps" is wrong for owner paths that need only the entry, and unsafe where a
   map can now throw (required, step 1; it decides the record's fields).**
   - Owner paths read the body for fields the entry has or could have:
     - merge selection lists every segment with `s->seg.core().doc_count()` (`index.hh:1550-1555`) at
       every `refresh()` and `commit()` with `auto_merge` (`:750`, `:821`). Converted to `seg()`, every
       refresh maps every segment, and D3's warm-to-hot `publish_through()` → `refresh()` →
       `maybe_merge()` maps every twin, so D3's "without I/O" (`cold-tier.md:88-90`) fails. The entry
       has the count (`files.hh:86`).
     - `commit_allowance()` reads the same count (`index.hh:3248`); it runs from `settle_storage()`,
       which `~hold()` calls in a destructor (`:3209-3211`).
     - `commit_files()` reads `structures()`, `version()` and `total_length()` of every segment for
       the manifest (`index.hh:4073-4081`); `segment_ref` has none of them (`files.hh:79-93`). As written
       every commit maps every segment; carrying them on the record needs a first map or a manifest
       field (a format change, open until release).
   - A map can now fail where nothing failed before: open and `mmap` errors (`EMFILE`, `ENFILE`,
     `ENOMEM` at `vm.max_map_count`), the checks Q3 moves, D6's `too_many_open`. Inside `commit()` the
     reads run in `guarded()`, which fences on any `system_error` (`index.hh:2268-2276`), so a
     descriptor shortage at the first commit after an open fences a healthy index. In `schedule()`
     (`:2523`) a source failing its checks throws out of `refresh()` or `commit()`, where a finished
     merge's corrupt source is marked damaged and skipped (`:2827-2828`, `:2858-2870`). In `apply()`'s
     lookups see CT-R5.
   - Off the owner: `read_checked` counts media retries into the owner's plain `media_retries_`
     (`index.hh:4402-4411`, `:4533`), which a reader thread's map cannot write; and a loser of the
     compare-exchange destroys its body on a reader or pool thread, where the leaf check reports owner
     state changed off the lease (on a pool worker in every build, anywhere in debug builds,
     `owner.hh:58-62`), so the body's destruction carries no such check.
   - Fix, in revision 1 before step 1's code: D2 classifies every read of a segment body by its path:
     the reads through `segment_view` (`index.hh:958`, `:1792`, `:1883-1959`, `:2118`, `:2392`), stage
     1's lookups (`:3345-3389`), merge selection (`:1554`), scheduling and the job (`:2523`,
     `:2728-2759`), commit (`:4003-4108`), the allowance (`:3248`), install and flush installs (`:1392`,
     `:3822`), digest, check, heal and `mark_damaged` (`:1603`, `:1325`, `:891-903`, `:2864`). Each is
     entry-only (merge selection, the allowance, the all-dead erase) or a map point. It decides
     `commit_files()`'s three fields (map at commit, and say so, or carry them on the record), and gives
     each map point its failure rule: resource errors are the host's, never a fence and never out of a
     destructor or a `noexcept` chore (`read_checked` already tells them from the medium's,
     `:4414-4418`); a source failing at scheduling is marked damaged and skipped; retries count on an
     atomic. Each rule gets a faultfs-injected test.

3. **CT-R3. The quiescence predicate never holds for an idle index whose last call posted deletes
   (required, step 1's `demote()`).**
   - D3 and D5 require "no posted delete (`deletes_pending()`)" (`cold-tier.md:81-83, 127-128`).
     `deletes_pending()` is `posted_.size()` (`index.hh:851`), and `posted_` shrinks only in
     `collect_deletes()` (`:3063-3081`), which owner calls run: `reclaim_posted()`, `drain()`,
     `finish_deletes()`, `publish_merges()` (`:3117`, `:3259`, `:3097`, `:2847`). A check-in's chores do
     not (`:2341-2353`). Every open with orphans posts deletes (`recover()` → `reclaim()`, `:4371-4372`).
   - `quiescence_probe.out`, part 1: after a commit 5 deletes are pending; after the executor ran all
     five, 5; after a check-in, 5; after another run, 5; after `reclaim(0)`, 0.
   - So an index idle after any published merge, commit with reclaim, or reopen is never demotable
     until a writer call happens to collect.
   - Warm does not need the condition: a posted delete touches only the IO and its channel
     (`index.hh:3028-3030`), which demotion keeps. Cold does: `close()` waits for posted deletes with no
     timeout (`:3103-3110`; `limits.md`, the reclaim row), which `chore_scope` must never do and which
     hangs a sweep that runs on the deletes' only worker.
   - Fix: drop it from warm's predicate; cold's chore calls `collect_deletes()` (non-blocking) and then
     requires `posted_` empty. T5 includes an index idle after posting; T6's mutant "demotion with a
     posted delete" becomes "close with a posted delete".

4. **CT-R4. `close()` drops the reclaim queue and frees the storage permit for bytes still on disk
   (required before step 2).**
   - `close()` resets `storage_permit_` (`index.hh:602`); the queue (`reclaim_`, `:4530`) goes with the
     object, and the next open reclaims only `reclaim_slice` (`:4371-4372`).
   - `quiescence_probe.out`, part 2: before `close()`, 5 files queued, 6 segment files for 1 live
     segment, 6,574 B on disk and 7,093 B on the storage budget; after it, the same files and bytes on
     disk and 0 B on the budget; the reopen (`reclaim_slice` 0) queues all 5 again and counts 7,093 B.
   - With millions of indexes the shared storage budget undercounts the disk by every closed index's
     queue, so admission accepts writes the disk cannot hold; a cold index never reopened keeps its
     garbage. D7's idle core, "freed at demotion to warm" (`cold-tier.md:161`), repeats this if the
     queue moves into it.
   - Fix: cold also requires an empty queue and no debt; the sweep posts the reclaim (with the host's
     executor, which the catalog then requires, `reclaim()` posts and returns) and closes on a later
     pass, or the catalog keeps a closed index's queued bytes on the budget until it reopens. T5 checks
     the budget against the bytes on disk after closes.

5. **CT-R5. `apply()` is not atomic when a segment read in stage 1 throws (pre-existing; required
   before Q3 moves the checks).**
   - `apply()` calls `evaluate()` with no handler (`index.hh:663-664`) and rolls back only a verdict
     other than accepted (`:670-675`). `evaluate()` reads segments in its lookups (`:3345-3389`). An
     exception after an earlier operation leaves that operation's record, key slot and pending delete
     in the buffer; nothing advances `transactions`, so the next accepted transaction's position covers
     them (write-buffer D5).
   - `apply_throw_probe.out`: with the older segment's `keys` section damaged at its size, the open
     passes; `apply({replace Q2, replace Q1})` throws `corrupt_segment` ("keys checksum") at Q1's
     lookup; the next transaction, `{add Q3}`, is accepted, and after `refresh()` `get("Q2")` returns
     `b2`. The failed transaction's first operation is published with the next one. The same damage
     in `key_index` or `versions` fails the open today ("section checksum").
   - Today a damaged region of a lazily verified section triggers it. D2 and Q3 move every check the
     open makes (those two sections' among them) into the first map, and add open and `mmap` errors
     and `too_many_open`, all inside the same call; a replica that applies the same log without the
     fault diverges from this one's digest.
   - Fix, on main first as its own change with this probe as its test: `evaluate()`'s exceptions roll
     the undo log back and rethrow, leaving the buffer and positions as before the call. Step 1 moves
     the checks after it lands.

6. **CT-R6. The `closing` fallback makes pinners wait on a guard holder without bound (required
   before step 2).**
   - D5 closes any unpinned open slot, hot or warm; if `close()` leaves it closing, new pinners wait until
     `try_finish_close()` succeeds (`cold-tier.md:126-131`), that is, until a reader that may hold its
     guard for minutes (shared-reader's "A long read holds disk") or forever (SR-D7) releases. D4 makes a
     pin a checkout (`:106`), and write-buffer D2 says a checkout "never waits".
   - With the sentinel published and every retired view freed, no reader can hold or newly acquire a
     view of the index (acquires meet the sentinel), so `close()` there always completes.
   - Fix: cold only from warm, after a scan finds no retired view; a slot whose views are held stays
     warm and counts against the caps; closing stays a debug-reported fallback. Waits on `opening` and
     `demoting` are suspensions resumed on the waiter's executor, as D5 says of opening, and a
     synchronous pinner gets `too_many_open` rather than block a reactor thread (ARCHITECTURE `:6`).

7. **CT-R7. The unpublished sentinel reads as an empty index (required before step 3).**
   - `reader_registry::acquire` guards any non-null pointer (`hazard.hh:119-130`), and
     `reader_handle::acquire()` throws only on null (`index.hh:1164-1168`). A caller that does not test
     for the sentinel reads a view with no segments: every get misses and every search is empty, with no
     error. `live()` (`:1194-1199`), prospect and Xapiand's seam have no such test, and the seam's fast
     path (shared-reader D9) skips `publish_through()` when the published position is current, which it
     is on a warm index.
   - D3 rebuilds through `publish_through()` → `refresh()` (`index.hh:1226-1232`, `:745-752`), which
     flushes when a write reached the warm index (the write buffer's views cover buffers without a
     flush, `:2417-2427`) and runs merge selection (CT-R2): "without I/O" holds only when nothing is
     buffered and no merge is due.
   - Fix: `acquire()` never returns the sentinel; it throws a typed `index_unpublished`, or the handle
     offers an awaitable acquire that publishes. The sentinel is never in `pub_->current` (a
     `unique_ptr` would delete a static) and never retired. The warm-to-hot publication builds the view
     over the current segments and buffers without refresh's flush and merges. T6 adds the mutants
     "sentinel read as a view" and "sentinel retired".

8. **CT-R8. The hard cap's eviction is not tied to the resource over the cap (required before
   step 2).**
   - D6 counts maps, descriptors, vnodes and bytes per catalog (`cold-tier.md:135-141`), but the CLOCK
     hand walks slots with no per-slot count. A promotion at the maps cap that closes a warm slot frees
     no map; demoting a hot slot frees its maps only after its retired view's scan (CT-R1), which a
     reader can defer. "Cannot first evict an unpinned slot" (`:138-139`) is then undecidable, and an
     eviction can succeed and free nothing.
   - Check-then-increment races: two first maps at the cap less one both pass.
   - Thundering herd: cold to open is single-flight (`:118-120`); a first map is not. N readers meeting
     an unmapped record each open, map and check it, and N − 1 lose the compare-exchange (D2); at the cap
     their reservations refuse other promotions.
   - Fix: per-slot counts of maps, descriptors and vnodes (bodies on live records and on retired views);
     the evictor takes an unpinned slot that holds the resource over its cap and has no held view;
     counts reserved by `fetch_add` before the system call and returned on failure or a lost
     compare-exchange. T5 drives each cap to its hard value with every slot pinned and checks
     `too_many_open` and no kernel error.

9. **CT-R9. Warm's targets omit what warm keeps (required: restate T1).**
   - Warm's estimate is the idle-index note's three cuts plus a 130 B slot (`cold-tier.md:52`). The cuts
     count the whole 1,496 B publication and the whole 1,890 B segment as freed, but D3 keeps `pub_` (the
     reader state, the retired list, the generation and change counters) and D2 keeps a record per
     segment.
   - `warm_bytes_probe.out` (requested bytes per one-document index on `PosixIO`, 50 indexes): hot is
     5,394 B in 34 allocations (open 3,898 B, `reader()` 1,496 B). Retiring and freeing the view returns
     304 B. The empty active buffer the first view makes (`index.hh:2403-2408`) returns 920 B
     (`sizeof(buffer)` 896). The segment object returns 1,866 B in 14 allocations. 2,304 B stay
     (`basic_index` 1,320 B, the publication 136 B, the reader state 96 B and its block, the manifest
     and file maps). So the note's "view 920" is the empty buffer, its "publication 208" the view
     (`sizeof(view)` 208) and its "reader state 136" the publication: cut 2's 1,360 B saving rests on
     mislabelled rows.
   - From those pieces, with a record of about 200 B per segment (`sizeof(open_segment)` 936 less the
     744 B `segment`, its mapping pointer and owned string, plus its control block and bits, derived)
     and cut 3's 850 B taken as given: warm as D3 states it keeps about 2.6 KB of heap per one-segment
     index, and about 1.7 KB if demotion also drops the empty buffer, both before the 130 B slot, the
     host's `PosixIO` and allocator slack (7% in the note), against 1.5 KB. The design's own figures
     already miss: 1.4 KB plus 130 B is 1.53 KB.
   - Step 1's "T1's warm row without a slot" (`cold-tier.md:214`) runs before the sentinel and the idle
     core, so it measures a published view and the buffer, about 3.7 KB of heap by the same arithmetic;
     it needs a step-1 target of its own.
   - Fix: D3 drops the empty active buffer at demotion (the next write makes one), and T1 restates warm
     from measured pieces per segment count: the record (its `sizeof`, its bits' allocation, its control
     block) times segments, the publication object and reader state, the slot. T1 adds a 10-segment
     fixture. The lead corrects the note's labels (calibration: documents are claims).

10. **CT-R10. A one-bit CLOCK cannot meet T4's θ = 0.99 target (required before step 2).**
    - T4 asks a hot-plus-warm hit ratio of at least 0.80 at θ = 0.99 and 0.95 at θ = 1.2, with 110,000
      open of 1M, against a perfect frequency cache's 0.837 and 0.967 (`cold-tier.md:201`).
      `zipf_policy.out`: LRU 0.775 (Che's approximation 0.775) and 0.952; one-bit CLOCK 0.782 and 0.954;
      S3-FIFO (a 10% probationary FIFO, 2-bit counts, a ghost of keys) 0.817 and 0.961. D6's policy
      misses the 0.99 row by 1.8 points before any cost of the catalog and passes the 1.2 row by 0.4.
    - A frequency-blind policy also admits every one-time access: a scrub, a backup, or T3's own "touch
      each index once" evicts the working set.
    - Fix: a frequency-aware policy with the same per-hit cost, one small counter store and no relink
      (S3-FIFO, or CLOCK-Pro), chosen by T4; or T4's targets restated to the chosen policy's model.
      Limits' "at least 16% of requests miss" (`cold-tier.md:223`) is 22% under one-bit CLOCK.

11. **CT-R11. Gate rows that do not test their claim, or cannot run (required: revise T1 to T4).**
    - T2:
      - hot ≤ 10 µs p99 is already missed by today's first query on 1,000-document indexes on macOS,
        10.2 µs (`idle-index.md`, first table, row "1,000 | 100,000");
      - T2 names no fixture or segment count. Its fixtures have one segment, but a get reads segments
        newest first until it finds the key (`index.hh:1883-1896`), so a warm get maps every segment
        down to the hit and all of them on a miss; Limits' "blocks for one map" (`cold-tier.md:226`)
        holds for one segment only;
      - "warm ≤ 100 µs Linux" beside "Linux also after `drop_caches`" leaves open whether the warm
        target holds with the cache dropped, where a map reads the disk (cold opens: 1 to 4 ms p99);
      - its estimates come from one run per cell, with 2× run-to-run spread on macOS and no raw files
        kept (`idle-index.md`, Method; its ledger: "VM directory removed afterwards").
    - T3:
      - 10M takes 120 to 160 GB and 40M inodes (`cold-tier.md:200`; this Mac has 81 GiB free), and
        touching 10M indexes once at 0.4 to 1.1 ms per macOS open is one to three hours on one thread:
        a multi-hour gate, which the benchmark tiers rule out;
      - "counters within hard caps" checks the catalog's own counters; the claim is about the kernel's
        numbers (VMAs in `/proc/self/maps`, open descriptors, vnodes in use on macOS, slab on Linux,
        where 10M touched indexes leave dentries and inodes cached);
      - "stock sysctls" is not stock on the development Mac, whose `kern.maxfilesperproc` and launchd
        `maxfiles` are raised to 1,048,576 (CT-R12).
    - T4:
      - caps of 10,000 hot and 110,000 open exceed D6's macOS vnode soft cap: 100,000 warm plus 10,000
        one-segment hot indexes at 2 vnodes is 120,000, over 105,267, so on macOS T4 measures the vnode
        cap's demotions, not its configured policy (D6's own example is 80,000 warm and 12,000 hot,
        `cold-tier.md:154-155`);
      - "p99 ≤ T2's cold target" compares an 8-thread closed loop with 22% misses to a single-threaded
        latency; F3's cache simulation, one thread at 32.8% misses, already had a p99 of 414 µs on the
        gate VM (over the 400 µs Linux target) and 37.9 ms on a busy Mac (`small-indexes.md`, F3).
    - T1: CT-R9's restatement, a 10-segment fixture, and the slot's bytes counted (the slot array is
      not a per-index allocation).
    - Fix: T2 per fixture and segment count (1 and 10), cached and dropped stated per row, its baselines
      re-measured with raw files; T3 proves the bound at a count far above the caps (1M on Linux with
      caps of 10,000 hot and 50,000 open, say) sampling the kernel's numbers, 10M informational; T4 with
      caps D6 allows on each platform, the rlimit stated, and its own p99.

12. **CT-R12. The descriptor caps name the wrong mechanism (required before step 2).**
    - D6 caps open indexes at 75% and 95% of the soft `RLIMIT_NOFILE` less 1,024 on Linux and of
      `kern.maxfilesperproc` on macOS (`cold-tier.md:145, 147`). A process's limit on both is its own
      soft `RLIMIT_NOFILE`: 256 by default on macOS (launchd's `maxfiles`; this Mac's is raised to
      1,048,576, as is its `kern.maxfilesperproc`) and 1,024 under systemd's default on Linux. With
      macOS defaults the catalog would allow 75% of `kern.maxfilesperproc` and meet `EMFILE` near 250;
      with Linux defaults the formula gives 0. The idle-index runs raised the limit.
    - Fix: the catalog raises the soft limit to the hard one at construction (`setrlimit`, no privilege),
      takes its caps from the resulting soft value on both platforms (bounded by `kern.maxfilesperproc`
      on macOS), and makes the host's reserve an option; `limits.md` records the mechanism.

13. **CT-R13. The slot protocol leaves three orders unstated, and a pin costs more than it says
    (required before step 2).**
    - Unpin is "a lock-free decrement plus a time store" (`cold-tier.md:107`). Stored after the
      decrement, the time can land in a slot the sweep has meanwhile closed, erased and reused for
      another key (slots are reused, `:98-100`): a data race on a plain field. Store it first; the
      decrement releases it. T6's schedules reuse slots and carry the mutant "time stored after the
      decrement".
    - Hot to warm "stores `open`; pinners meeting `demoting` wait" (`:123`), and the busy path "restores
      `open`" (`:124`), without saying that the store and the waiters' resumption happen under the shard
      mutex, as open-to-cold's does (`:129`). Otherwise a pinner that read `demoting` under the mutex and
      parks after the sweep looked for waiters is never resumed.
    - The sweep reads a slot's key, index and handle without the shard mutex while a pinner may be
      filling a reused slot. It reads them only after its compare-exchange from `open` succeeds
      (acquire), and an opener stores `open` (release) after them.
    - "One read-modify-write per request" (`:108`) is three or four: the shard mutex's lock and unlock,
      the word's compare-exchange, the unpin's decrement. At θ = 0.99 over 1M the hottest key takes 6.5%
      of requests and the top 10 take 19.2%, each key's pins through one shard mutex; T4 reports the pin's
      cost on the hottest key at 8 threads.

14. **CT-R14. Q3 inherits an earlier ruling and needs the existence check D2 assumes (required,
    step 1).**
    - The lead accepted first-use checks for small-indexes F1 to F7 on 2026-10-18 "on the condition that
      a segment damaged at its size fails every read path clean and heal() reports it", with twelve
      tests (`8b834ee`; `docs/status/small-indexes.md` on `perf/small-indexes`, "The lead's rulings").
      Neither D2 nor Q3 carries it.
    - D2 says recovery "already opens every file for its size (`:4319`), so a missing file still fails
      the open" (`cold-tier.md:70-72`). `index.hh:4312-4319` opens the files the directory scan found,
      not those the manifest names; a manifest-named segment that is missing is caught today only by
      `load_segment_of`'s open (`:4461-4466`), which D2 takes out of open. F5 made the check explicit:
      each segment's existence and size against the scan.
    - Fix: open checks every manifest-named file's existence and size against the scan's names and sizes
      (no system call); the twelve damage cases run as step 1 tests with lazy segments; a record whose
      first map fails twice is marked so later reads fail fast with `corrupt_segment` until `heal()`
      replaces it.

15. **CT-R15. Smaller items (not blocking).**
    - `hash_slots` (`index.hh:226-307`) is an open-addressed table of 32-bit values with a full-key test
      and backward-shift deletion: the catalog's shard table is that shape (values are slot indexes).
      Reuse it rather than add a second (calibration, row 1).
    - D2's prospect citation (`searcher.hh:98-99, 223`) names prospect's own `segment_snapshot::seg`
      (prospect `searcher.hh:26-30`), a raw pointer the host fills. The migration is where snapshots are
      built (prospect's tests, Xapiand's seam), mapping each segment, which a search does anyway: its
      statistics read every segment's count and length (`searcher.hh:95-101`). prospect pins strata by
      commit (prospect `CMakeLists.txt:9`), so step 1 breaks nothing there until the pin moves.
    - The slot's IO owns the lock: `PosixIO`'s lock is a `flock` on its directory descriptor, closed by
      `~PosixIO`. A catalog destroyed with closing slots must leak those IOs with the lock, as the
      destructor leaks the lock (`index.hh:567-570`), or the lock goes with the descriptor.
    - Key-to-directory resolution must be injective. Two keys on one directory (APFS is case-insensitive
      by default) meet `EWOULDBLOCK` from the lock, which the catalog reports as a host error, not retries.
    - I5: step 1's `demote()` must force its publication; `publish_point()` skips when nothing changed
      (`index.hh:2379`).
    - I7 has a host half: no reference to a catalog's index outlives its pin (a reader handle excepted,
      which outlives the index by design and then throws `index_closed`). A writer that calls without a
      pin can wait on the sweep's chore in `lease_scope` (`owner.hh:120-123`) and wake on a destroyed
      index. A debug build checks a pin at every call on a catalog index.
    - D8's fetch on a pin miss is network I/O: it runs on the executor under `opening`, never on a
      reactor thread.

16. **CT-R16. The ledger has no done criteria** (`docs/status/cold-tier.md:1-21`). C1 proposes step 1's.

## Rulings on the lead's answers

- **Q1 (keep the per-index lock; a lock per partition later): upheld.** The directory descriptor also
  serves every lazy map: `PosixIO` opens files with `openat` on it (completion `posix.h:618-621`). Under
  (b) each map would resolve a path from the partition root; step 5's review weighs that cost.
- **Q2 (the catalog in strata core): upheld.** Demotion and close are correct only with strata's lease,
  views, records and quiescence state (`chore_scope`, `owner.hh:151-171`); the host supplies caps,
  timers and the key resolver. The sweep runs on the host's timer, not a new one.
- **Q3 (checks at first map): upheld, with CT-R14 and CT-R2's failure rules in the text and CT-R5 on
  main first.** Moving the checks also changes ARCHITECTURE's "footers and small sections at open"
  (`:149`), which the lead edits on landing, as the design's table says.
- **Q4 (no summary per cold index): upheld.** Existence is a directory lookup, and 160 MB per 10M
  indexes buys nothing until a host routes without opening.

## Blocking items / Required changes

Revision 1 states these before the code each shapes; an architect re-check (high) covers their text.

- **Before step 1's code:**
  - CT-R5: the rollback on `main`, its own change, with `apply_throw_probe` as its test.
  - CT-R2: D2's table of owner reads (entry or body), `commit_files()`'s three fields, and each map
    point's failure rule.
  - CT-R1: every record twinned; the scan of warm slots with retired views.
  - CT-R3: warm's predicate without posted deletes; cold's after `collect_deletes()`.
  - CT-R14: existence and size against the scan at open; the twelve damage cases with lazy segments;
    a failed first map marked.
- **Before step 2 (the catalog):** CT-R4, CT-R6, CT-R8, CT-R10, CT-R12, CT-R13, and CT-R11's T3 and T4.
- **Before step 3 (the sentinel):** CT-R7.
- **Before the lead measures T1 and T2:** CT-R9 and CT-R11's T1 and T2.

What may start now: step 1's items 1 and 2 (record and body, `seg()`) with `eager_segments` on by
default, which changes no behavior; lazy segments become the default once CT-R5 has landed and revision 1
states CT-R2 and CT-R14.

## Conditions

- **C1. Done criteria for step 1** (the lead writes them; each with its evidence):
  - K1: the record and body, compare-exchange install, a loser that destroys only its own body; T6 over
    the install under TSan, with the mutants "body cleared in place", "body stored without
    compare-exchange", and "loser reads the body it destroyed".
  - K2: CT-R2's table in code, each map point's failure rule with a faultfs-injected test (open,
    `mmap`, and check failures): no fence for resource errors, damaged-and-skipped merge sources.
  - K3: CT-R5's rollback on main, its test failing before and passing after.
  - K4: the twelve damage cases with lazy segments; a missing or resized manifest-named file fails the
    open (CT-R14).
  - K5: `demote()` twinning every record, forcing its publication, under CT-R3's predicate; T6's
    mutants including "twin only bodied records"; T5 on one index with readers holding views across
    `demote()`.
  - K6: T1's step-1 rows: hot within 5% of today; demoted with its own target; 1 and 10 segments.
  - K7: prospect's construction sites and Xapiand's seam listed, with the pin bump that adopts `seg()`.
  - K8: CI green on every platform, ASan, UBSan and TSan.
- **C2.** The lead re-measures the idle-index rows T1 and T2 rest on, with raw files kept, before they
  gate (calibration, row 3).
- **C3.** T4 runs with caps D6 allows on each platform and states its `RLIMIT_NOFILE` (CT-R11, CT-R12).
- **C4.** When durability-modes' checkpoint batch lands and keeps a past view out of retirement
  (shared-reader D4, last paragraph), cold's predicate gains "no view the owner keeps" and T5 runs a
  batch across a demotion. It is not on `main` today (`index.hh` has no batch).

Not run: strata's test suites and TSan (no strata code changed), Linux, FreeBSD, arm64, any benchmark
or timing, the idle-index harness, a model checker beyond `twin_race.py`.

Probes kept in `strata-wt/review-cold-tier/.scratch/ct-review/probes/`: `twin_race.py`,
`quiescence_probe.cc`, `apply_throw_probe.cc`, `warm_bytes_probe.cc`, `zipf_policy.cc`, their `.out`
files, `CMakeLists.txt`, `build.sh`, `run.sh`. Rerun with `build.sh` then `run.sh` under a light hold;
`zipf_policy` builds alone (`clang++ -O2 -std=c++20 zipf_policy.cc`).

## Re-check 1

- Date: 2026-10-27.
- Reviewed: `docs/design/cold-tier.md` revision 1 and `docs/status/cold-tier.md` at `ae1d88a`
  (`8459c7b..ae1d88a`: `e6bdf25`, `ae1d88a`); every `index.hh` citation at strata `main` `8798fe1`,
  whose `include/` equals `98ce743`'s (`git diff --stat 98ce743 8798fe1 -- include/` is empty);
  completion's `PosixIO::map()` at strata's pin `7734544` (`posix.h:342-354`: an `mmap` failure throws).
- Checked against: this review's CT-R1, CT-R2, CT-R3, CT-R5, CT-R14, CT-R16 and C1; I1; the designer's
  four questions ("What the re-check should look at") and Q5.
- Checked by: every read of a segment body in `index.hh` enumerated (`.seg`, `->seg`, `core()`,
  `load_segment`, `open_segment`) and matched to D9's rows; one new probe in
  `strata-wt/review-cold-tier/.scratch/ct-review/probes/`, built with Homebrew clang under a light hold
  (ASan and UBSan, zero reports):
  - `output_map_probe.cc` (`output_map_probe.out`): a wrapper IO over faultfs whose `File::map()`
    throws `ENOMEM` while armed (the error `vm.max_map_count` gives); a flush, then a merge, mapping
    their output under it. Added to `CMakeLists.txt`, `build.sh` and `run.sh`.
  - Not run: strata's test suites, TSan, Linux, FreeBSD, arm64, any benchmark, the earlier probes
    (the designer reran them with identical output; nothing they test changed).

### Verdict

**GO WITH CONDITIONS for step 1: CT-R1, CT-R3, CT-R14 and CT-R16 are closed; CT-R2 is partially
closed (D9 misses the output maps, which fence today); four new items (CT-R17 to CT-R20) are text
fixes to land before the step-1 item each shapes. Nothing blocks items 1 and 2 starting.**

### Status

- **CT-R1: closed.** D3 twins every record, bodied or not (`cold-tier.md:93-100`); demotion is complete
  when its retired views are freed, the sweep scans warm slots with retired views and their bodies
  count in the slot's maps (`:102-105`); `views_held()` matches by segment id (`:105-106`, against
  `index.hh:1244-1250`); K5 carries the mutant "twin only bodied records" (`status/cold-tier.md:42-45`).
  The twin's field list is CT-R18.
- **CT-R2: partially closed.** D9's table (`cold-tier.md:183-206`) decides `commit_files()`'s three
  fields (facts on the record, mapped in commit's prepare, `:67-71`, `:213-217`), gives each map point
  a failure rule (`:208-226`), and moves retries to an atomic (`:82-84`). Every owner read the grep
  finds is in the table except these, so it is not complete:
  - **Output maps.** A flush maps its new segments inside `guarded()` (`index.hh:748`, `:819` →
    `:3995` → `streamed_segment`, `:2290-2299`; `written_segment`, `:2281-2286`), and a merge maps its
    output on the job (`:2760-2789`), whose `system_error` `publish_merges` fences (`:2830-2831`).
    `PosixIO::map()` throws on `mmap` failure. `output_map_probe.out`: with the map failing `ENOMEM`,
    `refresh()` throws "durability failure" and the next apply and commit throw "the writer is
    fenced"; `force_merge(1)` the same. So a map shortage, the resource the catalog drives toward its
    cap, fences a healthy index, D9's "never a fence" fails for these paths, and its debug assertion
    "no map point inside `guarded()`" (`cold-tier.md:225-226`) trips on every flush. Pre-existing,
    but D9 claims the rule for every map.
  - **`install()`'s remap of its own copies** (`index.hh:1405-1413`), outside `guarded()`; the table
    lists only the incoming load (`cold-tier.md:203`).
  - Citation: "`live_length()` of a segment with new deletes" is `:4032`, not `:4041` (the metadata
    write).
  - Fix: D9 gains an "output" kind: a host error from `map()` on a file this index just wrote falls
    back to the bytes, as a backend that cannot map does (`streamed_segment` reads back, `:2296-2300`;
    `written_segment` and the merge already hold the bytes), never a fence; T5's faultfs gains map
    failures; K2 lists flush and merge outputs. `install()`'s remap becomes an unmapped record with
    facts copied from the incoming body (entry), or a map row with today's rule (`:1405-1406`).
- **CT-R3: closed.** Warm's predicate drops posted deletes; cold's runs a non-blocking
  `collect_deletes()` and requires `posted_` empty (`cold-tier.md:231-245`); K5 has "close with a
  posted delete" and T5 an index idle after posting (`status/cold-tier.md:42-45`).
- **CT-R5: open (pre-existing), dependency stated.** The design says lazy segments are the default
  only after `fix/apply-atomic` lands (`cold-tier.md:43`, `:222`, `:322`; `status/cold-tier.md:23-24`;
  K3 at `:38-39`). Not on `main`: the local `fix/apply-atomic` points at `45c1ec9`, an ancestor of
  `8798fe1`.
- **CT-R14: closed.** Open checks each manifest-named segment's existence and size against the
  scan's `track` records with no system call (`cold-tier.md:249-254`; `index.hh:4319`); the remaining
  checks run at first map (`:255-257`); a failed first map is marked (`:258-260`); Q3 carries the
  lead's 2026-10-18 condition and its twelve cases (`:261-266`, `:44`, `:402-403`; K4).
- **CT-R16: closed.** K1 to K8 (`status/cold-tier.md:28-50`). K2 and K5 need the additions below.

### The designer's questions

1. **D9's map-point table:** incomplete; CT-R2 above (output maps, `install()`'s remap).
2. **The static failed marker against I1: sound, with three statements.** The marker is installed
   only by compare-exchange from null, never replaced or cleared, and never freed, so no reader can
   see storage go; a reader that loads it throws, and holds nothing. The text must say: the record's
   destructor frees a body, never the marker; the loser of either race (body against body, body
   against marker) destroys only its own body and then uses what won, so a good body losing to a
   marker throws `corrupt_segment`, deterministically; and twins carry the marker (CT-R18).
   `needs_heal()` must see it (CT-R19).
3. **A host error in commit's prepare:** nothing half-changed that matters. The prepare runs after
   the flush, `publish_merges()` and `maybe_merge()` (`index.hh:819-821`) and before
   `guarded(commit_files)` (`:822`); it must stay there, since a flush can set deletes on records
   with no body. A throw there leaves what an `ENOSPC` from `commit_files()` leaves today: flushed
   segments and installed merges in `segs_`, unpublished until the next `publish_point()`; nothing
   durable, `applied_since_commit_` unchanged, nothing fenced. Bodies and facts the prepare already
   installed are true and stay. "With the index unchanged" (`cold-tier.md:214-215`) overstates it:
   say "as `refresh()` leaves it, unpublished".
4. **`maybe_merge()` swallowing scheduling's host errors:** yes, with limits (CT-R20).
- D10 against a delete posted between the collect and `close()`: holds. The sweep holds the lease
  through both, only owner calls post (`reclaim_posted`, `index.hh:3117`), and warm's predicate
  excludes pending merges, whose publication posts. One nit: with `posted_` empty, `end_deletes()`
  (`:3103-3110`) still waits on the group for a task's tail after it reported; bounded, but the chore
  should assert it is not waiting on a task that has not run.

### New items

17. **CT-R17. A reader's first map writes the record's facts, which the owner reads (required before
    step 1's item 1 lands).**
    - D2: facts "are set wherever a body exists (flush, merge output, install, first map)", on a
      record whose other fields are owner-written (`cold-tier.md:67-71`). A first map runs on the
      calling thread (`:197`), so a reader writes `structures`, `version`, `total_length` and the
      known flag while the owner reads them in commit (`index.hh:4074-4080`) or copies them into a
      twin: a data race on plain fields.
    - Fix: the record's facts are owner-written only. A reader's map leaves them in the body; the
      owner reads "record facts if known, else the body's" after an acquire load of the body, and
      copies them into the record in commit's prepare, at scheduling, and into twins. T6 gains the
      mutant "facts written by a reader's map", under TSan (K1).
18. **CT-R18. The twin's fields are not listed, and dropping one loses deletes (required before item
    5).**
    - D3 names "same entry, facts, bits shared, no body" (`cold-tier.md:94`). Warm does not require a
      commit (`:236`), so a twin can stand for a record with uncommitted deletes. `commit_files()`
      writes a live-docs file only where `deletes_dirty` is set (`index.hh:4023-4035`) and otherwise
      keeps the old generation (`:4053-4057`): a twin without `deletes_dirty` makes the next commit
      drop those deletes, and the documents return at reopen.
    - Fix: a twin copies `ref`, facts, the bits (shared), `dead`, `deletes_dirty`, `damaged`, and the
      failed marker (a static, so sharing it keeps I1), as `heal()` copies the first four
      (`:905-908`). T6 gains the mutant "twin drops `deletes_dirty`", whose oracle is a reopen.
19. **CT-R19. A marked record blocks every commit, and nothing reports it (required before item
    3).**
    - Commit's prepare maps every record whose facts are unknown (`cold-tier.md:213-214`), and after an
      open that is every segment. One whose first map fails its checks is marked, and every later
      commit throws `corrupt_segment` until `heal()` replaces it. Today that damage fails the open, so
      it is no availability regression, but it is a new outcome of `commit()` the design does not
      state, and `needs_heal()` reads only `damaged` (`index.hh:804-807`), which a reader's mark cannot
      set.
    - The prepare also maps records that `commit_files()` then erases as all-dead (`:4006`), so a
      damaged, fully deleted segment blocks commits for nothing.
    - Fix: D9 states commit's `corrupt_segment` outcome (unfenced, `needs_heal()` true, the host
      heals and commits again); `needs_heal()` also reports marked records; the prepare skips
      records whose `dead` equals `ref.doc_count`, and takes `total_length` from `ref.live_length`
      when the record has no live-docs generation (`:4053-4057` keeps them equal). K2 tests a
      damaged segment's first map inside commit's prepare.
20. **CT-R20. Swallowed merge errors need a count and a scope (required before item 3).**
    - Ruling on question 4: `maybe_merge()` may swallow host errors from scheduling's map beside
      `ENOSPC` (`index.hh:786-793`): merges are the policy's, and the next round retries. Limits:
      only `maybe_merge()`; `merge()`, `force_merge()` and `force_merge_deletes()` rethrow, as the
      host asked for them; a source failing its checks is marked and skipped, never swallowed as a
      host error; each swallowed round counts on an atomic the host reads, as `media_retries()`.
    - Why the count matters: under a map or descriptor shortage, skipped merges let segments pile
      up, and each adds a map per read, which feeds the shortage. Step 2's catalog reads the count
      (CT-R8's per-slot counts). K2 tests an `EMFILE` at scheduling: `refresh()` succeeds, the count
      rises, the next round merges.

### Q5 (facts in the manifest)

**Upheld: (a) for step 1.** It is buildable and changes no format. Two corrections for the record:
(b)'s "a commit never maps" is overstated, since a twin with uncommitted deletes and no body still
maps for `live_length()` (`index.hh:4032`, `:4102-4107`); and (b) also removes CT-R19's coupling of
commit to segment damage, an argument beyond speed. Decide it on numbers before step 2: T2 gains a
write-after-open row on the 10-segment fixture (maps and time of the first commit), with the many-
indexes workload's writes to cold indexes in mind.

### Conditions

- **C5.** Revision 2's text, before the item each shapes: CT-R17 (before item 1 lands), CT-R2's output
  maps, CT-R19 and CT-R20 (before item 3), CT-R18 (before item 5), and question 3's wording. A
  rechecker confirms them; no new architect pass.
- **C6.** K1 gains the mutant "facts written by a reader's map"; K2 gains flush and merge output maps
  under faultfs map failures, a damaged segment in commit's prepare, and the swallowed-merge count;
  K5 gains "twin drops `deletes_dirty`".
- C1 to C4 stand. Lazy segments become the default only after CT-R5 is on `main` (K3).

Not run: strata's test suites, TSan, Linux, FreeBSD, arm64, any benchmark or timing, the earlier
probes.

Probes added in `strata-wt/review-cold-tier/.scratch/ct-review/probes/`: `output_map_probe.cc` and
`output_map_probe.out`; `CMakeLists.txt`, `build.sh` and `run.sh` include it.

## Re-check 2 (C5, C6)

- Date: 2026-10-27.
- Reviewed: revision 2 (`git diff 46ce154..64ee8a4 -- docs/design/cold-tier.md docs/status/cold-tier.md`,
  `e1bfb7f`, `64ee8a4`), checked against this review's CT-R2, CT-R17 to CT-R20, the Q2, Q3 and Q5 items
  and C6; every `index.hh` citation read at strata `main` `8798fe1` (`git show 8798fe1:include/strata/core/index.hh`).
- Checked by: reading the text and the cited lines; `output_map_probe.out` read, not rerun (the claim
  it carries, that a failed output map fences today, is unchanged by a text-only revision).
- Not run: the probes, any build, strata's tests, TSan, Linux, FreeBSD, arm64, any benchmark.

### Verdict

**Step 1 is clear.** C5 and C6 are closed. Nothing blocks starting items 1 and 2, and items 3 and 5 have
the text they were waiting on. Four nits below, all wording or citation; none needs a re-check.

### Status

- **CT-R2 (output maps, `install()`'s remap, bytes in memory, `:4032`): closed.**
  - Output kind: D9 has four kinds and two output rows (`cold-tier.md:192-197`, table rows for merge
    output and flush output). The rule (`:226-233`) keeps the bytes on a host error from `map()`:
    `written_segment` holds them (`index.hh:2281-2286`), `streamed_segment` reads back through its own
    file (`:2296`), and the merge reads `file` the same way instead of reopening (`:2769-2772`), so no
    new descriptor. A host error no longer reaches `fence` (`:2830-2831`) or `guarded()`. The probe's
    fence (`output_map_probe.out`) is cited as today's behavior.
  - The debug assertion is split: no input map in `guarded()`, a destructor or a `noexcept` chore;
    output maps there never throw a host error (`:259-260`). That fixes the trip on every flush.
  - T5's faultfs gains `map()` failures on flush and merge outputs (`:340`); K2 lists them with
    `output_map_probe` as the oracle (`status/cold-tier.md:46-47`).
  - `install()`'s copies (`:1405-1413`) have a row: lazy is an unmapped record with facts from the
    incoming body; eager is the map with today's rule, to the host, unfenced (`:1405-1406`).
  - Citation `:4032` for `live_length()` of a segment with new deletes: correct (`index.hh:4032`).
- **CT-R17: closed.** D2: facts are written only by the owner, under the lease, where it makes a body,
  in commit's prepare, at scheduling and into twins; a reader's map leaves them in the body; the owner
  reads the record's facts if known, else the body's after an acquire load (`cold-tier.md:70-73`). T6 has
  "facts written by a reader's map (under TSan)" (`:342`); K1 has it. The reads it protects are
  `index.hh:4074-4080`, which the table now sends to the facts.
- **CT-R18: closed.** D3 lists `ref`, the facts, the bits (shared), `dead`, `deletes_dirty`, `damaged`
  and the failed marker, and says why (`cold-tier.md:98-104`: `commit_files()` writes a live-docs file only
  where `deletes_dirty` is set, `index.hh:4023-4035`, else keeps the old generation, `:4053-4057`). Step 1's
  item 5 names the fields; T6 and K5 have "twin drops `deletes_dirty`" with a reopen as the oracle. A record
  with `unsynced` cannot be twinned: warm excludes "no unsynced record" (`:268`), so that field needs no
  copy.
- **CT-R19: closed.** D9 has "Commit with a marked record": unfenced `corrupt_segment` from the prepare,
  `needs_heal()` reports markers, the host heals and commits again (`cold-tier.md:245-249`; D11 `:295`;
  the live_length row, `:205`; Risks, `:375`). The prepare skips records whose `dead` equals `ref.doc_count`
  (the erase at `index.hh:4006` drops them) and takes `total_length` from `ref.live_length` when there is
  no live-docs generation, which `:4053-4057` keeps equal to `total_length()`; the copy happens in the
  prepare, before `commit_files()` rewrites `ref.live_length` (`:4050`), so a record taking its first
  deletes in this commit still gets the right value. K2 tests a damaged segment's first map in the
  prepare, with `needs_heal()` and a commit after `heal()`. Q5's (b) now names the coupling.
- **CT-R20: closed.** D9, "Scheduling": a source failing its checks is marked and skipped, never
  swallowed; only `maybe_merge()` (`index.hh:786-793`) swallows a host error from the map, beside
  `ENOSPC`; `merge()`, `force_merge()` and `force_merge_deletes()` rethrow; each swallowed round counts
  on an atomic the host reads, as `media_retries()`, with the pile-up reason and step 2's reading of it.
  The table row says "source marked, or round skipped". K2 tests `EMFILE` at scheduling (`refresh()`
  succeeds, the count rises, the next round merges); step 1's item 3 lists the count.
- **Q2 (marker against I1): closed.** The record's destructor frees a body, never the static marker;
  the loser of either race destroys only its own body and uses what won; a good body losing to the marker
  throws `corrupt_segment` (`cold-tier.md:77-84`); twins carry the marker (D3); `needs_heal()` sees it
  (D9, D11).
- **Q3 (prepare's outcome): closed.** "As `refresh()` leaves it": flushed segments and installed merges in
  `segs_`, unpublished until the next `publish_point()`, nothing durable, `applied_since_commit_`
  unchanged, nothing fenced, installed facts and bodies true and kept; placed after `:819-821` and
  before `:822` (`cold-tier.md:235-243`). "With the index unchanged" is gone from the design; the ledger's
  K2 says the same. The D10 nit is taken: `posted_` empty still waits for a reported task's tail, and
  debug builds assert no task is unrun (`:274-276`).
- **Q5: closed.** Ruled (a) for step 1 and decided on numbers before step 2; (b)'s "a commit never maps" is
  corrected to "maps only records with new deletes and no body (`:4032`, `:4102-4107`)", and (b) is
  credited with removing the damage coupling (`cold-tier.md:446-451`). T2w, the write-after-open row on the
  10-segment fixture (maps and time of the first commit against `eager_segments`), is reported, not gated
  (`:338`), and the ledger's blocked-on line says the same.
- **C6: closed.** K1 gains "facts written by a reader's map"; K2 gains flush and merge output maps under
  faultfs `map()` failures, a damaged segment in commit's prepare, and the swallowed-round count; K5 gains
  "twin drops `deletes_dirty`" (`status/cold-tier.md:39-53`). The design's T5 and T6 rows carry the same
  items, so the two documents agree.

### Nits (no re-check needed)

1. D9's merge-output row cites `:2770-2773` and the text says the merge "opens the file again, `:2771-2772`";
   the `map()` is `:2769`, the reopen `:2770` and its charge `:2771`. Fix the citations when the table is
   transcribed to code comments.
2. D3 says `heal()` "copies the first four of the record's own fields the same way (`:905-908`)". Those
   lines copy the bits, `dead`, `deletes_dirty` and `ref`, not the facts; say that, since the twin's list
   begins with `ref`, the facts, the bits, `dead`.
3. D2 says the owner writes facts "under the lease ... where it makes a body (flush, merge output ...)".
   A merge's output record is built on the pool (`index.hh:2789`); it is unpublished until
   `publish_merges()` installs it, so there is no race, but "before the record is published" is the exact
   condition.
4. K2's "a commit after `heal()` succeeds" assumes `heal()` can replace a marked record whose file is
   still damaged; D11 has `heal()` replace it by reloading, which throws for a segment that is really
   corrupt. The test should use damage `heal()` can repair (a transient `EIO`, or a record whose file was
   restored), or state that a commit then needs the segment dropped.

Not run: any build, strata's tests, the probes (`output_map_probe` read, not rerun), TSan, Linux, FreeBSD,
arm64, any benchmark.

## Re-check: CT-R5 fix

- Date: 2026-10-27.
- Reviewed: strata `fix/apply-atomic`, one commit `9ee1277` on `45c1ec9` (`include/strata/core/index.hh`
  `apply()` `:664-679`, `abandon()` `:703-711`, `writable_buffer()` `:3550-3562`; `test/apply_atomic_test.cc`).
- Checked by: detached worktree `strata-wt/recheck-ct-r5` at `9ee1277`; ASan and UBSan Debug builds (Homebrew
  LLVM) under `quiet light`; the architect's `apply_throw_probe` copied unchanged; the fix's test; a new
  state and allocation-failure probe; seven mutants. Probes and sweep outputs stay in that worktree's
  `.scratch/ct-r5/` (`r5_state_probe.cc`, `sweep.sh`, `mutant.sh`, `sweep*.out`).

### Verdict

**CT-R5 is CLOSED** for the case the finding names (a stage-1 segment read that throws): the probe and the test
pass with the fix and fail without it, and a failed `apply()` leaves the index equal to a twin that never
saw it. One new, narrower finding (CT-R5a): an allocation failure inside two stage-1 steps still escapes the
undo log, which the commit message and the `apply()` comment claim to cover.

### Checks

1. **`apply_throw_probe`: passes with the fix, fails at `45c1ec9`.**
   - With the fix: `[keys] ... corrupt_segment: keys checksum`, then `{add Q3}` verdict 0, `Q2 = b`, "the failed
     transaction is not visible".
   - With `index.hh` reverted to `45c1ec9` (`git apply -R` of the commit's `include/` diff): `Q2 = b2`, "HALF
     VISIBLE", as in `apply_throw_probe.out`.
2. **`apply_atomic` fails with the fix reverted.** Reverted `index.hh`, same test: `apply_atomic: FAILED`,
   exit 1, with `:79` (`Q2` not `b`), `:81` (`Q9` visible), `:73` and `:74` (metadata `m` kept); with the fix
   `apply_atomic: ok`, exit 0, under ASan and UBSan.
3. **Every throwing path in stage 1 leaves no partial state, for a segment-read throw.**
   - `rollback()` (`:3430-3466`) replays the undo log in reverse: key and docid slots, pending delete and its
     `pending_keys` entry, `deletes_dirty` (`segment_dirty`, `:3477`), `last_docid`, `first_docid_only`,
     metadata with `meta_dirty_`. Then `order.rewind(mark_.records)` and `docs = mark_.docs`.
   - `mark_` (`:3553`) is now set right after `wb_` exists and before `keys.reserve`, `build_docids` and
     `docids.reserve`, so it is never stale from an earlier transaction. With `make_shared` throwing, `wb_`
     stays null, `undo_` is empty and `rollback()` does nothing (`if (b)`, `:3454`). With a fresh `wb_`
     the mark is empty and the rewind is a no-op.
   - `keep = pub_ && live_reads()` (`:3457`): with a reader the lane and `b->bytes` are kept (rejected records
     stay unreferenced and counted, as for a rejected verdict); `order` and `docs` still rewind. Without one
     the lane truncates to the mark.
   - `pending_docs_` cleared, the `reserve` returned and `charge()` refreshed in `abandon()` (`:705-711`).
     `commit_allowance()` between `evaluate()` and the verdict does not allocate or throw.
   - Outside the undo log: `transactions`, `visible_`, `m_.applied`, `allowance_`, `reserved_`,
     `applied_since_commit_`, schema and `invert_pending()` all move only after the accepted branch (`:681-691`),
     which a throw skips, so the position stays as before the call. Segment reads only fill the segments'
     own caches. Nothing else in `evaluate()` mutates.
   - `r5_state_probe` (four variants: reader live or not, earlier buffered records or not; the damaged
     `keys` section, `corrupt_segment` at `N=0`): reads, metadata, the storage budget, the follow-up
     `{add Q3, set_meta z}` and the committed file sizes equal a twin that never saw the failing
     transaction. The memory budget is not compared (map capacity grows by design).
4. **Mutants.**
   - Caught by `apply_atomic`: skip the `pending` restore (`:79`, `Q2` deleted at the next freeze); skip the
     `meta` restore (`:73-74`); the catch returns `v` instead of rethrowing (`:72`).
   - Not caught by `apply_atomic`, caught by `r5_state_probe`: drop `storage_permit_.shrink(reserve)` from
     `abandon()` (storage budget grows on every throw, 556 runs); put `mark_` back after `build_docids` (8 more
     crashes in the allocation sweep). Not caught by either: `keep = true` always (harmless; it hides the
     CT-R5a crash).
   - Reverted fix under the allocation sweep: 28 atomic, 524 leaking, 0 crashes. With the fix: 532 atomic, 24
     leaking, 4 crashing (of 560).

### New finding

**CT-R5a. An allocation failure between a mutation and its undo entry is not rolled back (low; fix with the
follow-up, not required before Q3).**
- `remove_copy()` pushes `pending` and inserts into `pending_keys` (`:3474-3475`) before logging the undo step
  (`:3476`), and `push_record()` stores the key slot (`:3517`) before `undo_.push_back` (`:3518`). A
  `bad_alloc` in the hash node, the bucket array or `undo_`'s growth leaves the entry or slot unlogged.
- `r5_state_probe` (fail the Nth `operator new` inside `apply()`; `sweep.sh` over 8 variants, N up to the
  last allocation of the call): 24 runs leave partial state (a pending delete that the next freeze applies, so `Q2`
  vanishes and a segment is missing; or a stale slot or record); 4 runs leave a key slot pointing past the
  truncated lane, and the next `get` is a segfault under ASan (`write_buffer.hh:408` UBSan first). Every
  stack at those points is `remove_copy()` or `push_record()`. The other 532 are atomic,
  including every allocation in `writable_buffer()` (`make_shared`, `keys.reserve`, `build_docids`).
- Present before the fix (it makes the same case atomic in most places); the commit message ("an allocation")
  and the `apply()` comment ("restores every slot") say more than holds.
- Fix: reserve `undo_` for the transaction before the first mutation (a fixed number of steps per operation)
  so logging cannot throw, and log before mutating, or insert into `pending_keys` before `pending`. Add the
  allocation sweep to `apply_atomic` (it needs only a counting `operator new`).

Smaller: `abandon()` ends with `undo_.clear()` and `apply()` clears again at `:710`; the test has no reader and
no storage budget, so `keep` and the reservation are covered only by the probe above.

Not run: strata's other tests and ctest, TSan (already 54/54 clean per the lead), Linux, FreeBSD, arm64, any
benchmark; the probe against a replica digest (D2/Q3 failure sources other than a damaged section).

## Re-check 2 (C5, C6), again at main c82ac9f

- Date: 2026-10-30.
- Why again: the earlier "Re-check 2 (C5, C6)" above read every `index.hh` citation at `8798fe1`. Main is now
  `c82ac9f`, which adds `defer_io` (`4563ed1`, `88b7713`), `work_pool_executor` (`997d1d1`, `4e96b49`), the
  commit barriers, F1 to F3 (fields and keys), and, ahead of those, the cold-tier step 1 code itself (see
  "Step 1 is already on main"). Revision 2's text has not changed since `e1bfb7f` except the X-POOL note
  (`2b35570`, step 2, unrelated to C5 and C6).
- Checked by: reading the design at this worktree's `f8c4080` (identical to `main:docs/design/cold-tier.md`);
  a line-level map of every `index.hh` citation from `8798fe1` to `c82ac9f` (`git diff -U0`; script and both
  files in `.scratch/rc2/`, not committed); reading the cited symbols at `c82ac9f`; `apply()` and the undo log at
  `c82ac9f`; `docs/mechanisms/defer-io.md`, `docs/reviews/phase7c-l5.md` (L5-B1) and the step 1 ledger.
- Not run: any build, test, probe, sanitizer or benchmark (the task forbids them).

### Verdicts

- **C5: CLOSED.** The text that closes it is unchanged and still says what the earlier re-check read:
  CT-R17 in D2 (`cold-tier.md:70-73`; T6's fourth mutant, `:342`), CT-R2's output maps and `install()`'s copies in
  D9 (`:216`, `:226-233`, `:259-260`), CT-R19 (`:245-249`), CT-R20 (`:250-257`), CT-R18 in D3 (`:98-104`) with
  step 1's item 5 and T6, the marker's lifetime (`:77-84`), the prepare's outcome (`:235-243`) and Q5 (`:446-451`).
  The citations are stale as line numbers and still right as symbols; see the next section.
- **C6: CLOSED.** K1 ("facts written by a reader's map"), K2 (flush and merge output maps under faultfs `map()`
  failures with `output_map_probe` as the oracle; a damaged segment's first map in commit's prepare; the
  swallowed-round count) and K5 ("twin drops `deletes_dirty`", a reopen as the oracle) are in
  `docs/status/cold-tier.md` (K1, K2, K5) and in T5 and T6 of the design. The step 1 ledger ticks K1 and K2 with
  named tests and killed mutants (M1 to M5 and the later ones); K5 is not built yet (item 5).

### Citations at c82ac9f

Every line number in the design, and the status ledger's "citations checked at main `8798fe1`", is now stale: the
file grew by 909 lines. Line numbers are right only against `git show 8798fe1:include/strata/core/index.hh`.
Shifts for the unchanged lines (new = old + shift):

| Old range | Shift | Examples (old to new) |
|---|---|---|
| to about 800 | +0 to +93 | `:725-739` to `:818-832` (`check_in`), `:786-793` to `:879-890` (`maybe_merge`) |
| 800 to 1400 | +93 to +127 | `:879` to `:993` (`heal`), `:1244-1250` to `:1371-1377` |
| 2200 to 2900 | +270 to +300 | `:2379` to `:2664` (`publish_point`), `:2479-2493` to `:2769-2783`, `:2341-2353` to `:2626-2638` |
| 2900 to 3400 | +300 to +302 | `:2977-2980` to `:3278-3281`, `:3063-3081` to `:3364-3382`, `:3103-3110` to `:3404-3411` |
| 3700 to 4150 | +379 to +401 | `:3702` to `:4081` (`buffered_records`), `:4032` to `:4436`, `:4145-4154` to `:4593-4602` |
| 4300 to 4550 | +448 to +517 | `:4365` to `:4821`, `:4414-4418` to `:4871-4875`, `:4512` to `:5027`, `:4547` to `:5064` |

Not a shift but a rewrite, because step 1's code replaced what the lines described (read these by symbol at
`c82ac9f`): `open_segment` (`:2093-2111`, `:2103-2105`, `:2109`) and `segment_view` (`:926-931`, `:958`,
`:2116-2118`) are now the record and body of D2; `commit_files()` (`:4006`, `:4023-4057`, `:4074-4080`,
`:4102-4107`) and `heal()` (`:886-910`) and `needs_heal()` (`:804-807`); `written_segment` and `streamed_segment`
(`:2280-2302`); the merge's output reopen (`:2769-2773`); `install()` (`:1370`, `:1384-1413`); `check()` (`:1325`);
`media_retries_` (`:4533`); and the open path (`:4318`, `:4461-4497`).

Still correct in meaning at their new places: the `check_in()` and `maybe_merge()` anchors, `publish_point()`'s
skip of an unchanged state, `free_views()`, `end_merges()`/`end_deletes()`/`collect_deletes()`,
`buffered_records()`, `pending_`, `applied_since_commit_`, the reclaim queue and the `guarded()` body.
Two other staleness points in the text: T5 (`:341`) names `pool_executor`, which is deleted (`997d1d1`) and
replaced by `work_pool_executor`; D10's "which a chore must never do" (`:274`) is now a choice rather than a rule
of the executor, because `defer-io.md` allows a host chore to wait on its own index's merge and delete groups
(depth 2). Neither changes C5 or C6.

### Step 1 is already on main

The question "can items 1 and 2 start" has been overtaken. `phase/cold-tier-step1` landed (`66ddb16`, with
`d0ab622` the record/body split, items 1 and 2 with D9's map points), after the CT-R5 rollback (`eb87572`:
`9ee1277`, and `d3a59ee` for CT-R5a), and its code review (`docs/reviews/cold-tier-step1-code.md`, approve with
conditions, CR1 to CR4 and CR8 folded by `444c26f`) is on main. `docs/status/cold-tier.md` on main and in this
worktree still says "Blocked on: CT-R5's rollback landing on main" and "items 1 and 2 may start"; both are
stale and should be rewritten from `docs/status/cold-tier-step1.md`.

- **K3: satisfied in code, not re-run here.** `apply()` at `index.hh:697-760` keeps the undo log, `abandon(reserve)` on
  a throw, and rethrows; `rollback()` at `:3740`. CT-R5a is in: `remove_copy()` logs `pending` before the key insert
  and `reserve_undo(2)` precedes both it and `push_record()`'s slot stores (`:3780-3860`, `d3a59ee`).
  `test/apply_atomic_test.cc` has the counting `operator new` sweep (skipped under TSan, `:22-27`, `:192`). I read
  these; I did not rerun `apply_throw_probe`, the sweep, or the revert check, which need a build. What K3 still
  needs: a rechecker with a build to run the sweep and confirm `d3a59ee` (the CT-R5 re-check above closed CT-R5 and
  opened CT-R5a, and no re-check of the R5a fix is on file).
- **Item 6 (lazy as the default) is no longer gated on CT-R5.** It is gated on the interaction below, K4's twelve
  damage cases, and the two call-site pin bumps (K7). The comment at `index.hh:519-521` ("until apply() rolls back
  (cold-tier step 1, item 6)") is stale in its first half.

### New finding

**CT-R21. The design never meets `defer_io`, and `demote()` reopens the hole L5-B1 closed (medium; decide before
item 5, state it in the design before then).**

- `defer_io` forces `eager_segments` on at `open()` (`index.hh:592`) and `create()` (`:4681`), whatever
  `STRATA_EAGER_SEGMENTS` says (`88b7713`), so that `apply` makes no device call
  (`defer-io.md`: "With `defer_io`, `apply` makes no `IO` or `File` call"). That fixes the lazy-open case only.
- Demotion (D3, item 5) makes every record an unmapped twin on a live index. The next `apply` that replaces by
  key or deletes by docid runs `locate_docid`/`lookup_key`, which call `s.seg()` (`index.hh:3684`, `:3703`): a
  map point under the lease, per D9's `apply()` row (`cold-tier.md:208`). On a `defer_io` index that is a file
  open and an `mmap` on the host's reactor thread: L5-B1 again, reached by demotion instead of by the option.
  The forced-eager flag does not prevent it; nothing in D10's warm predicate mentions `defer_io`.
  Reasoned from the code, not reproduced (no build).
- The L5 review ruled the owed open "cold-tier step 1's" (`phase7c-l5.md`, ruling on 2b and the L5-B1 fix text).
  `grep` finds no `defer_io`, owed map or `map_owed` in `cold-tier.md`, `status/cold-tier.md`,
  `status/cold-tier-step1.md` or `mechanisms/segment-records.md`: that obligation was dropped.
- It is not an edge case. `defer_io` is the mode Xapiand's reactors will run (phase7c D4, D6), and Xapiand is the
  many-indexes workload this design exists for. Under forced eager, T1's cold row (0 maps) and T2's cold-open
  rows do not describe a `defer_io` index at all: it maps everything at open.
- Needed, one of: (a) D10's warm predicate excludes `defer_io` indexes, so they are hot or cold only, and cold
  reopens eager (cheapest; keeps L5's claim; costs the warm tier for Xapiand and says so in N1); (b) a `defer_io`
  index is mapped by a host chore before its next `apply` (the catalog's wake is the natural place: a pin that
  wakes a demoted index maps its records as a chore) with a debug assert in `apply` that nothing is unmapped; (c) an
  owed-map verdict as L5 described (it must be raised from inside `evaluate` mid-transaction, which L5 judged
  heavy). Recommendation: (a) for item 5, (b) in step 2 with the catalog; either way write it into D3, D10 and D9's
  `apply()` row, add T5/T6 rows "demote then apply under `defer_io` makes zero `faultfs::calls()`", and drop the
  `pool_executor` name from T5.

Smaller, outside this scope, one line each for the lead: `defer-io.md` "Not covered" (the unmapped-segment bullet)
is stale after `88b7713`; `index.hh:519-521` as noted above.

### Go or no-go

Items 1 and 2: moot, they are on main and reviewed; nothing in C5, C6 or the new main blocks building on them.
Item 3 (D9's failure rules) is also in. **Item 5 (`demote()`) is no-go until CT-R21 is written into the design.**
Item 6 waits on CT-R21, K4 and K7, not on CT-R5.

Not run: any build, strata's tests, `apply_throw_probe`, the allocation sweep or the revert check, TSan,
Linux, FreeBSD, arm64, any benchmark.

## Re-check 4 (revision 3, CT-R21)

- Date: 2026-10-30.
- Reviewed: `docs/design/cold-tier.md` revision 3 and `docs/status/cold-tier.md` at `da7d527`
  (`git diff e44d98b..da7d527 -- docs/`: `c49c4b2`, `da7d527`); strata `main` `3129c39`, whose
  `include/` differs from the cited `c82ac9f` only in the `eager_segments` comment
  (`index.hh:519-521`): `apply()` `:697-760`, `heal()` `:993-1035`, `writer_scope` `:2608-2621`,
  `open_segment` (`seg()`, `map_body()`, `fail()`) `:2340-2385`, `locate_docid`/`lookup_key`
  `:3684`, `:3705`, `load_files`/`load_segment`/`load_record` `:4893-4958`, `close()` `:638-650`,
  `live()` `:2533-2536`; `docs/mechanisms/defer-io.md`; `work_pool_executor.hh:1-11`; Xapiand
  `docs/design/phase7c.md` D2, D5, D6 (local, `e265acee6`).
- Checked against: CT-R21 and its recommendation; defer-io.md's C15 rule ("owed answers end") and
  Xapiand D6.2's retry before any later position (`phase7c.md:259-262`); I1, I8; K5; calibration
  (mutants that a default option makes equivalent; numbers compared across platforms).
- Checked by: every `open_segment` construction site at `3129c39` (`:1548`, `:2565`, `:3089`,
  `:4943`, `:4955`) traced to whether it leaves a record without a body under forced eager; 40
  cited lines printed and read; one probe in `.scratch/arch-r3/` under a light hold:
  - `owed_model.py` (`owed_model.out`): an explicit-state model of one host turn answering
    `map_owed` by `promote()` and retrying, with an adversary that may `demote()` whenever the lease
    is free and warm's predicate holds, in three variants (revision 3 as written; the turn holds a
    pin the sweep respects; strata refuses `demote()` while a `map_owed` answer is outstanding).
  - Not run: any build, strata's tests, TSan, ASan, UBSan, Linux, FreeBSD, arm64, any benchmark
    (design text; `demote()` and `promote()` do not exist yet).

### Verdict

**Item 5 (`demote()`, `promote()`, `map_owed`): GO WITH CONDITIONS.** The decision is sound for
every owner path under `defer_io`: the premise of I8 holds in today's code, the check sits where
`flush_owed` sits, and no merge, commit or heal reaches an unmapped record outside a chore. Two
things must be in the design and K5 before the branch's code review, not before it starts:
CT-R22 (the answer can repeat) and CT-R25 (K5 rules a weak test can tick).

**Item 6 (lazy as the default): NO-GO, unchanged in kind.** Nothing in revision 3 blocks it at
the design level, and `defer_io`'s forced eager open stays, so CT-R23 does not touch it. It waits on
built evidence: K4's twelve cases, K7's two pin bumps, K3's rechecker run, and item 5's K5 tests.

What holds, and reproduces:

- **I8's premise, at `3129c39`.** Under `defer_io` (`eager_segments` forced, `:592`, `:4681`) every
  site that makes a record gives it a body: open goes through `load_segment` (`:4802` passes no
  sizes, `:4900`, `:4944`), `install()`'s copies take the eager branch (`:1547`, `:1557`), flush and
  merge outputs adopt (`:2565-2575`, `:3089-3090`), and `heal()` reloads whole (`:1016`). Only
  `load_record` (`:4955`) and `install()`'s lazy branch (`:1548`) make unbodied records, and both
  are reached only with `eager_segments` off. So unmapped records under `defer_io` exist only
  between `demote()` and `promote()`, as D3 says.
- **The check's place.** `apply()` runs `live()` (`:700`), the replay check (`:705-708`), then
  `flush_owed` (`:711-715`); nothing before `evaluate()` (`:743`) reads a segment (`flush_due`,
  `lane_fits`, `estimate_bytes`, `drain_pending` are buffer and permit state). An index-wide flag
  read between the replay check and `flush_owed` therefore covers both stage-1 map points
  (`:3684`, `:3705`) and applies nothing; a fenced or closed index throws from `live()` first.
- **Owner paths other than `apply`.** Merge scheduling maps under the lease (`:2811-2821`), commit's
  prepare maps (`:921`), `heal()` maps twins only when `damaged` (`:1002`); all three run as host
  chores under Xapiand D5 and D7, never on a reactor, so they need no owed answer and may leave the
  flag set (the next `promote()` maps what is left). No owner path replaces a bodied record with an
  unbodied one except `demote()`.
- **Races.** `demote()`, `promote()`, `apply`, `heal()` and `close()` all hold the lease
  (`writer_scope`, `chore_scope`), so the flag needs no ordering of its own while only they touch
  it: the lease's handoff orders the store and the load. A reader racing `promote()` on one record is
  D2's install race (compare-exchange, the loser destroys its own body, `:2369-2383`); a reader
  that installs the marker first makes `promote()`'s map throw `corrupt_segment` through
  `throw_failed()`, which `promote()` must count as done (K5 rule 7 covers the fresh-failure case).
  `close()` sets `fenced_` (`:649`), so a `promote()` posted after a close throws `index_fenced`
  from `live()`.
- **Not L5's heavy form.** The flag is index-wide and read once per apply; nothing is raised from
  inside `evaluate()`. CT-R21's options (b) and (c) are combined as the review recommended, and the
  reason for not taking (a) (Xapiand's idle indexes would be hot or cold only) is right on memory
  and maps alone (T1), whatever the latency turns out to be (CT-R24).

What blocks item 5's landing: CT-R22, CT-R25. Before step 2: CT-R23, CT-R24's row. Low: CT-R26.

### Status

- **CT-R21: CLOSED for `apply`; the read side continues as CT-R23.** Stated in D3 ("Warm under
  `defer_io`", `cold-tier.md:129-163`), D9's `apply()` and `promote()` rows (`:254-255`) and
  no-input-map rule (`:308-310`), D10 (`:320-321`), I8 (`:378-380`), T1, T2, T5, T6 (`:395-401`),
  step 1 item 5 (`:419-422`) and K5 rules 6 and 7 (`status/cold-tier.md`). T5 names
  `work_pool_executor`.
- **Citations: refreshed and right in meaning.** Of 40 printed: `:592`, `:4681`, `:383-387`,
  `:2372`, `:2314`, `:2262-2263`, `:2303-2304`, `:2308`, `:993`, `:1022-1025`, `:1371-1377`,
  `:1547`, `:2569-2575`, `:2664`, `:2674-2675`, `:2693-2698`, `:2769-2783`, `:818-832`, `:3090`,
  `:3159-3171`, `:3278-3281`, `:3329-3331`, `:3364-3382`, `:3404-3411`, `:4081`, `:4393`,
  `:4407-4410`, `:4427-4439`, `:4436`, `:4500-4501`, `:4549-4556`, `:4593-4602`, `:4768`, `:4821`,
  `:4859`, `:4871-4875`, `:4949-4958`, `:5027`, `:5064`, `:1706-1711`, `:2811-2821`, `:1454-1456`
  all name what the text says. Off by a little: `lookup_key`'s map is `:3705`, not `:3703`;
  `flush_owed` is `:710-715` inside the cited `:706-715`. Xapiand D6.2 is `phase7c.md:259-262`.
- **Ledger, stale at `3129c39`.** "Blocked on ... For the lead" asks for two edits that are on
  `main`: `defer-io.md:42` no longer has the unmapped-segment bullet, and `index.hh:519-521` already
  reads "until K4, K7 and item 5's CT-R21 tests are in". Drop the bullet.

### New findings

**CT-R22. "The answer ends" does not hold: a `demote()` between `promote()` and the retry answers
`map_owed` again, without bound (medium; before item 5 lands).**
- D3 (`:142-145`) and K5 rule 7 argue termination from "a `promote()` that completes clears the
  flag". Between that chore and the retry the lease is free, and warm's predicate still holds
  (`map_owed` applied nothing, so there is no buffered record). In step 1 the host calls `demote()`
  itself; in step 2 the sweep compare-exchanges `{0, open}` (D5), which excludes only a pin.
  Xapiand's readers take no checkout (`phase7c.md:215`), and nothing in D3 says the writer's turn
  holds a pin across the owed retry.
- `owed_model.out`: revision 3 as written, a cycle (map_owed, promote, demote, map_owed, ...) with no
  bound; with a pin held across the retry, at most 1; with strata refusing `demote()` while an
  answer is outstanding, at most 1.
- defer-io.md makes the other two answers end inside strata (`drain_asked_`/`drain_ran_`,
  `index.hh:724-736`; the buffer test for `flush_owed`, `:711`), not by a host rule.
- Fix: the same shape for `map_owed`. A bit set when `apply` answers `map_owed`, cleared by the next
  apply that passes the check and by `refresh()`, `commit()` and `close()`; `demote()` returns false
  while it is set. State the turn's bound in D3: at most one of each owed answer, in the order map,
  flush, drain, so at most three retries; Xapiand's D6.2 ("retries the apply once more") becomes
  that loop when it adopts warm. K5 rule 7 gains: `demote()` between `promote()` and the retry
  returns false, and the retry is accepted; mutant: drop the bit.

**CT-R23. Reads have no backstop: a `defer_io` host that reads a demoted index maps on its
reactor, silently in release builds (medium; before step 2, or before any host demotes a
`defer_io` index).**
- D3 (`:149-153`) relies on step 2's catalog promoting at a pin's wake. Xapiand runs every read on
  a reactor (`phase7c.md:156`) and readers take no checkout (`:215`), so a read-only client never
  pins and never wakes; its first `get` or scan after a demotion opens and maps a file on the
  reactor: L5-B1 for reads, which `defer_io`'s forced eager open closes today. `no_input_map_scope`
  covers only stage 1, so no debug build catches it either.
- Needed before step 2, one of: reads pin through the catalog so the wake promotes; step 3's
  sentinel ships with step 2 for `defer_io` indexes, so the read throws `index_unpublished` and the
  host promotes; or `demote()` refuses a `defer_io` index until one of those exists. In every case
  an observable: a count of maps outside `promote()` and the owner's chores under `defer_io`
  (beside `load_context::retries`), which Xapiand's gate asserts is zero. Item 5 adds the strata
  half to K5: after `promote()`, a `get` and a scan make no `faultfs::filesystem::calls()`.

**CT-R24. The wake's cost compares a Linux number with a macOS one (low for the text; the row is
needed before step 2).**
- "33 to 50 µs" is the Linux gate VM's first use, the map plus the first query, on the pre-split
  base (`small-indexes.md:283`, `:497`; `idle-index.md:164`). "0.4 to 1.1 ms" is a macOS warm open
  (`idle-index.md:95`). On Linux an open is 53 to 85 µs warm and about 0.5 ms with the cache dropped
  (`idle-index.md:92`, `:211`), so with the cache warm an open and its first query (54 to 92 µs) is 1.1 to 2.8 times the 33 to 50 µs, not 10 to 30 times; on macOS
  the cost of one map is unmeasured. The chore hop (a post to the pool and the resume on the reactor)
  is not estimated at all.
- The decision does not rest on it (warm's 0 maps and smaller heap against hot, T1), so the fix is
  the text: per platform, with the hop named as unmeasured.
- The row is T2's `defer_io` variant; make it a row of its own, **T2d**: on Linux and macOS, 1 and
  10 segments, cache warm and dropped, 100,000 indexes, one wake each in random order: (i) warm,
  `promote()` posted to `work_pool_executor` from a thread off the pool and the resume, then one
  `get`; (ii) cold, the open on the same executor, then one `get`; (iii) hot, the `get`. p50 and p99
  per case, raw files kept, and `promote()`'s time over its segment count reported against the 33
  to 50 µs estimate. Target: warm below cold at every segment count on each platform; where it is
  not, D3's latency sentence goes and the memory argument stands. Lead-run under `quiet exclusive`.

**CT-R25. K5 has rules a weak test ticks (medium; fold into K5 before item 5's code review).**
1. Rule 2 and T6's "twin only bodied records" are equivalent mutants under eager segments, which
   are the default until item 6 and forced under `defer_io`: every record is bodied at `demote()`,
   so twinning only bodied ones twins all. The test must run with `eager_segments = false` (or
   demote twice) and assert at least one record has no body when `demote()` starts.
2. Rule 6's mutant "answer `map_owed` without `defer_io`" is not killed by `default_path_unchanged`,
   which never calls `demote()`, so the flag is never set. Add: without `defer_io`, `demote()` then a
   replace-by-key apply is accepted (and maps).
3. Rule 1's "closing" clause cannot be reached: `close()` sets `fenced_` (`index.hh:649`) and
   `live()` throws `index_fenced` (`:2534`) before any predicate, so dropping the clause is an
   equivalent mutant. Drop the clause, or test "`demote()` and `promote()` after `close()` throw
   `index_fenced`" with the mutant "skip `live()`".
4. Rule 1's "changes nothing" omits the flag. Under `defer_io`, an apply after a refused `demote()`
   is accepted with `calls()` unchanged; mutant: set the flag before the predicate.
5. Rule 4's "0 maps" must count every body the index keeps, retired views included (faultfs's live
   mappings or a count of body destructors), not `segs_`; counted over `segs_`, "skip
   `free_views()`" survives, since the twins are already there.
6. Rule 9 (T5) passes with zero reports from a schedule that never overlaps. Report counters that
   show the interleavings ran: late maps through a retired view, install compare-exchange losses,
   and a `promote()` racing a reader's map, each at least 1.
7. Rule 7 adds a twin that carries the marker from before `demote()`: `promote()` completes and
   clears the flag. Mutant: `promote()` rethrows the `corrupt_segment` of a marked record.
8. CT-R22's and CT-R23's tests above.

**CT-R26. The flag's type and two small rules (low).**
- A plain owner field is sound for item 5 (all access under the lease). Step 2's wake wants to skip
  the chore hop for an index with nothing owed, the common pin, and reads it without the lease:
  make it `std::atomic<bool>` (release store, acquire load) behind `promote_owed()`. A stale read is
  harmless, since `map_owed` backstops it. `promote()` with the flag clear makes no `calls()`; test it.
- A debug assertion at the check: with the flag clear under `defer_io`, every record in `segs_` is
  bodied or marked. It catches a future creation site that breaks I8's premise, which
  `no_input_map_scope` catches only when a test happens to reach that record in stage 1.

### Blocking items

- Before item 5's code review: CT-R22 in D3 and K5; CT-R25 in K5 (and T6's mutant list).
- Before step 2, or before any host demotes a `defer_io` index: CT-R23; CT-R24's T2d row.

### Conditions

- C7. Item 5's ledger lists each K5 rule with its test, its mutant and the mutant's result, as
  step 1's `mutants.py` did, and states the options each test runs with (`eager_segments`,
  `defer_io`).
- C8. When Xapiand adopts warm (step 2), its D6.2 handles `map_owed` with the bounded loop of
  CT-R22, and its gate asserts CT-R23's count is zero.

Probes kept in `strata-wt/cold-tier/.scratch/arch-r3/`: `owed_model.py`, `owed_model.out`. Rerun
with `quiet light ... -- python3 -I owed_model.py`.

Not run: any build, strata's tests, TSan, ASan, UBSan, Linux, FreeBSD, arm64, any benchmark or
timing; the earlier probes (`twin_race`, `output_map_probe`) were not rerun.
