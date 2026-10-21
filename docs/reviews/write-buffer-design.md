# strata review: the write buffer (preliminary design review)

- Date: 2026-10-21.
- Reviewed: `docs/design/write-buffer.md` revision 0 at `56fd828` and its ledger
  `docs/status/write-buffer.md` at `6d6644b` (branch `design/write-modes`; HEAD equals `origin`;
  `6d6644b` adds only the owner's answers to Q1 to Q3 to the ledger, after `acdc908`). With it:
  `design/shared-reader` revision 2 at `b1568a7` (its review is appended to
  `docs/reviews/shared-reader-design.md` there); the core branch `phase4/shared-reader-core` at
  `41fde48` plus its uncommitted working tree (`hazard.hh`, `owner.hh`, `deleted_bits.hh`, the
  `index.hh` diff); the step-0 prototype branch `perf/lanebuf` at `7a6d1b9` plus its uncommitted
  `bench/kv/proto/lanebuf.hh` (read for what the bake-off will measure, not reviewed as code).
- Checked against: the owner's goals (`write-buffer.md:29-42`) and answers (ledger `:39-45`);
  `ARCHITECTURE.md` write path (`:67-112`), concurrency and the slice (`:163-185`), memory ownership
  (`:188-192`); `reviews/stage-1.md` ruling (c) (`:315-337`); `format-14-kv.md` D3 (`:266-328`) and
  T6 (`:384`) on `design/format-14-kv`, with its raw `report.md:79-84` and `bufmem.jsonl.gz`;
  `docs/perf/buffer-bytes.md` on `explore/buffer-bytes`; `durability-modes.md` D1 (`:775-781`,
  `:945-953`) and D10 (`:2031-2080`) at `95357d9`; `track-c.md` D11 (`:460-626`); Kronuz/journal
  `9c87929` (`turn_driver.h`, `turn_log.h`); `index.hh` at `41fde48` (`install_merge`'s carried
  deletes, `:1776-1804`); RocksDB 11.11 (`write_thread.cc:449-454`, `db_impl_write.cc:1258-1262`);
  `reviews/calibration.md` (every row applied: the catalog searched for an existing write queue,
  concurrent map, or reclamation scheme and none found (`queue` is a blocking deque, `stash` a timer
  slot store, `epoch` a clock); the cited baselines' raw files found; HEAD against origin; limits
  against the load-bearing rule; every mechanism a fix relies on located in code or named as owed).
- Checked by: probes in `.scratch/review/` on this worktree, Homebrew clang 23.1.2, macOS x86_64
  (i9-9980HK), under light `quiet run` holds:
  - `vmap_stress.cc`, `build.sh`, `run.sh`, output `runs.out`: D4's single-writer, many-reader
    version map as written (atomic 8-byte slots, record header before the slot's release store, no
    erase, doubling at 7/8 with the old table kept, a rejected transaction marked "never" and its
    slot restored, V stored with release per group of 8), one leader, readers checking every answer
    against the closed form of the schedule. Natively 3,000,000 positions, 400,000 keys, 4 readers,
    15 growths; under TSan 120,000 positions, 3 readers, 12 growths. Five mutants.
  - `view_model.py`, output `view_model.out`: a deterministic model of D5, D6, and D8 (positions,
    shadowing, pending segment deletes, freeze, flush, merge install), for WB-1 and WB-2.
  - `bytes.py`, output `bytes.out`: the version map's bytes per record at T6's measurement points.
  - Not run: no strata build or test (design only); no benchmark or timing; no arm64, Linux, or
    FreeBSD host; no model checker; prospect untouched; the step-0 prototype not built or run.

## Verdict

**REJECT: candidate (C) is the right shape and its version map holds under the probe, but six
gaps in revision 0 (WB-1 to WB-6) give torn reads, resurrected deletes, frozen buffers still being
written, scans with no way to find their records, and a bake-off that decides a path the
production presets do not run. Revision 1 fixes them before step 1; step 0 may continue under
C1.**

What holds, and reproduces:

- D4's map protocol as written. `runs.out`: `vm_native_correct` 4,261,195 reads with 0 wrong and 0
  torn across 15 growths; `vm_tsan_correct` clean with 529,752 reads. The release store of the slot
  after the header, the acquire load, no erase, and "the leader stops writing the old table only
  after the new one holds everything" (`write-buffer.md:229-237`) are enough for a reader that
  loads V before the table pointer. `prev` chains stayed at most 1 long (mean 0.0000-0.0002).
- The mutants the design names die where expected: V published before stage 1 ends
  (`vm_*_v_early`: 33 wrong natively, 53 under TSan); a table published before it is filled
  (`vm_*_grow_early`: 23,897 and 8,488 wrong); a slot stored relaxed and "never" written to a plain
  field (`vm_tsan_relaxed_slot`, `vm_tsan_plain_never`: TSan races at `vmap_stress.cc:88` and
  `:50`). See WB-9 for what native runs miss.
- D1's choice of (C) over (B) and its reasons; (C) over A' as the starting recommendation, since
  A' is (C) plus a skip list and can only win on T3 (WB-8 sharpens the rule).
- D2's chores as writerless groups and D12's suspend-don't-block seam.
- The stage-1 ruling's reason survives (WB-13).

What blocks: WB-1 to WB-6, each with one line under "Blocking items".

## Findings

1. **WB-1. A reader that loads V and then acquires a newer view reads positionless state past V:
   torn reads (required).**
   - D5 has the reader load V, acquire a view, and read at V (`write-buffer.md:257-263`). Records
     in buffers carry positions, so `prev` hides what is above V. But two parts of a view carry
     none: segment live bits, which D6 and D8 set at the freeze from the pending segment deletes
     (`:294-299`, `:346-348`), and segments a flush or merge installs (`:355-358`).
   - A freeze or install between the reader's load of V and its acquire hands it a view whose bits
     include deletes above V. `view_model.out`: a transaction moving 10 from `a` to `b` (both in a
     segment, sum 100), then a freeze; the reader that loaded V = 0 reads `a = None, b = None,
     sum = 0`: the delete half of the transaction without its add. After a flush's install it
     reads the later state (`a = 40, b = 60`) while claiming position 0.
   - SR-D15's argument (`:261-263`) shows the view covers V; nothing stops it from being past V.
   - Fix: a reader reads at `max(V, view.position)`, where a view's position is the position all
     of its positionless state reflects (the freeze's F, an install's group boundary). Both reads
     in `view_model.out` then return the transaction whole. V is monotonic per reader either way.
     With WB-6, a view's position never exceeds the acknowledged prefix. T8's model checks at that
     position, and its schedule freezes and installs between a reader's load of V and its acquire;
     a mutant reading at V must fail it.

2. **WB-2. A delete pending against a merge's source is lost when the merge installs, and the
   deleted key comes back (required).**
   - D6 keeps deletes of segment copies in the buffer's pending list until the freeze (`:294-299`).
     `install_merge` carries into the output only the bits set on the sources since scheduling
     (`index.hh:1776-1795` at `41fde48`), so a delete still pending at install is in neither. The
     freeze later applies `(source id, ordinal)` to a segment that is gone.
   - Shadowing hides it until the buffer flushes. Then two live copies of the key exist (the
     merge's output and the flushed one), which breaks format 14's one-live-copy invariant
     (`format-14-kv.md:283-286`), and segments hold no tombstones (format-13 F8), so a later delete
     resolves to the newest copy and exposes the stale one. `view_model.out`: "after flush: get k =
     40 (live copies of k: 2)", then "after delete, freeze, flush: get k = 50 RESURRECTED".
   - `heal()` (same segment id, re-read) and `install()` (everything replaced) need the same rule.
   - Fix: a merge's install (a lease operation) carries the pending deletes that name its sources:
     either it remaps them through `plan.ordinals` into pending deletes of the output, or it
     applies them to the sources' bits first, at a group boundary whose position the published view
     then carries (WB-1). T8 schedules merges installing with deletes pending on their sources, and
     a mutant that drops them must fail; T9's replay compares digests after it.

3. **WB-3. A freeze seals lanes that writers are still filling (required).**
   - Pre-stage copies records into the writer's lane before it joins a group (`:188-191`), outside
     any group. A freeze runs in a leader at a group boundary (`:342-348`) and says the frozen
     buffer's lanes and records "nobody writes again". A writer that pre-staged into the active
     buffer's lane and links after the freeze has records in the sealed buffer; the next leader
     then writes their headers there, and the writer may still be appending to that lane's tail.
     The new active buffer's map cannot reference them: a 32-bit reference has no buffer field
     (`:201-204`).
   - "A lane ... is freed with its buffer" (`:184-186`) and "claims a lane ... in the index's lane
     table, released at check-in" (`:175-177`) disagree on whether lanes belong to the index or to a
     buffer.
   - This is not only step 2: at one writer a writerless group (a search's coalesced freeze, a
     node-budget freeze) can freeze while that writer pre-stages.
   - Fix (the simpler of two): a lane belongs to one buffer; a writer learns at linking whether its
     buffer was sealed, and the leader copies such records into its own lane in the active buffer
     (cost only at freezes); nothing reads a sealed buffer beyond its position index (WB-4), so a
     late write into a sealed lane's tail reads nothing; the writer's view guard keeps the sealed
     buffer alive until it links. State it, and T8 freezes while writers are in pre-stage.

4. **WB-4. Scans and flushes have no safe way to find a buffer's records (required).**
   - (C)'s runs cover "every record up to a position", and a scan sorts "the records past the
     delta's cover and at or below V" (`:370-379`). Nothing in D3 or D4 lets a reader enumerate
     records by position: lanes are written by their writers (record bytes) and the leader
     (headers) at once, with no published fill mark, so walking a lane is a data race and finds
     rejected and unfinished records. The flush of a frozen buffer has the same need.
   - (C)'s run pairs are built and installed by readers (`:375-378`) and "retire like tables"
     (`:378-379`), but D10's retire list belongs to the lease holder (`:394-402`): a reader has
     nowhere to put the pair it replaced.
   - Fix: one append-only position index per buffer (a 4-byte record reference per accepted write,
     in position order), appended by the leader, its count published with release; run builds,
     the private tail, and the flush read it up to V. Replaced run pairs go to the lease holder
     through a lock-free push list it drains at its scan (the prototype already has one,
     `lanebuf.hh` `retire_list`), or only the lease holder installs runs. Its 4 bytes per record
     count in WB-7.

5. **WB-5. The write group's execute-then-log path is not the one journaled indexes run; T11
   decides a path Elysian's presets do not take (required).**
   - D4 and D7 run stage 1 in the leader, then stage one batch per group in the turn log
     (`:214-222`, `:312-323`). For replicated groups D7 says apply runs on committed entries, one
     applier, one lane (`:331-334`). durability-modes makes every journaled index a consensus
     group, standalone included: "Standalone durability is the same mechanism as a group of one"
     (`durability-modes.md:780-781`, `:952-953`), and every preset keeps a synced journal
     (`:951`). Xapiand's default preset is `replicated` at `memory` (`:2060`).
   - So lanes, the parallel copy, pre-resolution, and "no group barrier" reach only indexes without
     a strata journal: 7a, where Xapiand keeps its own WAL (`xapiand-on-strata.md:6`), and the
     benchmarks against RocksDB with its WAL off. On every journaled index the applier copies each
     record and runs stage 1 alone, and proposals are already concurrent, so "a request never waits
     on another request's writer" holds there with no write group at all.
   - T11, the row that decides D14 for Xapiand (`:491`), measures the journal-less path only.
   - Fix: a table in D7 of which path runs per (journal or none, preset, rf), and what each part of
     (C) buys on each. Then either (a) state that D14's decision binds journal-less indexes only
     and that Elysian's indexes take the applier path whatever T11 shows, or (b) carry lanes into
     the applier path (a client pre-stages its records into a lane at proposal time on the leader
     replica; the applier fills headers and copies only entries from other replicas) and add T11's
     shape on the applier (R proposing threads, one applier, `memory` at rf = 1) as the deciding
     row. The owner's Q3 bar then applies to the path Xapiand runs.

6. **WB-6. Q1's answer needs four rules the text does not have (required).**
   The owner chose visibility at acknowledgement (ledger `:41`). With durability-modes' per-write
   acknowledgement levels (a default and a floor per preset, `durability-modes.md:2033-2045`,
   `:2056-2062`):
   - **V is the contiguous acknowledged prefix.** A `none` write behind a pending `memory` write is
     not visible until the earlier one is, so V never passes an unacknowledged position. D7's "at
     once for `none`" (`:324-327`) holds only for an index whose writes are all `none`. Two writers
     store V today (the leader for `none`, the turn completion for barriers); one owner of V, or a
     monotonic rule over the prefix, is required.
   - **Every verdict waits for its predecessors.** A version conflict is an observation of earlier
     unacknowledged writes; returned before their barrier, a client learns of a write a crash then
     removes. Rejections and kept-docid answers are delivered no earlier than the acknowledgement
     of the groups before them. D4 step 3 (`:221-222`) and D12 (`:435-437`) say when followers wake,
     not this.
   - **V is stored before the writers resume**, so a request that reads after its own acknowledged
     write sees it (T11's index-then-read request depends on it).
   - **Positionless state waits for acknowledgement too.** With WB-1's rule, a view's position is
     visible at once, so a freeze or an install of a buffer holding unacknowledged writes publishes
     its view only when V reaches its position, and V never passes the newest published view's
     cover (positions in a buffer no published view holds). The view is published under the lease,
     so the turn completion that acknowledges F schedules a writerless group for it, rather than
     publishing off the lease (SR-D15's order then holds with one publisher of views).
   - T8 runs with a log under mixed acknowledgement levels, with crashes before barriers, and checks
     that no reader and no verdict observes a write the crash removed.

7. **WB-7. The 153 B per record is not supported; the same layout measured 177 B (restate).**
   - D3's estimate takes the map slot at 9.1 B (8 bytes at 7/8 full, `:206-210`). A doubling table
     averages far less full. `bytes.out`: at T6's 1,000,000 records the table has 2,097,152 slots,
     16.8 B per record; `kv_proto_bufmem`'s method (resident growth from 100,000 to 300,000
     records) charges 15.7 B. With the 144-byte record that is 160-161 B, before anything else.
   - Format 14's prototype `sortrun2` already is this layout (inline records with a 24-byte header,
     8-byte slots at 7/8, a main run and a delta): 161.5 B accounted and **176.7 B RSS**
     (`format-14-kv/report.md:84`). (C) changes the header's contents (position and `prev` instead
     of lengths and a dead flag), not its size, and adds WB-4's 4-byte position index.
   - D10 keeps each replaced table until no record names a view of its generation, and growth
     publishes no view (`:272-276`), so a buffer that grows to 1,000,000 records without a freeze
     holds every earlier table: another 16.8 B per record until the next freeze (`bytes.out`).
     Either growth bumps the generation (a new view object, O(segments)) or tables are protected
     on their own.
   - T7: a chunk table of 256 pointers per lane (`:182-184`) is 2 KiB if it is a fixed array, which
     a reader-safe table needs unless reallocation is retired too; T7's target is format 14's buffer
     plus 1 KiB (`:487`).
   - Fix: restate D3 and D13 at about 165-177 B with the slot cost at the table's real occupancy,
     name what closes the gap to 153 B (format 14 named the same squeeze: varint lengths, a 4-byte
     docid offset), and keep T6's target, reporting the miss honestly if it stands. Not blocking.

8. **WB-8. The bake-off does not decide in every outcome, and its prototype omits what the
   threshold weighs (required before the lead measures).**
   - **Rule 1 has no outcome when both finalists miss** T1, T2, T4, or T7 (`:496-499`). Likely at 8
     writers: (C)'s and A''s stage 1 are serial and RocksDB's inserts are parallel. State the rule
     then (the closer one by the row it misses, and the miss reported).
   - **T7 cannot run on the prototype**: its procedure is the engine's `run.py rows ... many`
     (`:487`). Either a prototype T7 or drop it from rule 1.
   - **T4 has no A' target** (`:484`), so A' cannot "miss" it; the 10% rule (`:498-499`) is the
     only check. Say so.
   - **The prototype has no real segments.** D14 counts pre-resolution as a multi-writer cost and
     the parallel resolution as its gain (`:466-469`), and the leader's per-segment lookup (filter
     and dictionary, `index.hh:1952-1964`) is what pre-resolution moves off the serial path. The
     prototype's segments are sorted `std::vector<std::string>` stubs searched by `lower_bound`
     (`lanebuf.hh` `segment`, working tree at `7a6d1b9`), and T11's indexes are never flushed. Q3's
     bar then compares two modes without the cost that separates them. T11 runs with most keys in
     segments that use the real key filter and dictionary, or with a stated stand-in cost per
     segment probe, at Xapiand's segment counts.
   - **Noise against the bar.** "At most 3% at one writer" and "at most 5% more CPU" need an A/A
     spread below them, reported beside each decision; a difference inside the spread counts as
     zero.
   - **T3's p99 hides (C)'s worst case.** A delta past 4,096 records merges into a main run of up to
     the whole buffer on a reader's scan (`:376-377`), a copy of 4 bytes per record (4 MB at
     1,000,000) that one scan in thousands pays. T3 reports p99.9, the maximum, and the run-build
     time distribution beside p99, and rule 1's T3 clause states which.
   - **T1's one-writer baseline** times `main`'s whole `apply` (`bufmem.cc:53-75`) against a lean
     prototype loop; it cannot show "no slower than today" either way. T5 on `main` is that check;
     say T1's 1-writer column is informational.
   - **The prototype's reader protection is not D10's.** `lanebuf.hh` (working tree) protects
     readers with a process-wide epoch domain (thread-local slots, `atomic_thread_fence`), which
     shared-reader D5 rejected (alternative 4: one stalled reader stops every index's frees) and
     which ThreadSanitizer does not model. T2 then times a read path that will not ship. The
     prototype uses the hazard registry (`hazard.hh`, with SR-D18's fix) or reports the difference.
   - Smaller: T11's RocksDB uses one DB with 1,000 column families, which share one write queue and
     WAL path; the row states that asymmetry as `many` does.

9. **WB-9. Record fields written after publication must be atomic, and T10's x86_64 mutant cannot
   fail (required).**
   - D4 sets a rejected record's position to "never" after its slot was published (`:239-242`), and
     the record has a "superseded" flag (`:197`) that D6 never assigns a writer. A reader may be
     reading either field. Written as plain fields that is a data race (`vm_tsan_plain_never`, TSan
     at `vmap_stress.cc:50`). The position (and the flag, if kept) are `std::atomic` read and written
     relaxed, or never written after publication. The map itself does not need "never":
     `vm_*_no_never` passes, since the restored slot already hides the record; the mark serves only
     enumeration (WB-4), so a reader of the position index skips "never" records.
   - T10 promises that "a mutant with the slot stored relaxed fails" natively on x86_64 and macOS
     arm64 (`:490`). On x86_64 a relaxed store compiles to the same `mov` as a release store:
     `vm_native_relaxed_slot` reads 4,920,228 times with 0 wrong. Only TSan
     (`vm_tsan_relaxed_slot`, races at `:88`) or arm64 hardware catches it, as SR-D4 found for the
     hazard handshake. T10 names macOS arm64 and TSan as its killers.

10. **WB-10. The lane limit is not load-bearing, and a lane is held for a whole checkout
    (required).**
    - Three bits of lane in the 32-bit reference (`:201-204`) cap a buffer at 8 lanes, one of them
      the leader's for copied records, so 7 writers get the parallel copy; T11's R = 16 exceeds it on
      hot indexes. No mechanism is named, and Lucene's DWPT count has no such cap. The limits rule
      asks for the mechanism and a cap at least the largest of Lucene's, Tantivy's, and Xapian's.
    - A checkout claims its lane at checkout and releases it at check-in (`:175-177`), so a request
      that writes once and then reads, or suspends, holds a lane throughout.
    - Fix: claim a lane for a pre-stage and release it after the verdict (or per thread, not per
      checkout); name what sets the lane count (the reference's bit split against chunk count and
      offset) and test T8 and T11 with more writers than lanes, the leader-copy path included.

11. **WB-11. Kronuz/journal: today's driver is single-threaded, but no journal change is needed
    (ruling).**
    - Confirmed: `TurnDriver::stage` pushes onto a plain `std::vector` (`turn_driver.h:57-60`) and
      `begin()` drains it (`:66-79`), so staging from another thread races. Record sequences are
      the caller's and checked contiguous per group (`turn_log.h:288-294`), which the lease's
      handoff already orders.
    - D7's two options (`:317-323`) miss a third: a lock-free inbox per core on strata's side (the
      shape of `LinkOne`) that the core's turn loop drains into `stage()` before `begin()`. It adds
      no hop and changes nothing Detent links. Recommendation: that inbox, in strata; journal stays
      single-threaded, and Detent's agent gets a note, not a change. If a second consumer wants it,
      it moves to journal then, additively.

12. **WB-12. Search lanes need buffer deletes, and their freeze waits on the slowest inverter
    (condition for step 4).**
    - A replace by `_id` decided in one lane leaves the old document's postings in another lane's
      inverter; shadowing by key (D6) does not hide postings. Step 4 needs pending buffer deletes
      `(lane, docid)` the leader appends and the freeze applies, as it does for segments.
    - D11's freeze waits for each lane to finish inverting (`:416-417`): the barrier on the slowest
      member that (C) avoids for key-value (`:152-153`) returns for search, coupled to each writer's
      executor. T12's NRT p99 reports that wait.
    - Rejected search documents never reach a lane's inverter (they invert after the verdict):
      state it, since D4's "never" path covers only key-value records.

13. **WB-13. The stage-1 ruling's change is accepted (ruling).** `reviews/stage-1.md:317-337`
    rejected a DWPT pool to keep docid assignment sequential and deterministic. (C) keeps stage 1
    sequential in one leader at a time, and the total order at admission becomes the queue's arrival
    order recorded in the log, which replay reproduces (T9). What the ruling rejected, Lucene's
    global delete queue, stays out. The lead amends ARCHITECTURE item 1 on landing, as D2 says.

14. **WB-14. The leader's worst case on a reactor thread exceeds the slice (condition).**
    D12 says a leader's work is bounded CPU within the slice (`:433-437`). Growing a 2,097,152-slot
    table reads 16 MiB and writes 32 MiB, milliseconds against a 100-250 µs slice
    (ARCHITECTURE `:173-185`); a freeze clones the bits of every segment that took a delete. Today's
    buffer has the same growth pause on its owner, so it is not new, but the claim is. Either the
    leader moves growth and clones off the reactor (an incremental migration that readers probe in
    both tables, or a growth job on the pool with the leader waiting as a task), or D12 states the
    bound and T1 and T2 report the longest group.

15. **WB-15. Smaller gaps (conditions for step 1's tests).**
    - Commit and log reclamation with frozen buffers: a checkpoint's position is at most the oldest
      unflushed frozen buffer's first position, or commit flushes them first; D8 says neither.
    - A keyless buffered document deleted by docid has no key to shadow and no live bit
      (`:300-304`); name its mechanism (a tombstone through the docid map, applied at flush).
    - RocksDB's small-group cap is the write's size plus one eighth of the maximum group size
      (`write_thread.cc:451-454`), not "plus one eighth of it" (`:216-217`).
    - Not weighed in D1: Elasticsearch's answer for conditional writes over per-thread Lucene
      buffers is a live version map with per-key locks, so stage 1 runs in parallel for distinct
      keys. It removes D1's leader ceiling (Risks, `:591-594`) but makes docids follow lock order,
      which then has to be the log's order; one paragraph saying why (C) is chosen over it closes
      the comparison.

## Blocking items / Required changes

Revision 1 resolves these before step 1; a scoped architect re-check (high) covers their text.

- **WB-1:** readers read at `max(V, view.position)`; a view's position is that of its positionless
  state; T8 schedules a freeze and an install between a reader's V and its acquire, with the
  read-at-V mutant.
- **WB-2:** a merge's install (and `heal()`, `install()`) carries the pending deletes that name its
  sources; T8's case and mutant.
- **WB-3:** lanes' ownership across a freeze stated; records pre-staged into a sealed buffer are
  copied by the leader; T8 freezes during pre-stage.
- **WB-4:** a per-buffer position index for runs, tails, and flush; a retire path for run pairs a
  reader installs.
- **WB-5:** which path runs per journal, preset, and rf; D14's decision scoped to the path it
  binds, or the applier shape added to T11.
- **WB-6:** V as the contiguous acknowledged prefix with one owner; verdicts after their
  predecessors' acknowledgement; V before resumption; views with unacknowledged positionless state
  published only at acknowledgement, under the lease.

Required in revision 1 but not blocking step 1's start: WB-8 (before the lead measures step 0),
WB-9, WB-10. WB-7 is a restatement.

## Conditions

- **C1. Step 0 continues now** (benchmark code; gates decide landing, not starting), provided the
  prototype builds WB-1's read rule, WB-3's freeze rule, WB-4's position index, and WB-9's atomic
  fields, protects readers with the hazard registry (WB-8), and the lead sets WB-8's decision rules
  in `docs/perf/write-buffer-bakeoff.md` before the first timed run. Its K1 to K4 stand, plus:
  K5, T8's model check includes the WB-1, WB-2, and WB-3 schedules.
- **C2.** WB-11: the staging inbox is strata's; journal is unchanged unless a second consumer asks.
  The lead tells Detent's agent so before step 3.
- **C3.** WB-12's buffer deletes and freeze-wait report are in step 4's done criteria.
- **C4.** WB-14 and WB-15 are answered in revision 1's text or in step 1's done criteria.
- **C5.** On landing, the lead edits ARCHITECTURE items 1, 4, and 9 (`:88-89`, `:95-97`,
  `:108-109`) per D2, D3, D5, and WB-13.

## The owner's answers, against these findings

- **Q1 (acknowledged at the index's level):** consistent with the design once WB-6's four rules
  are in; linearizable get-by-id reads the live version map, as durability-modes already says
  (`durability-modes.md:2049`).
- **Q2 (search at a freeze, coalesced):** consistent; WB-6 makes a durable index's search freeze
  wait for its acknowledgement, and WB-12 states what the freeze waits on.
- **Q3 (1.5× requests per second or 0.5× p99, at most 3% at one writer and 5% more CPU):**
  measurable once WB-8's A/A spread is reported; it decides the right question only on the path
  WB-5 names. The ledger and the bake-off plan carry it; the design's Q3 text (`:527-536`) should
  record the owner's bar in revision 1.

Not run: strata build and tests, the step-0 prototype, any benchmark or timing, arm64 (native),
Linux, FreeBSD, a C11 model checker, prospect.

Probes kept in `.scratch/review/` (this worktree): `vmap_stress.cc`, `build.sh`, `run.sh`,
`runs.out`, `view_model.py`, `view_model.out`, `bytes.py`, `bytes.out`. Rerun with `build.sh` then
`run.sh` under a light hold; `python3 -I view_model.py` and `python3 -I bytes.py` need no build.

## Re-check

- Date: 2026-10-21.
- Reviewed: `docs/design/write-buffer.md` revision 1 at `cd8affb` (HEAD equals
  `origin/design/write-modes`; `git diff 56fd828 cd8affb`), scoped to WB-1 to WB-15, C1 to C5,
  and the text they changed. This review file at `6f2a5ca` is unchanged by `cd8affb`. Read for C1:
  `perf/lanebuf` at `4f4e478` (its prototype commit `523e677` and the uncommitted `lanebuf.cc`) and
  the core branch's `hazard.hh` (working tree on `41fde48`). Citations re-read where the revision
  leans on them: `durability-modes.md:1685-1703` (D7, apply at `commit_memory`).
- Checked by: reading, against the probes kept in `.scratch/review/` (unchanged; not rerun, since
  the protocol steps they model are the ones revision 1 keeps or the fixes they demonstrated).
  - Not run: no build, test, or prototype run; no new probe; no benchmark; arm64, Linux, FreeBSD.

### Verdict

**APPROVE WITH CONDITIONS: WB-1 to WB-5 and WB-7 to WB-15 are closed. WB-6 is partially closed:
its rules collide with durability-modes D7 on indexes that mix acknowledgement levels, which
matters from step 3. Four new findings are not blocking: WB-16 to WB-19 (WB-19 is required in the
prototype now). Step 1 may start, and the step-0 prototype may resume under C1 as amended
below.**

### Status

| Item | Status | Where |
|---|---|---|
| WB-1 | Closed. Views carry a position; readers read at `max(V, view.position)`; a view is published only once V reaches its position; T8's schedule and the read-at-V mutant | `write-buffer.md:326-333`, `:435-438`, `:641-642` |
| WB-2 | Closed. Frozen buffers and segments are components with copy-on-write bits; merge, flush, `heal()`, and `install()` each carry or drop pending deletes, stated per install; schedule and mutant | `:362-380`, `:641-642` |
| WB-3 | Closed. A lane belongs to one buffer; the sequencer copies a sealed lane's records before stage 1; late appends are unreachable through the map and position index; the guard keeps the sealed buffer alive; T8 freezes during pre-stage | `:213-224`, `:462-469`, `:274-275` |
| WB-4 | Closed. A per-buffer position index, published at the group's end, is the only enumeration path; a rejected transaction rewinds it; only the lease holder installs run pairs, built on the pool. See WB-17 for the trigger | `:494-510`, `:300-304` |
| WB-5 | Closed. Goals and D7 tabulate the paths; on journaled indexes multi-writer means concurrent proposals plus pre-staged lanes on the leader replica; T11a (applier) decides Xapiand's mode against the owner's bar, T11b the journal-less one; Q4 puts the scope to the owner. T11a's RocksDB comparison (WAL on, against a modeled log delay of 0) is informational until step 3's engine run, which the row already requires before code is deleted | `:45-63`, `:396-409`, `:644-645`, `:658-660`, `:680-699` |
| WB-6 | Partially closed. Closed: one owner of V, V before resumption, positionless state published at acknowledgement under the lease, the crash schedule. Open: see "WB-6, what remains" below | `:421-444` |
| WB-7 | Closed. Memory restated at the slot's real occupancy and format 14's 176.7 B, about 181 B with the position index; the squeezes named; replacements bump the generation; the chunk table starts at 8 entries. See WB-17 for what the bump costs | `:250-262`, `:526-538` |
| WB-8 | Closed. Both-miss rule, T7's prototype form, no A' T4 target, real segments, A/A spreads, T3 at p99.9 and maximum, T1's one-writer column informational, the hazard registry in the prototype, the column-family asymmetry stated | `:629-662`, `:540-541`, `:719-720` |
| WB-9 | Closed. No field is written after publication; "never" and "superseded" are gone; T10's killers are TSan and macOS arm64 | `:234-238`, `:643` |
| WB-10 | Closed. Lanes claimed per pre-stage; the reference drops the lane field; the chunk table's 32,768 entries are the limit, with a freeze as the mechanism; T8 runs more writers than lanes. See WB-19 for who grows the tables | `:206-208`, `:240-248` |
| WB-11 | Closed. A per-core inbox on strata's side; journal unchanged | `:414-419` |
| WB-12 | Closed. Buffer deletes across lanes; rejected documents never inverted; T12 reports the slowest lane's wait | `:550-564`, `:646` |
| WB-13 | Closed (recorded) | `:70` |
| WB-14 | Closed. Growth past 65,536 slots and freezes with large clones go to the pool, awaited as a task; T1 and T2 report the longest group | `:582-588` |
| WB-15 | Closed. Checkpoints cover flushed state only; docid tombstones for keyless buffered documents; RocksDB's group cap restated; per-key locks weighed | `:486-490`, `:383-387`, `:270-271`, `:146-152` |
| C1 | Closed in the text (step 0's pieces and K5). For the prototype, see "Step 0" below | `:707-737` |
| C2 to C5 | Closed in the text: the inbox note to Detent, step 4's criteria, step 1's criteria, the ARCHITECTURE edits on landing | `:418`, `:556`, `:750-751`, `:69` |

**WB-6, what remains.** Rules 1 and 2 make V the prefix acknowledged at each write's own level and
hold every verdict until V reaches it (`:424-432`). durability-modes D7 applies at `commit_memory`
precisely so that a `memory` reply carries its verdict without waiting for a barrier, and it
rejected the alternative because "every reply with a verdict would then wait for the next barrier"
(`durability-modes.md:1699-1703`). With the floor below the default (`replicated`: `memory`
default, `none` floor; `strict` allows `quorum` writes beside others), one `quorum` write
holds the visibility and the verdicts of every later `memory` write until `commit_durable`, and
revision 1 states this only as "a `quorum` write holds V" (`:441-442`). The owner's answer was
"acknowledged at the index's durability level", which can be read as the index's level, not each
write's. Before step 3, D7 picks one of two rules and says so, and durability-modes' owner
agrees:
1. V is the prefix acknowledged at the index's default level. A stronger write is visible at that
   level before its own acknowledgement, which the default level's guarantees already cover.
2. Keep per-write levels, and state the head-of-line latency as a cost T11a measures under a mixed
   stream.

Step 1 has no log, so it does not wait for this.

### New findings

16. **WB-16. A get cannot check a frozen buffer's live bits as written (not blocking; text before
    step 1's item 4).** Frozen-buffer bits are indexed by position-index slot (`:362-364`), and a
    get "checked against its own live bits" (`:337-338`) reaches a record through the map, which
    yields a record reference, not a slot. The header holds a position, which is not the slot either:
    several writes share one position, and rejected transactions take none. Either a get on a frozen
    buffer relies on shadowing (newer components are probed first, so a keyed delete always meets
    its newer record or tombstone before the stale copy), and only enumeration (flush, run builds)
    reads the bits, or the slot goes into the header. Say which; T8's model covers a keyed delete of
    a frozen-buffer copy read after a second freeze.

17. **WB-17. Run builds fire on writes alone and each install publishes a view; the stated cost is
    about 10 times low (required in the prototype before the timed runs).**
    - "When the uncovered tail passes a threshold (256 records) the next lease holder ... posts a
      build" (`:503-506`) triggers on writes, whether or not anything scans. On T1 and T11, which
      never scan, the pool then sorts every 256 records and merges a delta into the main run every
      4,096. Each main-run merge copies 4 bytes per buffered record, about 244 merges and roughly
      0.5 GB copied per million records at 1,000,000. Format 14 built its view only when a scan or a
      flush asked (`format-14-kv.md:306-313`).
    - Each install publishes a view (`:527-528`): about 3,900 publications per million records at
      a 256-record threshold, each with a registry scan of H records, against D10's "at most a few
      hundred" (`:536-537`).
    - Fix: builds are posted only when a scan ran since the last install (a flag a scanner sets), or
      when a flush needs the order. The publication count is restated, and T3 reports publications
      per second and the scan time. T1 and T11 then pay nothing for runs, as the format-14 baseline
      paid nothing.

18. **WB-18. Pre-resolution is keyed on the view generation, which now moves on every table, chunk
    table, and run replacement (required in the prototype before the timed runs).** D4 trusts a
    pre-resolution only while "the current generation is still g", and lists replacements among
    what bumps it (`:309-311`, `:526-528`). A replaced map table, chunk table, or run pair changes
    nothing pre-resolution reads (frozen buffers' maps, components' bits, segments), yet it
    invalidates every pre-resolution in flight, so the sequencer redoes the segment lookups that
    pre-resolution exists to move. Under WB-17's builds that is every 256 records. Fix: two counters,
    the view generation for retirement (D10) and a component epoch for pre-resolution, bumped only
    by a freeze, flush install, merge, `heal()`, `install()`, or a bit change. T9's mutant ("used
    across a changed generation") is restated on the component epoch.

19. **WB-19. Writers grow the chunk table and the lane table, but "only the lease holder replaces"
    them (required in the prototype now and in the text before step 2).**
    - A lane takes a new chunk during pre-stage, outside any group, by claiming the next chunk-table
      index with `fetch_add` (`:241-243`). The chunk table "grows by replacement, retired like a map
      table" (`:243-244`), and D10 says only the lease holder replaces such tables (`:525`). A
      writer whose claimed index is past the table's capacity has no lease to grow it. If a writer
      stores a chunk pointer into a table the lease holder is replacing, the new table misses it
      unless the copy and the store are ordered, which nothing states.
    - The lane table "grows as concurrent pre-stages need lanes" (`:215-216`), from writers too.
    - Fix: a chunk directory that is never replaced, two levels fixed at creation (for example 16
      root entries of 2,048-entry pages, a 128-byte root, pages installed by compare-and-swap on
      first use and freed with the buffer). It needs no retirement and keeps T7's bytes. Lanes go on
      a lock-free push-only list that readers never touch. T8 runs pre-stages that cross a page
      boundary at once.

### Step 0: may the bake-off resume

**Yes, under C1 amended.** The prototype at `523e677` still protects readers with its
process-wide epoch domain (`lanebuf.hh` `class epoch`). The core branch's `hazard.hh` still has
SR-D18's race (`hazard.hh:130`, `:136`), which T11's 1,000 indexes sharing one registry would hit.
So before the first timed run:
1. The prototype takes `hazard.hh` with SR-D18's fix, or a copy of it fixed the same way.
2. It builds revision 1's step-0 list (`:707-723`), plus WB-17's scan-triggered builds,
   WB-18's component epoch, and WB-19's fixed chunk directory.
3. The lead writes the decision rules (`:648-660`) into `docs/perf/write-buffer-bakeoff.md`.
4. K1 to K5 pass at small counts under TSan, with T9's mutants killed.

Functional work on the branch needs none of this to resume.

### Conditions

- **C1** amended as above. **C2 to C5** stand as answered.
- **C6.** WB-6's remaining choice is in D7, with durability-modes' agreement, before step 3.
- **C7.** WB-16 is in the text before step 1's item 4; WB-17 to WB-19 are in the text before
  step 2. The code reviewer confirms all of them at file:line. Unless D7's WB-6 rule changes the
  protocol beyond the two options named, no further architect re-check is needed.

Not run: builds, tests, the prototype, benchmarks, arm64, Linux, FreeBSD, a model checker.
