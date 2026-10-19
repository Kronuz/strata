# strata review: the layers design (Phase 4, D1)

- Date: 2026-10-15.
- Reviewed: `docs/design/layers.md` at `9abf71c` (branch `phase4/layers`, worktree
  `strata-wt/layers`, not pushed). It answers Phase 4's D1, revision check 1's N4, and
  revision check 2's M2, with decisions L1–L16 and five open questions.
- Also ruled: the skip keying of the doc-values codec draft, `include/strata/docvalues.hh`
  at `bc1061d` (branch `phase4/docvalues`), against `phase-4.md` D4.
- Checked against the code the design cites, in the worktree at `9abf71c`:
  `include/strata/{format,writer,reader,index,merge,search,searcher}.hh`, `tools/
  strata_engine.cc`, and `test/`. Also my Phase 4 design review and its two revision
  checks, `phase-3.md` (D12, D17, D18), BENCHMARKS, and PLAN's Ratchets.
- Checked by reading. Nothing was built, run, or measured: timed trials hold the shared
  VM.

## Verdict

**APPROVE WITH CONDITIONS.** The seams in L1 are real. I checked each against the code
it cites, and the ownership table matches what the code does:

- `flush_from` (`writer.hh:353-466`) already writes the sections in exactly L2's order:
  `inverted`'s six, then `docids`, `versions`, `pk_filter`, `stored`, `stats`.
- `begin_document` (`writer.hh:479-506`) already takes the key, version, stored record,
  and length as separate fields.
- The digest's `D` record (`index.hh:524-533`) is the docid, the version, `inverted`'s
  length, and then `stored`'s size and bytes, as L8 says.
- Hits leave the per-document path only through `seg_->norm` and `seg_->length`
  (`search.hh:281, 930-933`) and `seg->docid` at collection (`searcher.hh:120`). All
  three move cleanly to `inverted`'s reader and `core()`.

Option A over B (L16) is right for the reason given. Typed parts in the buffer avoid an
allocation per part per document on the indexing path that Phase 3 is ratcheting.

Five things are not right yet:

- The plugin interface's static calls cannot see the runtime state they need (R1).
- L7's region verification as written adds a lookup to a per-block hot path (R2).
- L3's cross-reader reference breaks if a segment moves (R3).
- L15 does not measure everything the refactor can move (R4).
- Step 7 belongs to Phase 3, not to this refactor (R5).

R6–R10 answer the five open questions. R11 rules on the doc-values skip keying.

Steps 0 and 1 can start now. The rest waits as the last table states. Landing any step
on `main` still waits for N4: Phase 3's ratchet rows recorded at a stated commit.

## Required changes

1. **R1. Plugin calls need a context** (L4, `layers.md:208-213`; L10, `456-459`).
   - `P::validate(part)`, `P::bytes(part)`, and `P::disk_estimate(part)` are static and
     take only the part. Validation already depends on runtime state:
     - `key_of` reads the manifest's `m_.key_prefix` (`index.hh:1026-1037`), and L10
       moves it to `keys_plugin::key_of(d)` with no way to reach the prefix;
     - Phase 4's `values` validation checks each value against the slot table (D3), the
       declared type, multiplicity, and NaN.
     Neither can be static.
   - Required: `P::validate(const validate_context&, const part&)`. The context carries
     the index options, the manifest's key prefix, and the slot table at the
     transaction's position. `key_of` takes the same context.
   - `bytes` and `disk_estimate` may stay static, but they must not depend on runtime
     state, so their sums stay equal to today's (`index.hh:711-716, 891-899`).
   - **Check:** step 4's `index_test` and T2 schedules with a key prefix set and
     unset.

2. **R2. Region verification is resolved once per reader** (L7, `368-371`).
   - `region_verifier::verify(kind, offset, length)` "takes section-relative offsets;
     the core adds the section's file offset". `inverted`'s `need()` calls it on every
     postings and positions decode it guards (`reader.hh:988, 1161`), which is the
     per-block path. A lookup by kind there, even a small table scan, is new work on
     that path.
   - Required: each reader receives, at construction, a handle per section it owns (the
     section's file offset plus a reference to the shared `verified_regions`). A verify
     is then `regions.verify({base + offset, length})`, today's arithmetic
     (`reader.hh:199-204`) exactly.
   - The lazily verified `lengths` state (`reader.hh:418-422`, the atomic in
     `integrity`) stays in `inverted`'s reader.
   - **Check:** step 2's A/B on the term and phrase classes (L15's last paragraph names
     step 2 already; R4 makes it mandatory there).

3. **R3. No reader holds a pointer to a sibling reader** (L3, `183-188`).
   - The format-12 `keys` reader "holds a reference to the `inverted` reader". Both
     live in `std::tuple<typename P::reader...>` (L4) inside `basic_segment`. Moving or
     copying the segment leaves that reference pointing at the old tuple.
   - Today's `segment` survives moves only because `dict_`'s context is
     `integrity_.get()`, a heap object with a stable address (`reader.hh:140, 427`).
   - `strata_engine` returns segments by value from `open()`
     (`tools/strata_engine.cc:368, 378, 392, 457, 604, 657, 801`), which is safe only
     under guaranteed copy elision.
   - Required, one of:
     - the dependency is passed at call time (`keys.find(key, live, segment.get<inverted_plugin>())`);
     - or `basic_segment` deletes its copy and move constructors, and every `open`
       returns a prvalue.
     The first is preferred: the dependency is deleted at format 13 anyway, and it keeps
     `basic_segment` movable.
   - **Check:** a `static_assert` on `basic_segment`'s move semantics; under ASan, a test
     that moves a format-12 segment and then looks up a key.

4. **R4. L15 measures everything the refactor can move** (`565-587`).
   - **The multi-segment index before merging.** Phase 3's rows run on the
     multi-segment index before and after merging (PLAN, Phase 3 gate). The refactor's
     per-query cost is per segment (`get<P>()` and scorer setup at `searcher.hh:113,
     131`), so it multiplies with segments. L15 serves one index per binary; it runs
     both indexes, before and after a forced merge.
   - **Allocations per query, by class**, from the allocation-counting build: equal
     between A and B, not merely within noise. A `std::function` or a tuple copy on
     scorer setup shows up here before it shows up in time.
   - **Serving memory**, the cgroup row: the reader tuple and the split `integrity`
     state change what each open segment pins.
   - **Merge.** Step 5 replaces the merge path. A forced merge of the 5M index to one
     segment is timed, with byte identity checked by hash, and T3's write-amplification
     row runs before and after.
   - **The decision rule.** "Every interval contains 1.0 or lies below it" passes a
     noisy 8% regression whose interval is [0.97, 1.08]. And at 95% per row, across
     about 50 rows, two or three false alarms are expected. So:
     - a row passes when its upper bound is at or below 1.03, BENCHMARKS' 3% effect
       size;
     - a row above that runs again with ten trials;
     - it blocks if its upper bound still exceeds 1.03.
   - **Platforms.** The gate VM (Linux x86_64) and the FreeBSD VM. macOS on the host is
     a report row. arm64 runs when its replacement machine exists, before Phase 3R's
     gate, as the Track P rule asks of every measurement.
   - **Step 2 and step 3 A/Bs are mandatory**, not conditional on "more than expected".
     Step 2 changes the reader every query uses; step 3 changes the per-document builder
     calls.

5. **R5. Step 7 is Phase 3's D17 debt, not this refactor** (open question 4). Phase 3's
   D17 already charges "the flush's sort and spill buffers" (`phase-3.md:301-303`). The
   writer's term table and posting buffers are uncharged today (`layers.md:280-282`),
   and Phase 3's indexing rows run under `STRATA_BUDGET_MB`.
   - Charging them changes when budgeted indexes flush, and therefore the segment count
     and memory that Phase 3's indexing rows measure.
   - It lands in Phase 3, on today's code, before Phase 3's ratchet rows are recorded,
     so the baseline that N4's A/B compares against includes it.
   - The refactor then carries it through unchanged and stays byte-identical with no
     exception. Step 7 leaves L14.

### The open questions

6. **R6. Digests across layouts** (open question 1). Format 13 splits the `D` record.
   - `D` carries only the core's fields: docid, version, and from format 13 the key.
   - Each plugin with per-document content emits its own record:
     - `inverted` a length record;
     - `stored` a record holding the stored bytes;
     - `values` its `V` records (D17).
   - Two rules follow:
     1. A plugin never adds fields to the core's record.
     2. A plugin with nothing for a document emits nothing.
   - Then two layouts digest the content they share equally. More important, appending
     a plugin to a layout (L2: "later plugins append") leaves every existing index's
     digest unchanged. Under rolling upgrades (Track D8), replicas on two binaries
     compare digests during the overlap. With L8 as written, the first plugin that
     contributes a `D` field breaks every digest comparison across the upgrade.
   - Cost: one more hash per document per plugin. The digest is off the query and
     indexing paths.
   - Format 12 keeps today's record (step 0's pinned value). The split lands with the
     key in `D` at format 13, the one point where the digest changes value anyway
     (`layers.md:395-397`).

7. **R7. The manifest records the plugin set; `unsupported_segment` stays as defense**
   (open question 2).
   - An opaque layout id in the manifest would forbid appending a plugin. Instead, the
     manifest records each plugin's identifier and the format version of its sections.
   - Open compares that set with the binary's layout before touching a segment:
     - a missing plugin fails the open with a typed error naming it;
     - an extra plugin in the binary is fine, since its sections are absent and its
       structure flag is clear.
   - `install` and checkpoint transfer check the same set, so a node never accepts a
     checkpoint it cannot read; that is a Track D8 rolling-upgrade rule.
   - At the segment, an entry the layout cannot claim still raises
     `unsupported_segment`, distinct from `corrupt_segment`, so `heal` and `media_test`
     never report a healthy file as damaged.
   - **Check:** a test opening an index whose manifest names an unknown plugin, and one
     installing such a checkpoint.

8. **R8. The standard for the merge copy paths** (open question 3). Re-pinning the
   merge fixture is right, but the checks the design names are not enough. The digest
   does not cover:
   - skip data;
   - block and whole-list competitive pairs;
   - norms;
   - the stored frame index;
   - region CRCs.
   A raw copy that misplaces a skip entry passes the digest and corrupts pruning.
   Required, for each of P17 and I7:
   - every section of the copied output decodes to the same content as the rebuild
     path's output for the same merge: postings with skip data and impacts, positions,
     norms and lengths, and the stored frame index and records;
   - `check()` passes;
   - `verify` and `parity` return identical results on both outputs;
   - the output is deterministic: the same inputs give the same bytes;
   - T3's row is before and after, with the fixture re-pinned in the same commit.

9. **R9. Format 13 fixes the core's sections; file order is not plugin order** (open
   question 5).
   - L2 ties three orders together: file order, plugin order, and digest order. They
     need not agree.
   - File order matters for byte identity (format 12 only) and for what an open touches.
     Pinned sections verified whole at open (`docids`, `stats`, `versions`, the key
     filter, `term_index`, `reader.hh:76-83`) cost fewest page faults when adjacent.
   - Required for format 13:
     - the header, then the core's sections (`docids`, `stats`) at fixed positions;
     - then each plugin's sections, ordered by the sink by verification class (pinned
       first) and then by layout;
     - the cold-start row (BENCHMARKS, "open time per GB") measured on the choice.
   - Digest order stays layout order (R6). `docids_section` remains only in format 12's
     layout and is deleted with format 12.

### The doc-values ruling

10. **R10. Skipping keys on documents with a value, 4,096 per group, never splitting a
    document.** The skip index answers two questions:
    - which ordinals a range filter can skip while iterating;
    - which values an aggregation over a filtered set decodes.
    Neither choice in the question answers both.
    - **Keyed on raw ordinals** (D4's text, `phase-4.md:174-175`): a column at 1%
      density puts about 41 values in a 4,096-ordinal group. The skip index then grows
      toward the data's own size, and most groups are nearly empty.
    - **Keyed on value index** (the draft, `docvalues.hh:66, 86, 117-119`): a
      multi-valued document can straddle two groups. Its group bounds then describe
      parts of documents, and skipping by ordinal needs the inverse of rank and offsets
      to find where a group starts. The draft's group entries hold only a minimum and a
      maximum (`group_entry = 16`, `docvalues.hh:86`), with no ordinal span.
    - **Required:** a skip group is 4,096 consecutive documents that have a value, all
      of their values included. Each entry records:
      - the first and last ordinal;
      - the first value index;
      - the minimum and maximum over its values;
      - its value count.
      A range iterator binary-searches the entries by first ordinal and skips whole
      ordinal spans. An aggregation intersects its matches with each span and decodes
      only the value range `[first value index, next group's)`. For dense single-valued
      columns all three keyings coincide, so nothing is lost there.
    - The 128-value packing blocks stay in value-index order. They are the encoding,
      not the skip index.
    - D4's text changes from "blocks of 4,096 ordinals" to this. The group size stays a
      preset recorded per column, as the draft already does.
    - **Check:**
      - range iteration with skipping against brute force on sparse (0.1%, 1%),
        multi-valued (1–100 values per document), and dense columns;
      - aggregation decode counts on the same columns.

### Text and checks

11. **R11. Definitions and fixtures.**
    - "`fuzz_segment` corpus replay finds the same verdicts" (step 2, `layers.md:553`)
      means the same accept-or-reject outcome and the same exception type per input,
      not the same message. L7's rule loop may change which of two damaged sections is
      reported first.
    - Step 0 also pins:
      - a keyless segment (no `pk_filter`, which `writer.hh:451` omits when no key was
        added);
      - an empty segment, if `flush` can produce one;
      - a merge of inputs with deletes;
      - a segment written with `wide_skip_offsets`.
      Each is a byte path that only one branch exercises.
    - L10's `plugin_part_t` maps `docids_section` to "an empty base". Each layout entry
      needs a distinct empty base type, or two empty entries become a duplicate base. The
      design says so.
    - The `static_assert` that pins `sizeof(document)` (`layers.md:458`) holds through
      format 12. At format 13 the core part gains `key`, `doc_bytes` changes, and so
      does every flush trigger. That change is measured with format 13's indexing rows,
      and the design states it.

## Conditions

1. **N4 stands.** No step lands on `main` before Phase 3's ratchet rows are recorded at
   a stated commit. Steps may be written and reviewed on the branch before then.
2. **CI green on every platform per step** (L14), with `multisegment_test` under TSan
   at steps 3 and 4 as the design says, and at step 5 as well, since merges run beside
   flushes.
3. **One A/B report per measured step** in `docs/perf/`: steps 2, 3, 5, and 6 (R4). It
   records the commit pair, every row's interval, and the byte-identity hashes.
4. **Phase 3R's ordering** (L14). If Phase 3R's `strata::group` code is ready to depend
   on the index API before step 6 lands, it builds against the aliases (`strata::index`,
   `strata::segment`, `strata::document`), which the design keeps source-compatible. No
   group code names a plugin type before format 13.

## Findings

1. **The default `stored` merge decompresses one frame per document**
   (`layers.md:318, 322-324`; `reader.hh:209-249`). Today's merge does the same through
   `seg.stored(d.ord)` (`merge.hh:77`), so byte identity holds. It is the reason I7
   matters, and it is a cost the step-5 merge row will show.
2. **Random access in the doc-values draft decodes a whole 128-value block**
   (`docvalues.hh:400-404`). Sort keys, collapse, and scored filters read one value per
   document. An extraction of one lane, as `segment::length` does for lengths
   (`reader.hh:416-426`), avoids decoding 127 unused values. This lands before the sort
   and filter classes are measured.
3. **The scorer interface stays virtual** (`search.hh:121-126`), as L11 says. The
   refactor neither adds nor removes that dispatch, so L15 measures only what the
   refactor changes. That is the right scope for N4.
4. **`statistics(const segment&)`** (`search.hh:54`) mixes the core's document count
   with `inverted`'s total length. L11 gives it `inverted`'s reader, which carries a
   copy of the count. The two counts are checked equal at open, as `reader.hh:110`
   checks the footer's today.

## Which code starts when

| Step | Starts | Waits for |
|---|---|---|
| 0: fixtures pinning today's behavior | Now, with R11's additions | Nothing |
| 1: `section_io.hh` moves | Now | Nothing |
| 2: open path, plugin readers | After R2 and R3 are in the design | Its mandatory A/B (R4) before step 3 lands |
| 3: flush path, builders and sink | After step 2 | R1 before builders take parts; its A/B |
| 4: composed documents, `basic_index<L>` | After step 3 | R1 |
| 5: merge through L6 | After step 4 | R8's checks written into the step; the merge rows (R4) |
| 6: search on plugin readers | After step 5 | L15 as amended by R4, against the recorded Phase 3 commit |
| 7: flush memory charge | Not in this refactor | R5: Phase 3's D17, before the ratchet rows are recorded |
| 8: format 13 on the plugins | After step 6 | R6, R7, R9; Phase 4's freeze conditions |
| Doc-values skip index | R10's group layout on the draft branch now | D4's text updated to R10 in the same commit |

## Revision check 1

- Date: 2026-10-15.
- Reviewed: `docs/design/layers.md` revision 1 at `6022d98` (branch `phase4/layers`),
  against R1–R11 and the conditions above.
- Checked by reading the revision and the code it cites. Nothing was built or run.

### Verdict

**APPROVE.** R1–R11 are closed as specified, the conditions are carried into L14 and
L15, and steps 0–6 may proceed under N4. Nothing lands on `main` before Phase 3's
ratchet rows are recorded at a commit that includes D17's flush charge.

My rulings on the two new questions are S1 and S2 below. Both are format-13 text, and
they land in the design before step 7. Neither touches steps 0–6.

### Status

- **R1** (`244-283`): `validate_context` carries the options, the key prefix, and the
  slot table as of the operation, earlier declarations in the same transaction
  included. `bytes` and `disk_estimate` stay pure functions of the part.
- **R2** (`404-430`): `section_handle`s are resolved at construction; `need()` is
  today's add-and-verify. The lengths atomic moves into a heap object owned by
  `inverted`.
- **R3** (`214-234`): `deps` is built at call time, nothing stores a sibling pointer,
  and the move `static_assert` and the ASan test are in step 2.
- **R4** (`677-715`): both indexes; exact allocation equality; serving memory; the
  timed merge with hashes and T3; the ≤ 1.03 rule with a ten-trial rerun; Linux and
  FreeBSD; A/B at steps 2, 3, 5, and 6.
- **R5** (`330-336`, `655-657`): the charge leaves the refactor, and the recorded
  baseline includes it.
- **R6** (`466-502`): format 12 is pinned; format 13's `D` holds only core fields.
- **R7** (`717-737`): the plugin set and `unsupported_index` at open and `install`;
  `unsupported_segment` stays as defense. Open: S2.
- **R8** (`384-397`): all five checks.
- **R9** (`146-186`): three independent orders. Open: S1, which revises my ruling.
- **R10** (`376`): deferred to `phase4/docvalues`, as ruled.
- **R11** (step 0's fixtures in L14; distinct empty bases and the `sizeof` pin in
  L10).

### Rulings

1. **S1. The pinned run goes at the end, the core's sections included. Fixed, not
   measured.**
   - R9 asked for pinned sections to be adjacent. "First" was a means, not the
     requirement. The end placement meets the requirement better on both sides:
     - **Write side.** It costs no extra pass. "Pinned first" spills and copies
       `postings`, the largest section, at every flush and merge (`writer.hh:421-427`
       shows the copy `positions` already pays). That is a certain indexing and
       write-amplification cost.
     - **Read side.** An open already reads the footer and the table of contents at the
       end of the file. Pinned sections just before them make one contiguous tail read.
   - Revision: the header alone opens the file. The core's `docids` and `stats` join
     the pinned run at the tail instead of sitting after the header. Both are built
     from per-document fields known at `add`, so nothing waits on them. The tail is
     `docids`, `stats`, then each plugin's pinned sections in layout order, then the
     table of contents and the footer. Streamed sections (`postings`, `positions`,
     `terms`, `stored`, and Phase 4's columns and trees) come between the header and
     the tail, in layout order. This replaces R9's "after the header".
   - Measuring "pinned first" as a second candidate would mean building a layout whose
     write cost is certain, to learn a read gain that the tail placement already
     delivers. So the order is fixed. The cold-start row (open time per GB, first query
     with a dropped page cache) and the indexing rows are still reported for format 13
     against format 12, so the choice is shown, not assumed.
   - **Check:** format 13's fixture pins the order. A reader test confirms that open
     touches only the header page and the tail run, by counting the verified-region
     pages a fresh open reads.

2. **S2. Version scheme: one format version per segment, recorded in the segment; the
   manifest keeps only a summary.**
   - The draft has two version notions that do not meet:
     - `open_context::version` is the segment's single format version (`415`), and a
       plugin picks its N − 1 decoder by it (`456`);
     - `P::section_version` (`257`) and L16's per-plugin range in the manifest imply
       versions per plugin, which no segment records.
   - Required, one of:
     - **(a) Recommended.** Section versions are a function of the segment's format
       version. `P::section_version` is deleted. The manifest records the plugin set
       (identifiers) and the lowest and highest format version among its segments.
       Every format bump makes each plugin carry an N − 1 decoder, which is the same
       code for any plugin whose sections did not change. This matches PLAN's practice
       of one format version per phase.
     - **(b)** Plugins version independently. Then each segment records each present
       plugin's section version (in its table of contents or beside the structure
       flags), `open_context` passes the plugin's own version, and the manifest's range
       per plugin is a summary checked against the segments at open.
   - **No per-segment record in the manifest** under either scheme. A merge policy that
     prefers rewriting N − 1 segments reads the version from the open segment, which
     the index holds already (`index.hh:736-744`). A copy in the manifest would be a
     second source of truth that can disagree with the file.
   - **Check:** L16's three manifest tests, plus a test where the manifest's summary
     disagrees with a segment it names. That disagreement fails the open as corruption.

### Findings

1. **"Nothing for a document"** (`486-488`). Under format 13's split digest, the rule
   for when `inverted` emits a length record must be stated on content, not on how the
   document was handled. Two candidates: emit when the document has an `inverted` part
   with at least one term; or emit for every document in a layout with `inverted`,
   length 0 included. Either gives the same result on every replica. The design picks
   one, so two implementations of the same layout cannot differ.
2. **L16's tests are listed as step 7's** (`735-737`). That is right, since the manifest
   change is format 13. The first draft's step 7, the flush memory charge, is gone from
   the table, so the numbering no longer collides.

## Re-check (revision 3)

- Date: 2026-10-17.
- Reviewed: `phase4/layers-refactor` at `707066b` (worktree `strata-wt/refactor`), 19
  commits on main `58eda08`: revision 3 of the design (`a874ab0`), step 7a (`d911396`),
  step 7b (`a86610e`), the ledger, and `docs/perf/layers-refactor.md`. Revision 2
  (`73bc792`, which folded S1, S2, and revision check 1's finding 1) had no check of its
  own; this one covers it.
- Checked against: `calibration.md` on main, its three checks applied (a mechanism built
  twice; CI on every platform for the reviewed commit; a cited row without a raw file); R1
  to R11, conditions 1 to 4, findings 1 to 4, and revision check 1 above; format 13's
  preliminary review at `4266374` (F13-R2, R3, R4, R5, R9, R10's condition, R11, C6, and "The
  rebase onto layers"); the system review (SYS-1, SYS-6, SYS-9, SYS-16); PLAN's "What waits"
  (PLAN.md:16-20); the lead's index at main `5a8e239`.
- Checked by:
  - The rebase: `git range-diff 3ec4bdf..c8c03f6 58eda08..8645134` finds all 14 earlier
    commits patch-identical, so earlier citations map one to one (`c8c03f6` is `8645134`,
    `894c04a` is `6681f75`). Between the two bases main's code changed only `bisection.hh`
    (`adb329e`, joining threads by hand) and a VM script. `a86610e`, `1d60098`, and
    `707066b` share one code tree; the last two change only the ledger.
  - Reading the three diffs whole, and the code they touch at `707066b`:
    `include/strata/{merge,reader,index,section_io,writer}.hh`, `test/fixtures_test.cc`,
    `test/merge_oracle_test.cc`, `test/merge_oracle.hh`, and `tools/parity.cc`.
  - `git grep` of every `merge_segments` and `merge_layout` call at `707066b`: 15 calls in
    7 files, each set against its positional form in `d911396`'s diff.
  - Fixtures: `git ls-tree test/fixtures` at step 0 (`9bae9c8`, whose parent is main
    `58eda08`) and at `707066b` list the same 12 blobs.
  - CI through `gh`, logs downloaded and searched. Run 39070316303 at `1d60098` and run
    39074035208 at `707066b`: all eight jobs green in each, 35 of 35 tests on every job,
    `fixtures` and `merge_oracle` passing in every job. The ASan job took 52 min 25 s of its
    60-minute limit in the first run (ctest 2,974 s) and 37 min 50 s in the second (ctest
    2,142 s), on the same code. `d911396` has no run of its own; `a86610e`'s (39070279704)
    was cancelled by the next push.
  - Two probes, written in this session's scratchpad and not compiled; their code is quoted
    in R12 and in F13-R11's status, and their outcomes follow from the language rule stated
    with each.
  - Format 13's kind and bit table (`format.hh:91-162` at `4266374`), for F13-R3 and R16.
  - Xapiand at `7e8cccb7d`, MIT files only, for the host paths F13-R2 must keep:
    `src/server/search_views.cc:2292-2294`, `src/query_dsl.cc:647, 681, 732-772`,
    `src/database/utils.h:61`, and `src/database/schema.cc:9177, 9421`. Every grep excluded
    `src/xapian/`, `sortable_serialise`, `remote_protocol_views`, `xapian.h`, and `pack.h`,
    and none of them was opened.
  - Not run: no build, test, sanitizer, fuzzing, or benchmark (`quiet check` exits 1: the
    gate VM's `night_gate.sh n1017` holds the host strictly until 01:17); no VM; neither
    probe compiled.

### Verdict

**APPROVE WITH CONDITIONS: revision 3 folds format 13's findings for layers in the right
direction, step 7a reaches every caller and changes no behavior, and step 7b is right for
format 12 and for a plugin outside the layout, but 7b drops the compile-time membership
query (R12). Four format-13 gaps remain (R13 to R16); each goes into the design before the
7c commit it names.**

What holds, and reproduces:

- **Byte identity with format 12.** The 12 fixture blobs at `707066b` are step 0's, written
  by main's unchanged code, and each is compared byte for byte: four flushed segments, two
  merges, and the digest by `fixtures_test`; `small.sx` by `segment_test`; `permuted.sx` by
  `docids_test`; the manifest, live-docs, and metadata files by `files_test`. All pass on
  the eight jobs at `1d60098` and again at `707066b`, one code tree, with `merge_oracle`
  comparing random merges against the old merge on each.
- **7a.** All 15 calls name their fields, and each passes what its positional form passed.
  Two differ only in spelling: `reorder_test.cc:307` passed `""` and now takes the default
  empty view, equal under `merge.hh:260`'s `empty()`; `index.hh:1123` passed the default
  region shift and now takes the member default (`merge.hh:227`). `merge_oracle_test`
  drives a non-default shift (`merge_oracle_test.cc:100, 131`) against the positional
  oracle (`:135`), so a misbound field fails there; cancellation runs in
  `background_merge_test.cc:618, 642`.
- **7b.** Format 12's mask holds every entry (`reader.hh:789-791`), the answer step 6's
  `index_of<P>() < N` gave; a plugin outside the layout takes a discarded `if constexpr`
  branch with no load (`reader.hh:772-778`); the mask moves with the segment, and the
  nothrow-move `static_assert` (`reader.hh:821`) compiles on every job. Its only caller is
  the new test, after a move (`fixtures_test.cc:126-129`).
- **L3's expansion rule is complete.** With K the key prefix and L a pattern's literal
  prefix, exactly one case holds: K is a prefix of L, and every match is a key; L is a
  proper prefix of K, and matches lie on both sides, disjoint by rule 2; or neither is a
  prefix of the other, and no match is a key. Xapiand's id prefix is `Q` (`utils.h:61`;
  `schema.cc:9177, 9421`). Its index listing (`OP_WILDCARD` over `QK<path>`,
  `search_views.cc:2292-2294`) and its id field's `*`, partial, wildcard, and fuzzy queries
  (`query_dsl.cc:647, 681, 732-772`; the fuzzy one fixes the field prefix and type byte)
  have literal prefixes that start with `Q`, so they expand over `keys` alone and find what
  format 12's dictionary walk found, provided every key starts with K and is stored as named
  (R13).
- **The constraints until 7c hold.** The branch edits neither `format.hh` nor `keys.hh`
  (`git diff 58eda08 707066b`), adds no merge or scan code that depends on the key prefix
  (7a moves the parameter into `merge_options`), and keeps `section_rule` and `built` as
  they were (`section_io.hh` last changed at 5c, `d09da01`).

What blocks: nothing blocks 7a, 7b, or format 13's rebase onto `707066b`. R12 is required
on this branch before the full review; R13 to R16 before the 7c commits each names; R17 to
R19 before the full review.

### Status

Revision 3's items:

- **F13-R2: partially closed.** `layers.md:247-249` (`scan` takes a prefix), `263-266`
  (`deps` empty from format 13), `268-287` (the routing, disjointness, expansion, and the
  key's bytes). The routing holds for prefix and wildcard expansion and for Xapiand's index
  listing (above); for Xapiand it also rests on F13-R10's seam inventory, exactly one
  `Q`-leading term per document (`format-13-design.md:512-515`), which stands. Open: R13,
  since the refusal sits only in the builder, the prefix has no stated home, keys need not
  start with K, and buffered documents have no key leaf.
- **F13-R3: partially closed.** `layers.md:318-321` (the concept's `structure` and
  `structures`), `341-347` (the mask; `index_sort` the core's; a sub-structure's bit
  without its plugin's is corruption), `349-355` (one per-kind table replacing
  `section_rule` at 7c), `560-566` (presence by the requiring bit, both ways), `568-573`
  (an unreadable set bit), `720-726` (`holds<P>()` per segment), and `807` (7c's table and
  its free-and-absent cases). With one table whose rows each name one owner, a format-13
  section is claimed and verified by its class, absent with its bit clear, or refused, so
  none opens unread. Format 12 at `707066b` still leaves an unclaimed kind unread
  (`reader.hh:111-125`), as main does (`default: break`); that is the refactor's no-change
  rule, and it goes with format 12. Open: R16 (which bits a binary reads; the core's
  `index_sort`; the tests) and R12.
- **F13-R4: closed.** `layers.md:200` (streamed and first-use sections with `lengths` last;
  then `docids`, `stats`, `index_sort`; then `norms`, `term_index`, `versions`,
  `key_index`), `204-214`, and `638-647` (the 20 bytes at `term_index`'s head, resealed with
  every other checksum; `lengths` read on first use only). Its check is R15.
- **F13-R5: closed.** `layers.md:614-619, 807`: one 7c commit carries the split, the key in
  `D`, every document's length record, version 0, the dropped interim `P` record per key,
  `digest.txt` re-pinned, and T2's schedules. Read with "drops any interim record", 7c's
  keys commit keeps that record, which holds the pinned value until F21's commit, so the
  value changes once; 7c's row should say so (R19).
- **F13-R9: closed on the layers side.** `layers.md:616-619` re-pins the fixtures that
  write a keyless document with a version (`fixtures_test.cc:79` writes `1 + d % 3` with an
  empty key). The refusal itself is F13-R9's fix (`format-13-design.md:501-502`), made in
  7c's keys builder.
- **F13-R11: closed by 7a.** `layers.md:445-467, 805`; `merge.hh:224-233` (the options),
  `238-248` (the core reads them), and `380-383`; `index.hh:1123-1124`;
  `tools/parity.cc:225`; the five tests. The oracle keeps its positional signature
  (`test/merge_oracle.hh:32-33`), as a verbatim oracle should. Probe, not compiled:
  `const strata::merge_options positional{12, &token};` compiles, since the struct is an
  aggregate. None of the 15 calls initializes it positionally, so the protection is a
  convention, which R19 asks the design to say.
- **SYS-16, layers' item: closed by 7b.** `reader.hh:769-778, 787-791, 816`;
  `fixtures_test.cc:126-129`; `layers.md:720-726, 806`. Open: R12. The lead's part of
  SYS-16 remains: the index's layers row still reads "rebase onto main (P1); step 5"
  (`README.md:12` at `5a8e239`).
- **SYS-6: partially closed.** PLAN writes the owner's exception (PLAN.md:16-20), and L15
  applies it (`layers.md:852-856`). Open: step 2's `COUNT` row on the stratified terms
  (1.156× from one round, `docs/perf/layers-refactor.md:11`) has not rerun; it is due under
  a granted hold before step 3 lands (C5).
- **SYS-1, layers' side: closed.** The four conflicts are ruled into L3 (routing), L4 and
  L7 (bits), L2 (order), and L8 (the digest) (`layers.md:49-63`), and 7c's commit order
  (`layers.md:807`) is format 13's review's (`format-13-design.md:163-166`). The lead's
  rebase decision is recorded (`README.md`, "Queued", item 1, at `5a8e239`).

Revision 2's items, checked here for the first time:

- **S1: closed in the design** (`layers.md:200, 204-214`), with F13-R4's tail. Its check,
  as I wrote it, cannot see what it bounds: R15.
- **S2: closed.** No `section_version` in the concept (`layers.md:313-328`);
  `open_context::version` (`526`); one version per segment and the manifest's summary
  (`870-877`); the disagreement test (`890-893`).
- **Revision check 1, finding 1: closed** (`layers.md:604-606`), but stated on the layout,
  which contradicts L12: R14.
- **Revision check 1, finding 2: closed.** The step is now 7c, and L16 still says "step 7"
  (`layers.md:890`): R19.

The first review's items:

- **R1: closed.** `validate(vc, part)` and `key_of(vc, d)` (`reader.hh:220, 441, 541`);
  `validate_context` holds the prefix only (`reader.hh:63-65`) until D3 brings the slot
  table.
- **R2: closed.** Handles taken at construction (`reader.hh:259-265, 285-291`;
  `section_io.hh:236-241`).
- **R3: closed.** `deps` built at each call (`reader.hh:766-768, 796-800`), the move
  `static_assert` (`821`), and the move-then-lookup test under ASan
  (`fixtures_test.cc:113-125`).
- **R4: closed in the design** (`layers.md:816-860`); its rows are open (condition 3).
- **R5, R6, R8: closed** (`layers.md:410-415`; `596-619`; `497-510`, with no copy path
  yet).
- **R7: closed in revision 1;** C6 of format 13's review refines it: R14.
- **R9: superseded** by S1 and F13-R4. **R10: deferred** to `phase4/docvalues`, as ruled.
- **R11: partially closed.** The fixtures (`fixtures_test.cc:99-110, 131-137`), distinct
  empty bases (`reader.hh:654-668`), and the `sizeof` pin (`reader.hh:716`) are in; the
  verdict rule is not what step 2 met: R17.
- **Findings 1 to 4.** 1 is superseded: main's merge already decodes each frame once per
  input (P1-R4 (b)), as do the oracle (`test/merge_oracle.hh:132-134`) and the plugin merge
  (`merge.hh:202-209`), so L6's text is stale (R19). 2 belongs to `phase4/docvalues`. 3 is
  closed (`layers.md:736-741`). 4 is closed: the inverted reader copies the core's count at
  open (`reader.hh:255`).
- **Condition 1 (N4): holds.** Nothing of the branch is on main; Phase 3's rows are being
  measured tonight.
- **Condition 2 (CI green per step): partially met.** CI ran only at pushed heads:
  `1d60098` and `707066b` are green on all eight, with 7b's code; `d911396` never ran, and
  `a86610e`'s run was cancelled. C6.
- **Condition 3 (one A/B report per measured step): open.**
  `docs/perf/layers-refactor.md:7-14` records no commit pairs, intervals, or byte-identity
  hashes; steps 5 and 6 are unmeasured; step 2's row covers queries only (no allocations,
  serving memory, merged index, or FreeBSD), and step 3's +0.9% peak RSS is unexplained
  (row 3). L13's code size and compile time are unmeasured (`layers.md:782-785`).
- **Condition 4 (Phase 3R's ordering): holds.** No group code exists.

### Findings

12. **R12. `holds<P>()` lost its compile-time form** (step 7b; L11; L14 7b).
    - Step 6's `static constexpr bool holds()` (`reader.hh:771` at `8645134`) answered "is
      P in this layout" at compile time. 7b makes it a runtime member
      (`reader.hh:771-778`) and adds no static replacement.
    - L11 says code generic over layouts "can ask before calling `get<P>()`, which does not
      compile there" (`layers.md:724-726`). It cannot: a runtime `if` instantiates its
      body, so `get<P>()`'s `static_assert` (`reader.hh:763`) still fires. Probe, not
      compiled: `if (s.template holds<values_plugin>()) { return s.template
      get<values_plugin>().count(); }` in a function template, called with a
      `search_layout_12` segment, fails to compile.
    - 7b's check asks for false "at compile time" for a plugin outside the layout
      (`layers.md:806`); the test checks it at run time (`fixtures_test.cc:129`).
    - It matters at 7c and in Phase 4: `document_layout` and the appended `values`,
      `points`, and `term_lists` plugins make code generic over layouts, which needs the
      static test to compile and the runtime test to read the file.
    - Fix: keep both. A `static constexpr bool contains()` beside `holds<P>()` returns step
      6's `index_of<P>() < sizeof...(Entries)`; generic code writes
      `if constexpr (S::template contains<P>()) { if (s.template holds<P>()) { ... } }`.
      The 7b test adds `static_assert(!segment::contains<outside_plugin>())`, and L11's
      sentence names both.

13. **R13. F13-R2's disjointness is enforced at flush, not at stage 1, and the key prefix's
    home in format 13 is unstated** (L3, rules 1, 2, and 4; L1; L4; L5; L10; L11).
    - Rule 2 has the `inverted` builder refuse a key-prefixed term (`layers.md:277-280`).
      Nothing at stage 1 refuses it: format 13's `key_of` reads `d.key` (`layers.md:689`),
      and `inverted`'s `validate` checks emptiness, the term limit, uniqueness, and
      positions (`reader.hh:220-232`). A host that leaves its id term in `terms` passes
      stage 1, and the builder throws later, in a background flush, on a document the index
      acknowledged. Strata's pattern is the reverse: stage 1 rejects a repeated term
      (`reader.hh:221-231`), and the writer's throw (`writer.hh:165`) is a backstop no index
      call reaches.
    - Rule 1 reaches every key only if every key starts with K. Today that holds because
      the key is the term found by its prefix (`reader.hh:441-452`) and because
      `replace_key` and `delete_key` reject a key without it (`index.hh:1576`). The design
      gives format 13's `d.key` no such check, so an add whose key lacks K is invisible to
      every term query and wildcard, the index listing included.
    - The prefix's home: L1 moves `key_prefix` to the `keys` plugin's options
      (`layers.md:147`); `validate_context` carries it "until format 13" (`layers.md:308`;
      `reader.hh:64`); `flush_context` has no field for it (`layers.md:379-384`); and rule
      2 has it reach the `inverted` builder "as the `keys` plugin's option" (`279-280`),
      which makes `inverted` read `keys`' options: a dependency in the direction rule 1
      removes. The planner, which needs both readers and the prefix, has no stated home.
    - Buffered documents: L10's buffer-path predicates (`layers.md:701-705`) have no key
      leaf. Under rule 2 a buffered document's terms hold no key, so delete by query (D16)
      on an id term or an id wildcard misses every buffered document.
    - Fix, in the design, before 7c's keys commit:
      1. Stage 1 rejects a term with K in `inverted`'s part as `invalid`, in `inverted`'s
         `validate` with the prefix from `validate_context`; the builder's throw stays as
         the backstop, as `writer.hh:165` is.
      2. In a layout with `inverted` and a key prefix, stage 1 requires `d.key`, when
         present, to start with K: the add path's form of `index.hh:1576`.
      3. The key prefix stays the index's, recorded in the manifest, and the core passes it
         to whoever needs it: `validate_context` (dropping "until format 13"), a
         `flush_context` field for the backstop, `check()`, and the planner. No plugin
         reads another's options, and L1's row (`147`) changes. L11 places the planner over
         `basic_segment`, which reaches both readers through `get<P>()`.
      4. L10 adds the key leaf over the buffered document's key to the buffer path's
         predicates, under the same routing.
    - Automata's `expand.hh` (SYS-9) takes `keys`' scan for rule 3's first two cases; its
      reviewer checks it against L3.

14. **R14. The plugin set: L16 lists the writing binary's plugins and L8 keys the length
    record on the layout; both need the plugins the index holds** (L16, L8, L12; format
    13's C6).
    - L16 records "the identifiers of the writing binary's plugins" (`layers.md:864-867`),
      and `install` checks the same set (`885-886`). The first binary that appends a plugin
      lists it in every index it writes, so a replica on the binary before it refuses that
      index and every checkpoint with `unsupported_index`, whether or not any segment holds
      the new structure. That breaks Track D8's rolling upgrade and the owner's N and N − 1
      rule (PLAN, SYS-3). Format 13's C6, rule 3, says so and "refines L16"
      (`format-13-design.md:721-725`); revision 3 relies on C6's outcome in L7
      (`layers.md:572-573`) and leaves L16 as it was.
    - L8 has `inverted`'s length record emitted by "every document in a layout with
      `inverted`" (`layers.md:604-606`), while L12 says a `search_layout` index holding
      only keys and stored records digests equal to a `document_layout` index
      (`767-769`). Keyed on the layout, the first emits a length record per document and
      the second none, so the digests differ.
    - Fix, one rule for both, before 7c's F20 and F21 commits:
      1. The manifest lists the plugins whose structures the index may hold. A plugin joins
         when the index first needs it (its profile for `inverted`, `keys`, and `stored`; a
         slot declaration for `values` and `points`), never only because the binary has it;
         a binary writes a plugin's structure only into an index whose manifest lists it;
         and under mixed binaries a plugin joins only once every replica's binary has it.
         These are C6's rules 3 and 5, for every appended plugin.
      2. L8's exception is stated on that set: every document of an index whose set
         includes `inverted` emits the length record, length 0 included. Revision check
         1's owner choice stands, L12's equality holds, and two binaries digest one index
         equally. If the owner meant the compile-time layout, L12's sentence goes instead.

15. **R15. S1's open check, as I wrote it, cannot see the reads it bounds** (S1; F13-R4,
    fix 5; L14 7c).
    - Revision check 1 asked for "counting the verified-region pages a fresh open reads"
      (`layers-design.md:376-378`), and L14 7c repeats it (`layers.md:807`). Only
      regions-class sections become verified-region spans (`reader.hh:116-124`); pinned
      sections are checksummed straight from the file's bytes (`126-128`) and never counted
      (`171`). After a fresh open the count is 0 whether the pinned run is contiguous,
      split, or at the front of the file.
    - Fix, before 7c's T7 test: the test copies the segment into an anonymous mapping and
      removes access (`mprotect`, `PROT_NONE`) to every whole page strictly between the
      header page and the page holding the tail's first byte, then opens it, so any read
      outside the header page and the tail faults; it also asserts `verified_regions() ==
      0` after the open. The fixture pins the order.

16. **R16. Which set bits a binary reads, and one derivation of presence** (F13-R3; L7;
    L11).
    - L7 refuses "a set bit this binary cannot read" (`layers.md:568-573`) without saying
      which bits a binary reads. For a plugin's bits it is the layout's `P::structures`.
      For the core's `index_sort` (bit 128, `format.hh:129` at `4266374`) the owner is
      always present, so "the owner is in the layout" decides nothing, and L7 calls it
      unreadable "until their readers land" (`573`). The core's open names its own kinds as
      header, docids, and stats (`547-548`), so an `index_sort` entry would be unclaimed
      and, by the same paragraph, corruption: two answers for one bit.
    - 7b computes the mask after the readers (`reader.hh:756`; members at `814-816`). At 7c
      a reader opens empty when its bit is clear; if each reader also reads the footer for
      that, `holds<P>()` and the reader's emptiness become two derivations of one fact.
    - 7c's checks list the free-and-absent cases (`layers.md:807`) but not the three
      classifications of an unreadable set bit.
    - Fix, before 7c's F2–F6 and F9 commits:
      1. L7 defines the readable bits: the core's (with `index_sort` once its reader
         exists) and each layout entry's `P::structures`. Any other set bit takes the
         three-way rule, and the core's list of its own kinds adds `index_sort`.
      2. `basic_segment` computes the mask before the readers, from the footer's flags, and
         passes each reader its bit in `open_context`, so `holds<P>()` and the reader read
         one bit.
      3. Tests: a lone segment with a set bit the binary lacks (`unsupported_segment`); that
         segment in an index whose manifest lists the plugin (`unsupported_index`) and in
         one whose manifest does not (`corrupt_segment`); and a segment with a plugin's bit
         clear, where `holds<P>()` is false and the reader answers empty.

17. **R17. R11's verdict rule is not what step 2 met** (L7; step 2).
    - L7 defines equal verdicts as the same accept-or-reject outcome and the same
      exception type per input (`layers.md:580-583`). Step 2 changed the type for 29 of the
      1,089 corpus inputs, from a leaked `std::invalid_argument` or `termdict::corrupt` to
      `corrupt_segment`, and now rejects a kind listed twice (`reader.hh:117`), which main
      accepted by using its last entry (`97d0ae6`'s message;
      `docs/perf/layers-refactor.md:11`).
    - Both are fixes: `heal` and `media_test` rely on damaged input throwing only
      `corrupt_segment`. But step 2's own check fails as written, and the ledger's
      deviations (`layers-refactor.md:23`) list neither.
    - Fix: L7's rule becomes the same outcome per input, with an exception's type allowed
      to change only from a leaked type to `corrupt_segment`; the ledger lists both
      changes; the full review rules on the duplicate-kind rejection as a behavior change.

18. **R18. Design claims the code does not build, missing from the ledger's deviations.**
    For the full review's inventory:
    1. No `section_plugin` concept and no `plugin_base<Derived>` (`layers.md:222-224,
       313-328`): nothing checks a plugin's shape.
    2. Of L2's compile-time checks (`layers.md:216-218`), only dependency order is asserted
       (`reader.hh:798`); nothing checks that no two plugins claim a kind or that
       `docids_section` appears once. At 7c the per-kind table gives the first by
       construction.
    3. L12's compile-only `document_layout` test against a stub `keys`
       (`layers.md:758-759`) does not exist.
    4. The digest: L3 has the key invariant read `keys`' scan (`layers.md:253-254`), and
       L8 has the core assemble `D` from `legacy_document_fields` (`590-593`); the code
       walks `inverted.scan_terms` with a prefix test (`index.hh:561-569`) and names the
       three readers (`559`). The index's merge calls the `search_layout_12` wrapper
       (`index.hh:1123`; `merge.hh:34, 380`), not `merge_layout` over its own layout.
    5. Format 12's `keys` scan returns non-key terms for a prefix shorter than K
       (`reader.hh:1442-1452`); its one caller passes K (`merge.hh:260-263`).
    6. `fixtures_test`'s empty flush passes if the writer starts refusing it
       (`fixtures_test.cc:131-137`), though `empty.sx` is pinned: one byte path can change
       without a failure.
    - Fix: build each, or list it as a deviation with the step that closes it; make the
      empty flush a failure when `empty.sx` exists. Before the full review.

19. **R19. Text.**
    1. `layers.md:238-243`: `key_reader` uses `prefix`, which its parameter list
       (`238-239`) does not declare.
    2. `layers.md:57, 58, 60, 349, 354, 616, 890`: "step 7" now means 7c.
    3. `layers.md:5` omits F13-R9, which L8 folds (`619`), and the revision table has no
       F13-R9 row.
    4. `layers.md:789-791`: every step keeps format 12 byte-identical and the API
       source-compatible; 7a changes `merge_segments`' signature on purpose (every caller
       is in this repository), and 7c is format 13.
    5. `layers.md:463-465`, `merge.hh:224-225`: the named options protect designated
       callers only, since positional aggregate initialization compiles; say so, and make
       "designated only" a review rule.
    6. `layers.md:720-721`: `basic_segment` computes the mask from the core, as the
       ledger's deviation says (`layers-refactor.md:23`).
    7. `layers.md:485`: today's merge no longer decodes one frame per document (findings,
       item 1).
    8. `layers.md:455`, `merge.hh:230`: an empty `order` is docid order except for a single
       permuted source, which keeps its order (`merge.hh:290`).
    9. `layers.md:200`: format 13's "layout order" is the table's rank per kind; the layout
       check asserts that a layout lists its plugins in rank order, or the text says rank.
    10. `layers.md:807`: 7c's keys entry says it keeps the interim `P` record per key until
        F21's commit (F13-R5's status).
    11. `layers.md:3`: "revision 2, approved": revision 2 had no check before this one. The
        ledger's "revision checks 1 and 2 APPROVE" (`layers-refactor.md:2`) names a check
        this file does not hold.

For the lead, outside this review's subject: the ledger's pre-existing versions finding
(`layers-refactor.md:24`) is broader than it says. The writer's width is
`bit_width(hi − lo)` (`writer.hh:686`), and the reader requires `lo ≤ 2^64 − 2^width`
(`reader.hh:506`), so it refuses a valid file whenever `lo > 2^64 − 2^width`, which a valid
file can reach at every width above 1, not only at 64. It is main's (main's
`reader.hh:420`) and belongs with format 13's F9 encoding of versions.

### New blocking items

None blocks 7a, 7b, or format 13's rebase onto `707066b`. Required, each before the point
named:

- R12, on this branch: before the full review, and before 7c's F9 commit calls
  `holds<P>()`.
- R13, in the design: before 7c's keys commit.
- R14, in the design: before 7c's F20 and F21 commits.
- R15, in the design: before 7c's T7 test.
- R16, in the design: before 7c's F2–F6 commit.
- R17, R18, and R19: before the full review.

### Conditions

- Conditions 1 to 4 carry, with the status above.
- **C5. SYS-6 and step 3, before step 3 lands.** Under a hold the lead grants, step 2's
  `COUNT` row on the stratified terms reruns with L15's ten trials on the rebased pair
  (`0707741` against `97d0ae6`), and step 3's +0.9% peak RSS is explained or measured equal
  (`97d0ae6` against `731918d`). Each goes into `docs/perf/layers-refactor.md` with its
  commit pair, interval, and hashes, as evidence for keeping the step and never as a gate
  row.
- **C6. Before landing,** every commit of the series builds and passes the fast tests (`git
  rebase --exec` over `main..HEAD` once the hold clears), since CI ran only at pushed
  heads, and the ledger records its planned local ASan and UBSan run.
- **C7. The ASan job's margin.** The same code took 52 and 38 minutes of the job's 60 on
  two runners, so one slow runner stands between this branch and a cancelled job. The split
  of main's ASan job that format 13's F13-R16 asks for lands before a landing criterion
  cites this job.
- **C8.** The full review of steps 0 to 7b is the format-and-durability tier: a fresh code
  reviewer and the architect's full design review, with R17 and R18 as their starting
  inventory.

### In one line each

- Verdict: APPROVE WITH CONDITIONS. 7a and 7b stand; R12 on the branch and R13 to R16 in
  the design, each before the commit it names.
- F13-R2: partially closed (`layers.md:247-249, 263-287`); open: R13.
- F13-R3: partially closed (`layers.md:318-321, 341-355, 560-573, 720-726`); open: R16,
  R12.
- F13-R4: closed (`layers.md:200, 204-214, 638-647`); its check: R15.
- F13-R5: closed (`layers.md:614-619, 807`).
- F13-R9: closed on the layers side (`layers.md:616-619`).
- F13-R11: closed by 7a (`merge.hh:224-248, 380-383`; `index.hh:1123-1124`).
- SYS-16: closed by 7b (`reader.hh:769-791, 816`; `fixtures_test.cc:126-129`); open: R12;
  the index row is the lead's (`README.md:12`).
- SYS-6: partially closed (PLAN.md:16-20; `layers.md:852-856`); the `COUNT` rerun is open
  (C5).
- SYS-1: closed on the layers side (`layers.md:49-63, 807`).
- S1: closed in the design (`layers.md:200, 204-214`); its check: R15.
- S2: closed (`layers.md:313-328, 526, 870-877, 890-893`).
- Revision check 1, findings 1 and 2: closed (`layers.md:604-606, 890`); wording: R14,
  R19.
- R1, R2, R3, R5, R6, R8: closed. R4: closed in the design, rows open (condition 3). R7:
  closed, refined by R14. R9: superseded. R10: deferred. R11: partially closed (R17).
- Findings 1 to 4: 1 superseded (R19), 2 to `phase4/docvalues`, 3 and 4 closed.
- Conditions 1 and 4 hold; 2 is partially met (C6); 3 is open.
- New: R12 (`reader.hh:769-778`), R13 (`layers.md:268-287, 308, 379-384, 701-705`), R14
  (`layers.md:604-606, 767-769, 864-886`), R15 (`layers.md:807`), R16
  (`layers.md:546-573`), R17 (`layers.md:580-583`), R18, and R19.
- Conditions: 1 to 4 carry; C5 to C8 are new.

Not run: no build, test, sanitizer, fuzzing, or benchmark (the host's strict hold until
about 01:17); neither probe compiled; no VM; no CI run at `d911396` alone; format 13's code
beyond its kind table; steps 0 to 6's code beyond what the statuses cite; no Xapian or
Xapiand GPL source.

## Landing review (layers revision 4, merge context, inverter API)

- Date: 2026-10-18.
- Reviewed: `phase4/layers-refactor` at `fb2d596` (equal to `origin`): the eight commits after
  the code review's `d508158` (`c08ad31` to `fb2d596`), with `6f57b26` read whole.
  `perf/inverter` at `35498e2` (equal to `origin`): its public headers against its base
  `a62e9f9`. `phase4/format13` at `a681347` (equal to `origin`, based on `d508158`): its keys
  and merge text.
- Checked against: `layers.md` revision 4 (L1, L5, L6, L10, L11, L14, L15); this file's R12 to
  R19, conditions 1 to 4, and C5 to C8; the code review (`layers-code.md`, APPROVE WITH
  CONDITIONS, LC1 to LC10) and the lead's rulings on it (`layers-refactor.md`); the inverter's
  CR4 (`inverter-code.md:58-73, 162-168`); `calibration.md`'s ten checks; `format-13.md:51,
  136, 543, 589, 1293` (F13-R18, F13-R38).
- Checked by: reading `merge.hh` at `fb2d596` (`:57-79, 160-170, 236-367`), `writer.hh:91-96,
  658-672`, and `reader.hh:92, 709-715`; main's `merge.hh:56-78, 169-170`; the inverter's
  `index.hh:41-56, 213, 896-897, 1942, 2087-2117`, `writer.hh:83-130, 229`, and
  `tuple_buffer.hh:112-200`; CI through `gh`: run 39181658078 at `05d9aac`, the last code
  commit, has all eight jobs green, with ASan+UBSan at 69 min 34 s of its 120; and the C5 and
  C6 records (`docs/perf/layers-refactor.md:16-27`, the ledger's C6 line).
- Not run: no build, test, sanitizer, fuzzing, or benchmark (a design review, by instruction).
  The `merge_charge` numbers are the developer's rerun of the code reviewer's probe, not mine.
  No Xapian or Xapiand GPL source was read.

### 1. The merge context (LC2, `6f57b26`)

Ruling: sound, and it lands. It moves one per-document read, `version`, from the core into the
plugin that owns it. It leaves the key collection in the core, against L1, which is format 12's
and is recorded as a deviation that 7c-3 closes (R20). The default merge's contract and five
text items follow (R21, R22).

What holds:

- **Same bytes by construction.** `keys_plugin::merge` forms each record from three things:
  `docids[i]` (the vector that `write_docids` and the footer use, `merge.hh:357, 367`), its own
  input's `version(src.ord)`, and the core's key map (`merge.hh:163-168`). Before, the core
  formed the same three values from the same reader and map, in the same order (`6f57b26`'s
  removed lines). `in[s]` is source `s`'s reader (`merge.hh:361`), and `order[i].source` is the
  sort record's source index (`:331`), so every lookup is one the old code made. `f.key` views
  the map, which lives until the merge returns (`:267`), so no document's key is copied. The
  ledger records the 20k corpus's 15 states byte-identical to main and ctest passing at
  `05d9aac`, and CI is green on all eight jobs there.
- **The charge.** The sort record shrinks from 24 bytes to 16 (`merge.hh:283`). The context
  holds `order` and `docids`, one copy of each. The code reviewer's `merge_charge` probe holds
  23,164,195 bytes against main's 23,344,211 in docid order and 19,195,979 against 19,375,995
  permuted. Peaks are 23,516,450 against 23,868,597 and 19,545,162 against 21,113,115. LC2's
  ruling, back to main's charge or below, is met.
- **Composition with format 13.** Format 13's reader keeps `version(ord)`, which returns 0 in a
  keyless segment (`format-13.md:543`), and a keyless input's map is empty (`merge.hh:267`
  sizes one map per source), so the loop holds for F13-R18's keyless segments. Format 13
  collects keys with the same scan, from the empty key and with empty deps
  (`format-13.md:589`). Its branch still reads `m.fields` (`merge.hh:63, 164` at `a681347`, on
  `d508158`), and its 7c-3 already redoes the merge's key collection (`format-13.md:1293`), so
  that commit now writes against `docids`.

#### Findings

20. **R20. The core still collects keys for the `keys` plugin, against L1's ownership** (L1,
    L6).
    - L1 gives the key collection to the `keys` merge (`layers.md:185`). In the code, the core
      does it. `merge_layout` names `keys_plugin` (`merge.hh:256`), scans its reader with its
      deps (`:268-280`), and hands the maps back through a context field that only that
      plugin reads (`merge_context::keys`, `:66`). Before `6f57b26` the core also read `keys`'
      versions. The versions moved, but the scan did not.
    - Why it stays for format 12: the scan needs `inverted`'s readers
      (`seg.deps<keys_plugin>()`), but `P::merge` receives only its own readers
      (`merge.hh:358-362`). L6 says that a plugin with deps "also receives each input's deps"
      (`layers.md:538-539`). Plumbing deps into the merge for one format-12 plugin, only to
      delete them at 7c, costs more than it buys.
    - Why it matters: the maps are the merge's largest per-document charge, 64 bytes plus the
      key per live keyed document (`merge.hh:275`; main's `:78`, so not introduced here). The
      core holds them through `inverted`'s and `stored`'s merges as well as `keys`'
      (`search_layout_12`'s order, `reader.hh:92`). Inside `keys_plugin::merge` they would live
      only while that merge runs, a lower peak. That is expected, not measured.
    - Fix: list it now in the ledger's deviations ("the core collects keys for `keys`' merge in
      format 12; closes at 7c-3"). At 7c-3, where `keys`' deps become empty (F13-R2):
      1. `keys_plugin::merge` scans its own readers from the empty key.
      2. `merge_context::keys`, `key_map`, and the core's `keyed` branch (`merge.hh:57, 66,
         256, 266-280`) are deleted, so the core's merge names no plugin except
         `docids_section`.
      3. L14's 7c row (`layers.md:910`) lists these deletions.
      4. 7c-3's `merge_charge` rerun records the peak.

21. **R21. The default merge's `doc_fields` differ from the flush's** (L5, L6, `plugin_base`).
    - The flush hands every builder `doc_fields` with docid, version, and key
      (`layers.md:424-428`; `writer.hh:91`, "the core hands every builder"). The default merge
      now forms `fields` "from `docids[ord]` and the source" (`layers.md:548-551`). A plugin
      other than `keys` reads only its own readers, though, and they hold no version or key. A
      plugin that takes the default merge and reads `f.version` or `f.key` writes different
      bytes from a merge than from a flush of the same documents, and nothing fails.
    - Nothing builds `plugin_base` yet (a ledger deviation), so this binds at the first plugin
      that takes the default merge (Phase 4's `values` or `points`).
    - Fix, in L5 and L6:
      1. Version and key are `keys`' inputs. Every other builder reads only `f.docid`, in
         flush and merge alike, and `plugin_base` passes `{docids[ord], 0, {}}`.
      2. A plugin that needs a document's key at merge declares `keys` in its deps.
      3. When `plugin_base` lands, a test for each plugin that takes the default merge shows
         that merging two flushed segments writes that plugin's sections byte for byte as
         flushing their live documents in the output's order does.

22. **R22. L5's and L6's text after `6f57b26`, and one check.**
    1. `layers.md:467-469`: "The merge holds 32 bytes a document" counts the sort record,
       `order`, and `docids`. It omits each input's ordinal map (4 bytes per input document,
       `merge.hh:290`), the copy kept for one permuted source (`by_docid`, `:300`), and R20's
       key maps. Name each, as main's.
    2. `layers.md:488-494`: `merge_context : flush_context` inherits `version`, `slots`, and
       `key_prefix`, but the built context has none of them (LC4 deleted `version`;
       `merge.hh:60-79`). List the built fields, or mark the inheritance as 7c's.
    3. `layers.md:484-487, 490`: `source_ref {input, ord}` is `merge_ref {source, ord}`
       (`layers.md:510`; `merge.hh:64`). Give one struct one name.
    4. `layers.md:538-539`: the deps sentence describes R20's 7c target, not format 12's code.
       Say so.
    5. `layers.md:524-526` and `910` delete `merge_options::key_prefix` at format 13. Format 13
       keeps it as the `inverted` builder's backstop (F13-R38, "deviation 1, accepted",
       `format-13.md:51, 136`). Layers takes F13-R38's reading.
    6. Code: `keys_plugin::merge`'s per-document loop (`merge.hh:163-168`) never calls
       `m.check()`. The key lookup moved there from the core's checked loop (`:327`), and
       `merge_options` promises a check per document (`:236`). Add one `m.check()` per
       document. This can wait for 7c-3's rewrite of the loop.

### 2. The inverter's public API (CR4, `perf/inverter` at `35498e2`)

Ruling: the host's new inputs stay public and move into `inverted`'s part. Everything else the
branch exposes is `inverted`'s machinery and leaves the public surface. The branch lands after
layers only by rebasing onto the plugins (R23, R24). The inverter has no design file, so this
is the public-API tier's design review of CR4. The code review's findings on its mechanics stand.

#### Findings

23. **R23. What stays public, and what does not** (L10, the public-API tier).
    - **Public: the host's input.** These stay public: `document::text`, `document::tokens`,
      `document::split_text` (`index.hh:46-56`), and `text_token` (`writer.hh:83-85`).
      Together they are the index's input contract, with its rules stated at the struct:
      positions strictly increase, a term is in `terms` or among the tokens but not both,
      and with `split_text` the words are maximal runs of bytes other than `' '`, at
      positions 1, 2, and so on. Stage 1 enforces the rules (`index.hh:1776-1785`, and
      `valid_words`). Two changes:
      1. `text_token` moves beside `term_entry`, with `inverted`'s part types, out of
         `writer.hh`.
      2. With `split_text`, `length` is ignored silently, while non-empty `tokens` are refused
         (`valid_words`). Stage 1 also refuses a nonzero `length`, so an ignored field cannot
         disagree with the count. This is a recommendation; the inverter decides.
    - **Not public: the machinery.** These move into a detail namespace (at the rebase,
      `inverted`'s internals) and are named in no host-facing header's API:
      - `tuple_buffer`, which sits in `namespace strata` (`tuple_buffer.hh:154`) with test
        hooks as public members (`hash_mask`, `max_run_bytes`);
      - `chunked_spill` (`writer.hh:229`);
      - `split_words`, `count_words`, `word_counts`, and `text_detail` (`writer.hh:87-130`);
      - `segment_writer::add(text, tokens)`.

      The word rule stays public as text at `split_text`. The SWAR splitter is an
      implementation.
    - **Unchecked inputs.** `tuple_buffer::add` checks none of stage 1's rules (CR4): a token
      that repeats a terms entry writes corrupt position deltas. `tuple_detail::hash` shifts by
      64 for a zero-length input (`tuple_buffer.hh:118`, undefined behavior). Stage 1 refuses
      both (`index.hh:1776, 1785`). Once the class is internal, both are preconditions that
      only the index reaches, after stage 1. State them at the type, and back them with a
      debug-build assertion of the same predicate, or with the encoder refusing a
      non-increasing position, one compare per position, if the performance engineer measures
      it as free. The rule for both branches: a public function checks its inputs, or it is
      not public.
    - Fix, before the inverter lands: the cut above, with the ledger listing the public
      additions (CR4 found the ledger's list incomplete).

24. **R24. How the inverter reconciles with layers when it lands second** (L5, L10; the
    `sizeof(document)` pin; one mechanism).
    1. **Rebase, not merge.** The inverter changes `index.hh` (567 lines) and `writer.hh`
       (197), the files that layers restructures into `basic_index` and the builders. It is
       redone on layers' structure. Format 13's 7c rewrites the same files, so the lead
       sequences the two (one writer per file set).
    2. **L10.** `text`, `tokens`, and `split_text` join `inverted_plugin::part` beside `terms`
       and `length`, and inheritance keeps `d.text` and the rest compiling
       (`layers.md:749-752`). `inverted_plugin::bytes(part)` takes the inverter's terms
       (`d.text.size() + sizeof(text_token) * d.tokens.size()`, `index.hh:897`).
       `inverted_plugin::validate` takes the token and word checks (`index.hh:1776-1785`,
       `valid_words`), with the key prefix read from `vc.key_prefix` (R13, `layers.md:765`).
       The core never reads the fields.
    3. **The pin** (`reader.hh:715` on layers; `layers.md:773-776`). `doc_bytes` charges
       `sizeof(document)` (`index.hh:897`). The part grows by 56 bytes on libc++ and 64 on
       libstdc++ (a string, a vector, and a padded bool), so every buffered document is
       charged that much more, flush triggers move, and segments flushed when the budget
       fills differ from main's. D17 charges what the buffer holds, so the charge stays and
       the pin moves: the commit that grows the part re-pins it to the new size, and its
       message names the shift. The inverter's "byte-identical segments" then holds at equal
       flush points only, which its ledger says. The indexing rows on `main` carry the shift,
       as L10 does for format 13's key. The cost falls on hosts that never send text, which
       matters for the many-indexes workload; a layout that costs them nothing (the text
       inputs behind one pointer, or in a part of their own) is the inverter's to measure.
    4. **L5's builder lifecycle.** L5 builds a builder at flush and feeds it in output order
       (`layers.md:446-448`). The tuple buffer is fed as each document enters the buffer, in
       arrival order, and gets the output order and the live set only at flush
       (`tuple_buffer.hh:175, 294, 331`). It also holds each document's stored record, key,
       and version (`:277-281`, `add`'s parameters), which belong to `stored`, `keys`, and
       the core's buffer. The rebase lists both as deviations with the step that closes
       each. Before 7c's builder commits make `P::builder` real, an L5 amendment, checked by
       the architect, states the buffer-time builder: when `add` runs, how the output order
       and the deletes reach `finish` (as `order` and `new_ordinal` reach a merge), and which
       plugin owns each column.
    5. **One inversion** (calibration's first check). At one flush thread the tuple buffer
       inverts (`index.hh:1942`). At more than one, `segment_writer::add(text, tokens)`
       inverts each docid run again (`index.hh:2087-2117`). Both paths are new for text, so
       tokens become postings in two implementations, and `inverted` gets one builder. The
       owner parked multi-thread indexing for many small indexes (`986edf3`, `b320132`), so
       my recommendation is to delete the multi-thread text path while it is parked, or to run
       one tuple buffer per docid run. The choice is the owner's, due before the multi-thread
       flush is unparked or at Phase 4's gate, whichever comes first. The two word hashes are
       the same multiply and xorshift with different tails (`index.hh:214` `key_hash`,
       `tuple_buffer.hh:114`), so the rebase's code review rules on whether they become one.

### 3. Revision 4 and the full design review (C8)

Ruling: revision 4 closes R12 to R19, and the branch carries the code review's pre-landing
conditions. Layers lands once four things are done: a docs-only commit with R20's deviation
line, R22's items 1 to 5, and R25; C6's remainder; and green CI at the tip that integrates
main.

#### Status

- **R12: closed.** `contains<P>()` is `static constexpr` (`reader.hh:806`), and
  `fixtures_test.cc:98-107` guards `get<P>()` with `if constexpr` and `static_assert`s
  membership. L11 names both (`layers.md:811`).
- **R13: closed in the design.** L5's `flush_context::key_prefix` (`layers.md:435`), L10's
  `valid` and `key_of` rows (`:765-766`), the buffer path's key leaf, L11's planner taking
  the prefix from the core, and 7c's tests (`:910`).
- **R14: closed.** L16's set rules (`layers.md:979-983`), and L8's length record on that set
  (`:680-681`).
- **R15: closed.** 7c's test (`layers.md:910`).
- **R16: closed.** L7's readable bits (`layers.md:632`). In code, `present_` is declared
  before `readers_` (`reader.hh:855-856`) and initialized in that order (`:791`).
- **R17: closed, with my error corrected.** My re-check named two step-2 behavior changes;
  there is one. Main's `decode_toc` already refused a kind listed twice, so step 2's check
  was dead (LC1), and `05d9aac` deletes it. L7 and revision 4's R17 row now say one change in
  33 inputs (`layers.md:81`), but the step-2 row of `docs/perf/layers-refactor.md:11` still
  says 29 of 1,089 (R25). For the lead's calibration log: check every behavior change a
  review names against main's code path, not only against the step's commit message.
- **R18: closed.** `section_plugin` (`reader.hh:757`) and the layout's two checks
  (`:776-777`); the digest's key invariant over `keys`' scan (`index.hh:571`); the index's
  merge over its own layout (`index.hh:1127`); the empty flush failing (`fixtures_test.cc:145-150`);
  the rest listed in the ledger's deviations. R20 adds the merge's key collection, which
  R18's inventory, mine, missed.
- **R19: closed.** Spot-checked: the header (`layers.md:1-8`), 7c's naming (`:910`), and the
  byte-identity exceptions (`:894`).
- **C5: the `COUNT` half is closed; the RSS half is partially closed.**
  - `COUNT`: 0.990× [0.987–0.993] on the stratified terms and 0.979× on the 962 gate
    queries, ten rounds, 0 mismatches (`docs/perf/layers-refactor.md:18-23`).
  - RSS: equal at the default 2,056 MB budget (3,531.5–3,531.6 against 3,531.6–3,531.7 MB,
    identical bytes), but not re-measured at the smaller budget where the +0.9% appeared
    (`:24-27`). At the default budget the buffer, not a flush's transients, sets the peak, so
    that run cannot see a per-flush cost.
  - I accept the remainder for landing under L15's rule 1, since 0.9% is under the 3% effect
    size. Its explanation carries as C9, because small budgets are what the many-indexes
    workload runs.
- **C6: partially closed.** The lead's run covers `main..d508158` (31 commits). Four code
  commits follow it: `6f57b26`, `7c12d1c`, `277d1ef`, and `05d9aac`. CI ran only at
  `05d9aac`, and the other three runs were cancelled. The same per-commit run over
  `d508158..HEAD` closes it, and it runs again over the landed series if landing rebases:
  main has nine commits since (PLAN, CI, and one doc; `git log HEAD..main`).
- **C7: closed by another mechanism.** Main caps the ASan job at 120 minutes (`584cab1`)
  instead of splitting it. The job took 36 min 38 s at `d508158` (the code review) and
  69 min 34 s at `05d9aac`, on similar code, so runners vary about twofold, and the cap
  leaves 50 minutes over the slowest run seen. Main's `d8d219c` (ASan at `-O1`, four tests at
  once) joins at landing. If a run passes 90 minutes, the split is due.
- **C8: closed by this review.** The code review's pre-landing conditions are folded: LC1 at
  `05d9aac`, LC2 at `6f57b26` (accepted in item 1), LC3 at `7c12d1c`, LC4 at `277d1ef`, and
  LC5 to LC10 at `05d9aac`. LC4's optional "one spill, two ways in" stays undone, as its
  ruling allows.
- **Condition 1 (N4): met.** Phase 3's ratchet rows are recorded at stated commits
  (`phase-3-ratchets.md` on main: strata at `5dfe415` and `2e774db`).
- **Condition 2: partially met, accepted.** Every step builds and passes on one platform
  (C6). Every platform, TSan included, ran at pushed heads that contain steps 3 to 5, the
  last at `05d9aac`. The series lands whole, so a TSan run at each step would serve only
  bisection.
- **Condition 3: open for steps 5 and 6** (`docs/perf/layers-refactor.md:13-14`, "not
  measured"). Under SYS-6 those branch rows are evidence only. The comparison that N4 requires,
  step 6 against the recorded Phase 3 commit (L15), becomes Phase 4's gate rows on main at the
  landing commit, merge rows and T3 included (C10).
- **Condition 4: holds.** No `strata::group` code exists on the branch (`git grep`).

#### Findings

25. **R25. Text before landing.**
    1. `docs/perf/layers-refactor.md:11`: "29 of 1,089" becomes 33 of 1,090 (LC1), with the
       corpus's commit.
    2. The ledger's Doing, Next, and Blocked on lines (`layers-refactor.md:23-31`) still
       describe `00139c7`: waiting on CI, C5 blocking, and C6 next. Its State line and its
       last lines say C5 is measured and C6 done. The ledger states one current state.
    3. The ledger's deviations gain R20's line, and its "Next" names R20's and R22.6's 7c-3
       items and R21's `plugin_base` test.

### Verdicts

**`phase4/layers-refactor`: APPROVE WITH CONDITIONS. It lands at `fb2d596` plus one
docs-only commit, after C6's remainder and green CI at the tip that integrates main.** The
plugin split, the merge-context change, and revision 4 hold. Every finding left is text, a
7c item, or a gate row.

**`perf/inverter`: REJECT as it stands at `35498e2`. Its mechanics hold per its code review,
but after layers it lands only through R23's cut and R24's rebase.** A re-check scoped to
the rebase decides it. The direction is accepted: one-thread inversion through the tuple
buffer, and text and tokens as public document inputs.

### Blocking items

- Layers, docs-only, before landing: R20's deviation line, R22 items 1 to 5, and R25.
- Layers, before landing: C6 over `d508158..HEAD` (and over the landed series if landing
  rebases), and CI green on all eight jobs at the tip that integrates main.
- Inverter, before landing: R23 (the public surface cut to `document`'s three inputs and
  `text_token`, the ledger listing them, and the tuple buffer's preconditions stated and
  asserted); R24 items 1 to 3 (rebased onto layers' plugins, the inputs in `inverted`'s part
  with its `bytes` and `validate`, and the pin re-pinned in that commit with the shift named);
  R24 item 4's two deviations listed with their closing steps.

### Conditions

- **C9.** Step 3's +0.9% peak RSS at the smaller budget is explained, or measured equal on
  main at that budget, with the many-indexes per-index memory rows, before those rows gate.
- **C10.** Phase 4's gate measures N4's comparison on main: the landing commit against the
  recorded Phase 3 commit, with L15's query, allocation, serving-memory, indexing, and merge
  rows, and T3.
- R20's move and R22.6 at 7c-3; R21 before the first plugin that takes the default merge; R24
  item 4's L5 amendment before 7c's builder commits; R24 item 5 (one inversion, the owner's
  call) before the multi-thread flush is unparked or at Phase 4's gate, whichever comes first.
- Conditions 1, 2, and 4 hold; condition 3 becomes C10; C5's remainder becomes C9.

Not run: no build, test, sanitizer, fuzzing, or benchmark (a design review, by instruction);
no probe compiled; `merge_charge` and C5 numbers are others' runs, read from the ledger and
`docs/perf/`; the inverter's tests and its code beyond the cited lines; format 13's code
beyond its merge and key lines; no VM; no Xapian or Xapiand GPL source.
