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
