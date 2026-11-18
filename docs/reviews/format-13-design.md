# strata review: format 13 design (preliminary)

- Date: 2026-10-17.
- Reviewed:
  - `docs/design/format-13.md` revision 0 at `e2ac3fe` (F1 to F22, T1 to T11), unchanged at
    the branch head `e779835`, and the ledger `docs/status/format13.md` at `e779835`.
  - `git log main..phase4/format13`, seven commits: the design `1bc0a38`; F2 to F6 `d1da0c9`;
    F7 `96083a4`; F8 `360dc7f` and `7c2c9bd`; F9 `e2ac3fe`; F22's tools `e779835`. Every diff,
    and the code it changes at `e779835`: `include/strata/{format,reader,writer,keys,index,
    merge,check}.hh`, `fuzz/fuzz_segment.cc`, `tools/strata_engine.cc`, and
    `test/{format13,segment,index,merge,reorder,background_merge}_test.cc`.
  - `phase4/layers-refactor` at `c8c03f6`: `include/strata/reader.hh` (the plugin readers and
    their rule arrays), `merge.hh`'s signature, `test/fixtures_test.cc`, and its ledger.
  - The system review `docs/reviews/system-2026-10-17.md` at main `586fe9d` (SYS-1, SYS-4,
    SYS-6, SYS-7, SYS-9, SYS-16) and the lead's rebase decision at main `bd0d857` (the index,
    "Queued", item 1).
  - The owner's two decisions the lead relayed during this review (2026-10-17): format 13 may
    freeze with a reserved section kind for points and a stated rule for adding it later, so
    points arrive without a format 14 (SYS-4); branch A/Bs under a granted hold are evidence
    for choosing an encoding, never a gate row (SYS-6).
- Checked against:
  - `docs/reviews/calibration.md` at main `3ca9f4f`, both rows applied: a second
    implementation of a mechanism is a finding (F13-R3, F13-R4); CI on every platform for the
    reviewed commit (F13-R16).
  - PLAN at `bd0d857`: the process (PLAN.md:3-18), Goals 1 to 5, Ratchets (Baselines,
    Reference, Passing, Carried rows), Every gate, Track P, Phase 4.
  - `docs/design/phase-4.md` (D1, D2, D4, D5, D11, D12, D14) and my review of it
    (`docs/reviews/phase-4-design.md`: R1, R4, R6, R7, R19, R20, N3, M3); `docs/design/layers.md`
    (L2 to L4, L7 to L9, L11, L14, L16, S1, S2) and `docs/reviews/layers-design.md`;
    `docs/design/workload-profiles.md`; `docs/design/limits.md`; `docs/design/frozen-index.md`;
    `docs/design/reordering.md` (Risks); `docs/gates/phase-3-ratchets.md` ("Disk after a full
    merge", "Serving memory", "Gate status"); BENCHMARKS (rule 4, the counterpart table, Ratchet
    rows); `docs/perf/query-gap-merged.md`, `p12-term-dictionary.md`, `p15-region-size.md`.
  - Xapiand at `7e8cccb7d`, its MIT files only: `src/database/schema.cc` (2136, 2880-2900,
    2960-2995, 5295-5325, 9150-9180), `src/database/utils.cc:71-78`, and
    `src/server/search_views.cc:2270-2310`. A grep across Xapiand's `src/` printed four lines of
    `remote_protocol_views.cc`, which PLAN lists as GPL (PLAN.md:552-555); the file was not
    opened and nothing below rests on it. No Xapian source, and not `sortable_serialise`.
  - Lucene and Tantivy from their documentation only. Detent's `docs/LIBRARY_BOUNDARIES.md`,
    read only for which libraries Detent links.
- Checked by:
  - Reading every diff and the code each claim cites, at the commits above.
  - A merge probe, `.scratch/review-format13/merge_probe.sh` (`git merge-tree --write-tree` in a
    scratch clone; no worktree or branch touched), output in `merge_probe.out`: `e779835` ×
    `c8c03f6` has 30 conflict hunks with 1,744 lines inside their markers in 8 files
    (`reader.hh` 12 hunks and 833 lines, `writer.hh` 6 and 464, `merge.hh` 3 and 293,
    `index.hh` 4 and 62), the same as at `e2ac3fe`.
  - CI records through `gh`, logs in `.scratch/review-format13/`:
    - Run 39060862198 (`e2ac3fe`): `linux-x86_64-clang`, `linux-x86_64-gcc`,
      `linux-arm64-gcc`, and `linux-tsan` pass. `macos-x86_64`, `macos-arm64`, and
      `freebsd-x86_64` fail to build on main's `std::jthread` (pre-existing, SYS-5).
      `linux-asan-ubsan` stops at its 60-minute limit after 27 of 34 tests; `search`,
      `engine_query`, `fuzz_segment_replay`, `fuzz_files_replay`, `lifecycle_mapped`,
      `background_merge_mapped`, and `reclaim_mapped` never ran.
    - Main's last complete ASan run passed in 59 min 57 s (run 39060700799). In the branch's
      run every test is 20% to 30% slower, the untouched ones (`aggregate`, `docvalues`,
      `sortkey`) included, so the runner was slower; the branch did not slow the suite.
    - Run 39066556772 (`e779835`): the three Linux builds and `linux-tsan` (40 min 49 s) pass,
      macOS and FreeBSD fail as above, and ASan was still running at 14:08 PDT.
    - Main at `bd0d857` (run 39067709065): `macos-x86_64`, `macos-arm64`, and
      `freebsd-x86_64` pass with `adb329e`'s fix.
  - Probes written, not compiled (the hold), in `.scratch/review-format13/`:
    `probe_format13.cc` (segment level: F4, F5, F7, F8, F9) and `probe_media_version.cc`
    (index level: F2). Each prints the observed outcome beside the design's text and exits 1
    on a contradiction. The outcomes stated below are read from the code at the lines cited.
  - Not run: no build, test, sanitizer, fuzzing, or benchmark; `quiet check` exits 1 (the
    gate VM's `night_gate.sh n1017` holds the host strictly until about 01:17). No VM,
    bare-metal host, or Xapiand cluster. `poststats` and `keysim` ran on no segment.

## Verdict

**APPROVE WITH CONDITIONS: the decomposition, the kind table, the keys section, and the
keyless rules are right; F2's index-level handling, F5's tail, F7's placement, and F8's
routing are not; the freeze criteria and three gate rows cannot decide what they claim.
Revision 1 answers F13-R1 to F13-R5 and F13-R11 before format-13 code returns to the
reader, writer, index, or merge paths (layers' step 7), and F13-R7 and F13-R8 before F10's
code.**

What holds, and reproduces:

- One version for every pending change, frozen once, with N − 1 readers from format 13 on
  (F1, F2), as PLAN.md:16-18 and R20 require.
- Stable kind ids, with format 12's `pk_filter` (11) retired rather than reused (F3;
  `format.hh:90-111`). An unknown kind fails at the table (`format.hh:295`), which closes R1's
  `default: break`.
- Structure flags in the footer, checked against the table in both directions (F4;
  `format.hh:325-336`); unknown bits and nonzero reserved words fail (`format.hh:269-270`;
  `reader.hh:76`). `format13_test.cc:96-122` removes each section, flips each of the 32 bits,
  and tries kinds 11, 18, 200, and 65,535: every case fails as corruption.
- The version is read before the footer, so another version's footer need not parse
  (`reader.hh:66-69`), at the segment level.
- `stats` at 8 bytes, and the length statistics as `inverted`'s own (F7), as L9 rules.
- The key leaves the term dictionary without loss: stage 1 already allows one key-prefixed
  term per document (`index.hh:1527-1537`) and now requires it boolean (`index.hh:1533`).
  Xapiand writes its id term that way: the id field is forced boolean (`schema.cc:9157-9159`),
  a boolean field term has weight 0 (`schema.cc:5305`), and a field term's position defaults
  to 0, which adds no position (`schema.cc:2136, 5306-5310`).
- One key per ordinal and per segment, checked by the writer (`keys.hh:64`) and by `check()`
  (`check.hh`, the keys walk); lookups through the range, the filter, then the FST
  (`keys.hh:136-152`); the filter read in place, as P15 requires (`keys.hh:120`;
  `p15-region-size.md:59-68`).
- Version 0 for a keyless document in every index (F9; `index.hh:1650`): one answer under
  every flush timing. M3's rejection of a conditional operation without a key stands
  (`index.hh:1623-1625`).
- `merge_segments` without `key_prefix` (`merge.hh:55-57`): the prefix means nothing once keys
  have their own section, and the merge collects keys with the keys cursor (`merge.hh:69-76`).
- Every open encoding names candidates and a measurement, and the design claims no size or
  speed for format 13 (format-13.md:13-16).

What blocks:

- F13-R1: a format-12 segment in an index fails as damage, and a flipped version byte escapes
  the read retry and `heal`.
- F13-R2: F8's fallback inside the segment breaks layers' reader rule, and wildcard queries on
  the key prefix (Xapiand's index listing) find nothing.
- F13-R3: F4's eight bits do not fit L4's one bit per plugin; three per-kind tables exist;
  bits without decoders pass.
- F13-R4: F5's tail breaks S1's order and holds a first-use section; F7's inner CRC hides two
  fields from the fuzzer; the "one encoding" claim is not enforced.
- F13-R5: the digest changes twice before the freeze.
- F13-R7: the carried rows are not freeze criteria.
- F13-R8: F10 to F13 spend one carried byte budget separately; A1's offsets drop format 12's
  wide path.
- F13-R11: `merge_segments`' positional tail, at the rebase.

## The rebase onto layers (SYS-1)

The lead's decision (index at `bd0d857`, "Queued", item 1) is right, and I rule the same way:
format 13 rebases onto `phase4/layers-refactor` at `c8c03f6` and lands its decisions as
layers' step 7 (LAY:697).

- **Layers' proof is byte identity with format 12:** step 0's fixtures and digest
  (`test/fixtures_test.cc`, six `test/fixtures/*.sx`, `digest.txt`) and the old merge kept as
  an oracle (`test/merge_oracle.hh`). Rebasing layers onto format 13 would trade a pinned
  format for one that F22 re-pins at every change, and would void the oracle.
- **Layers lands first.** It waits for N4 and its A/Bs (LAY:681-686). Format 13 cannot freeze
  before revision 1 clears a full review and its candidates are chosen (F1), even with points
  reserved by the owner's ruling (C6).
- **Format 13's decisions are bytes and codecs.** Layers changes neither `format.hh` nor
  `keys.hh` (its diff touches neither), so both carry over. The 30 hunks are in the reader,
  writer, index, and merge, which step 7 redoes on the plugins.

The four semantic conflicts, ruled (each a finding below):

1. Routing (F8 against L3 and L11): F8 moves to the planner; layers stands (F13-R2).
2. Structure bits (F4 against L4 and L7): F4's bits stand; L4 and L7 change (F13-R3).
3. File order (F5 against S1): S1 stands, with `lengths` out of the tail; F5 and F7 change
   (F13-R4).
4. The digest (F8 and F21 against L8): one change, at step 7 (F13-R5).

Until the rebase, what each branch must not change:

- **Format 13** stays out of `reader.hh`, `writer.hh`, `index.hh`, and `merge.hh` (the lead's
  rule), and also out of `search.hh`, `searcher.hh`, and `fuzz/fuzz_segment.cc`, which layers'
  steps 5 and 6 rewrote or which conflict. It works in `format.hh` and `keys.hh`; in new codec
  headers of free functions (F10's advance and impact entries, F11's block codecs, F13's
  seeds, F15's directory, F17, F18); in new tests of those units; in additive `strata_engine`
  modes; and in the design.
- **Layers** changes no format-12 byte, since that is its proof; edits neither `format.hh` nor
  `keys.hh`; adds no code that depends on the key prefix in merges or in the keys plugin's scan,
  both of which format 13 deletes; and keeps `section_rule` and `built` as they are until step 7
  merges them with `format.hh`'s table (F13-R3).
- **Step 7's commit order** on the rebased branch: F2 to F6 in the core's open and the sink;
  F7; the keys plugin on `keys.hh` with its dependency removed, and the planner's key leaf
  (F13-R2); F9's presence with `holds<P>()` per segment; F21's digest, once (F13-R5); F20 with
  L16's checks (F13-R1).

## Rulings on F1 to F22

| Decision | Ruling | Findings |
|---|---|---|
| F1 Scope and freeze | Sound with changes: the freeze criteria gain the carried targets, the decision rules, and the libraries' forms; the scope names Track P's other format items; item 3 (D15) gives way to the owner's points rule | F13-R7, R12, R14; C6 |
| F2 Version and compatibility | Sound with changes: the index path misclassifies both ways | F13-R1 |
| F3 Section kinds | Sound; one per-kind table | F13-R3 |
| F4 Presence and structure flags | Sound with changes: bit ownership in L4 and L7; a bit without a reader refused as unsupported; the free-and-absent tests | F13-R3 |
| F5 Table of contents and file order | Wrong as written: the tail breaks S1 and holds a first-use section, and "one encoding" is not enforced; the alternative is F13-R4's order and checks | F13-R4 |
| F6 Header and footer | Sound with changes: the first 12 bytes stay fixed in every version; a header self-check in the padding is optional | F13-R1 |
| F7 Statistics | Sound with changes: the 20 bytes move to a pinned section, and the inner CRC goes | F13-R4 |
| F8 Key dictionary | Sound with changes: routing, multi-term queries, the key's bytes, the filter as a candidate, the accounting, and `keysim` | F13-R2, R6, R10 |
| F9 Versions | Sound; the writer enforces it too | F13-R9 |
| F10 Skip entries | Sound with changes: a wide form, a flag field, the validation rule, a joint decision rule, and its expected effect stated from the profile | F13-R7, R8 |
| F11 Postings encoding | Sound with changes: a decision rule within the joint budget | F13-R8 |
| F12 Variable partitions | Sound with changes: no bytes at P0, the default | F13-R8 |
| F13 Threshold seeds | Sound; the bound holds for any score non-decreasing in wdf and non-increasing in length. Its bytes count in the joint budget | F13-R8 |
| F14 Positions | Sound | — |
| F15 Values | Sound with changes: a `binary` column; the column's version byte | F13-R13 |
| F16 Points | Sound as a reservation: kind 15 and bit 32 are reserved (F3, F4). The owner rules that format 13 freezes with points reserved; the rule for adding them later is not stated | F13-R13; C6 |
| F17 Term lists | Sound with changes: a frame bound; whether a document's list holds its key | F13-R13 |
| F18 Index sort | Sound with changes: the component count as LEB128; the section is the core's | F13-R3, R13 |
| F19 Stored frames | Sound: bit 31 already fails the frame bound (`reader.hh:239`) | F13-R12 (P14) |
| F20 Manifest version 2 | Sound with changes: L16's plugin identifiers, and its range check at open | F13-R1, R3 |
| F21 Digest | Sound with changes: one change, at step 7 | F13-R5 |
| F22 Fixtures, fuzz bases, tools | Sound with changes: an hour per decoder, every checksum resealed, `keysim`'s comparison, `poststats`' seed counts | F13-R6, R15 |

## Findings

### What the branch builds on

1. **F13-R1. F2's classification holds for a lone segment, not in an index** (F2, F6; T1).
   - **A format-12 segment fails as damage.** `load_segment` decodes a format-13 footer before
     the segment checks its version (`index.hh:2115`, then `open_segment`). A format-12 file's
     last 56 bytes end in `SEND` but fail the 48-byte checksum, so it throws
     `corrupt_segment("footer checksum")`; `read_checked` retries once and rethrows
     (`index.hh:2068-2085`), and the open fails as damage. F2's "a segment from another binary
     is not damage" (format-13.md:50-53) holds only for `segment(file)` (`reader.hh:66-69`).
   - **A flipped version byte escapes.** The version is trusted before any checksum. A media
     flip in bytes 8 to 11 of a committed segment makes `segment` throw `unsupported_segment`,
     which neither `read_checked` (`index.hh:2070-2073`) nor `heal` (`index.hh:474-490`)
     catches. Format 12 healed a transient flip there by the open's retry, since its mismatch
     was `corrupt_segment`; format 13 fails the open. A sticky flip fails it with a type
     `media_test` forbids (`media_test.cc:115-118, 143-150`). `media_test`'s sweep starts at
     offset 0 with a stride of size / 48 (`media_test.cc:100-101`), so on its segments of a few
     KiB it never lands on bytes 8 to 11. Probe: `probe_media_version.cc`; from the code, all
     8 cases (4 bytes, transient and sticky) give `unsupported_segment`.
   - **No test opens a real format-12 file.** `format13_test.cc:141-148` and
     `segment_test.cc:458-473` patch the version of a format-13 file. "A fixture for every
     format change" (PLAN.md:146) wants the old format's own bytes.
   - Fix:
     1. At the index, L16's rule exactly (LAY:763-768): the manifest's range refuses an index
        this binary cannot read with `unsupported_index` before any segment opens; a segment
        whose header version lies outside the manifest's range is `corrupt_segment`, retried and
        healed like any damage. Until F20 lands, a segment the manifest names that throws
        `unsupported_segment` is corruption, since the manifest names only segments its own
        binary wrote.
     2. One `format::read_version(view)` (magic and version) runs before `decode_footer` in
        `load_segment`, `written_segment` (`index.hh:843`), and `segment`.
     3. F2 states that the header's first 12 bytes, the magic and the version, keep their
        layout in every later version: a reader of any version reads them before it knows the
        version.
     4. Optional, at no disk cost: a CRC-32C of the header's first 32 bytes in bytes 32 to 35,
        inside the padding before the first section (sections start at 64,
        `format.hh:87`), so a lone segment's version is trusted only when it checks. With 1. in
        place this matters only to tools.
     5. Fixtures: main's format-12 `small.sx` kept as `test/fixtures/format12.sx`, refused by
        `segment` as `unsupported_segment`, and an index naming it refused as `unsupported_index`
        after F20, as corruption before it. `media_test`'s sweep includes bytes 8 to 11 of every
        segment.

2. **F13-R2. F8's routing moves to the planner, and multi-term queries on the key prefix read
   `keys`** (SYS-1 conflict 1; SYS-9).
   - **The fallback breaks layers' rule.** A dictionary miss falls back to `keys` inside the
     segment (`reader.hh:356, 364-372`). Under layers, term lookup is
     `inverted_plugin::reader`'s (LAY:115, 620-626), no reader reaches a sibling (LAY:240-244;
     the layers review's R3), and a dependency must come earlier in the layout (LAY:198-200),
     while `keys_plugin` follows `inverted_plugin` (LAY:170). F8 adds the reverse of the one
     dependency format 13 deletes (LAY:815-816).
   - **The equivalence holds for exact lookups only.** "Every query path, `verify`, and the
     statistics see what format 12's key term gave them" (format-13.md:173-174). The
     dictionary walks, `scan_terms`, `term_cursor`, and `scan_dictionary`
     (`reader.hh:384-433`), never see a key. Xapiand lists indexes with a wildcard over its id
     prefix, `OP_WILDCARD` over `QK<path>` (`search_views.cc:2292-2294`); after format 13 the
     expansion is empty. Refusing multi-term queries on the key prefix breaks a host path, so
     they read `keys`.
   - **"No term in the dictionary can equal a key" holds only through stage 1**
     (format-13.md:175-176). The writer drops a document's own key term (`writer.hh:316, 338`)
     but indexes the same string as another document's term; the key then hides behind the
     dictionary entry, since the fallback runs on a miss. Probe: `probe_format13.cc`, "F8"; from
     the code: accepted, `term("Qa")` with df 1 (the other document), `find_key("Qa")` at
     ordinal 0.
   - Fix:
     1. **Route by prefix, not by miss.** In a keyed index every term with the key prefix is a
        key, since stage 1 allows no other (`index.hh:1530-1535`). The planner resolves such a
        term through `keys_plugin::reader` as a key leaf: an iterator over at most one ordinal,
        wdf 0, no positions. It never asks `inverted`. Its statistics come from `keys`: df counts
        the segments whose `keys` hold it, deleted copies included, as format 12's postings did.
        `segment::key_term` and the fallback at `reader.hh:356` are deleted, and
        `keys_plugin::depends_on` is empty (L3).
     2. **Disjoint by construction.** The writer refuses a term with the key prefix in
        `inverted`'s part (`std::invalid_argument`), so the dictionary holds none, and `check()`
        verifies it. The prefix reaches the writer as the `keys` plugin's option (F20).
     3. **Multi-term expansion.** A pattern whose literal prefix starts with the key prefix
        expands over `keys` only; one whose literal prefix is a proper prefix of the key prefix,
        the empty one included, expands over both and merges in byte order, the sets being
        disjoint by 2.; any other expands over `inverted` only. Over `keys` it runs the ordered
        scan from the literal prefix (L3's `scan`; `keys.hh:188-195`) and tests each key against
        the automaton, so `keys` needs no intersect of its own and any K candidate serves, K3
        included. Expansion limits count both sources, and each expanded key is a key leaf.
     4. **The key's bytes.** F8 states that `keys` holds the host's key exactly as operations
        name it (`o.key`, `index.hh:1598`), which for Xapiand includes the `Q` prefix, and that
        the core never adds or strips a prefix. This freezes, and L3's `document::key` produces
        the same bytes.
     5. Automata's `expand.hh` reads the plugin readers' walks, not `segment` (SYS-9's ruling for
        automata stands).

3. **F13-R3. F4's bits stand; layers' one bit per plugin does not** (SYS-1 conflict 2).
   - F4 records eight structures, `norms` (2) and `index_sort` (128) among them
     (format-13.md:95-97). L4 gives each plugin one `structure` bit (LAY:277), and L7 handles a
     plugin's optional sections with `section_rule::presence` (LAY:289, 476-481); neither
     `norms` nor `index_sort` is a plugin (LAY:169-171).
   - A section that is optional without a bit brings back what R1 removed: its absence cannot be
     told from a loss (phase-4-design.md:104-106). D2 makes norms optional (phase-4.md:76), so
     norms needs its bit.
   - **Three per-kind tables.** `format::rule_of` holds the bit and the rank
     (`format.hh:135-162`); layers' plugins hold the verification class and presence (layers
     `reader.hh:146-149, 244-251, 454-457, 544-545`); and `reader.hh:80-92` hard-codes the
     pinned list. One mechanism, three copies, which can disagree: `lengths` is in no pinned
     list, and `rule_of` places it in the pinned tail (calibration, first row).
   - **Bits without decoders pass.** `known_structures` is 255 (`format.hh:131`), and a
     `values` section under a set bit opens unread, neither pinned nor region-verified
     (`reader.hh:93-110, 132-138`). Probe: `probe_format13.cc`, "F4"; from the code: opens.
   - `holds<P>()` is static at layers' step 6 (layers `reader.hh:769-771`; SYS-16); presence
     needs it per segment.
   - Fix:
     1. **One table in `format.hh`:** per kind, its id, its owner (the core or a plugin id), the
        bit that requires it (0: mandatory), its verification class (pinned, regions, first
        use), and its file rank. The core's open, the sink, `check_structures`, and each
        plugin's `rules` read it; the switch at `reader.hh:80-92` goes.
     2. **L4's `P::structure` becomes a mask:** `inverted` owns `inverted | norms`; `keys`,
        `stored`, `values`, `points`, and `term_lists` own one bit each; `index_sort` is the
        core's, since D14's order is the merge's order source, which the core calls
        (LAY:385-392). L7's presence becomes "the bit that requires the section".
     3. A set bit whose plugin this binary lacks is never read past and never called
        corruption: a lone segment throws `unsupported_segment` (L7, LAY:464-467), and an index
        whose manifest lists that plugin is refused with `unsupported_index` (L16). A set bit
        whose plugin the manifest does not list is corruption, since the manifest vouches for
        what its segments hold. This is the path by which `points` arrives after the freeze
        (C6), and it covers `values`, `term_lists`, and `index_sort` on the branch until their
        readers land.
     4. `holds<P>()` reads the segment's bits, and a plugin whose bits are clear opens empty
        (L7's "no such structure").
     5. T1 adds the free-and-absent cases F4 promises and no test covers: `stored` absent with
        its bit clear (opens, empty records); `norms` absent with its bit clear; `inverted`
        absent with its bit clear, which today fails because the reader requires `lengths`,
        `norms`, and the dictionary unconditionally (`reader.hh:118-127, 151, 157-158`); and a
        segment of the `document` profile.

4. **F13-R4. The tail holds only pinned sections, in S1's order; the length statistics move to
   a pinned section; the layout is canonical** (SYS-1 conflict 3).
   - **The order.** F5's tail is `docids`, `stats`, `lengths`, `norms`, `versions`,
     `term_index`, `key_index`, `index_sort` (format-13.md:127-128; `format.hh:151-158`;
     `writer.hh:529-539`). S1 puts the core's sections first, then each plugin's pinned sections
     in layout order (LAY:41, 182): `term_index` (inverted) before `versions` (keys). F5 gives
     no reason for the difference.
   - **A hole in the run.** `lengths` is first use (format-13.md:72), not pinned. An open reads
     its 24-byte header and the width byte (`reader.hh:118-126`), skips the body, then reads
     `norms` on. At 5M documents the body is 10.1 MB (`phase-3-ratchets.md:191`), so the open
     reads two runs, not the "one contiguous tail run" F5 and S1 claim (format-13.md:128-129).
     T7 as written passes anyway: every touched page lies inside the tail.
   - **A second checksum, and a fuzz blind spot.** F7's CRC over the 20 statistics bytes
     (`writer.hh:511`; `reader.hh:119-121`) covers bytes the table's section CRC already covers.
     `refresh_checksums` reseals the section, region, table, and footer CRCs
     (`fuzz_segment.cc:79-89`), not this one. Under recomputed checksums no input reaches the
     total length or the positions bound, which `positions_reader` uses to bound a document's
     count (`reader.hh:1110, 1155`). Format 12 kept both in `stats`, which the fuzzer resealed.
     Probe: `probe_format13.cc`, "F7"; from the code: `corrupt_segment (lengths header
     checksum)`.
   - **"One segment has one encoding" is not enforced** (format-13.md:122-123). `decode_toc`
     checks rank order and overlap only (`format.hh:312-318`), so gaps of any size and
     unaligned offsets open, and gap bytes are under no checksum. Probe: `probe_format13.cc`,
     "F5" (a 64-byte gap; packed, unaligned sections); from the code: both open.
   - Fix:
     1. The tail: the core's pinned sections (`docids`, `stats`, and `index_sort`, F13-R3), then
        `inverted`'s (`norms`, `term_index`), then `keys`' (`versions`, `key_index`), then each
        later plugin's, in layout order: S1 as written, encoded once in F13-R3's table.
     2. `lengths` leaves the tail and goes last among the other sections, before `docids`.
     3. The 20 statistics bytes move to the head of `term_index`, which is pinned, always present
        with `inverted`, checked whole at open, and copied out; the inner CRC goes. An open then
        reads nothing of `lengths`, and resealing covers the fields. L9 names `lengths`' header
        (LAY:544-546); `term_index` is `inverted`'s too, so L9's rule holds and its text changes.
        If the bytes stay in `lengths` instead, they become its trailer, `lengths` sits last
        before the tail, and `refresh_checksums` reseals the inner CRC.
     4. `decode_toc` requires the header entry at offset 0, every later section at the previous
        one's end rounded up to 64 bytes, and the table likewise: arithmetic on the table, no
        extra read. F5 says that padding content stays unchecked.
     5. T7 asserts that an open touches the header page and one run of pages ending at the footer.

5. **F13-R5. The digest changes once, at step 7** (SYS-1 conflict 4).
   - The branch has changed the digest once already: F9 sets a keyless document's version to 0
     (`index.hh:1650`), and `D` carries the version. F8 keeps a `P` record per live key to hold
     the value for keyed documents (`index.hh:569-574`; format-13.md:177-178), and F21 changes it
     again when it lands. L8 changes it once (LAY:522-523). F8's "its value does not change
     until F21's split" holds only where every document has a key.
   - Fix: the interim `P` record per key goes at the rebase. One commit changes the digest: L8's
     split, the key in `D`, `inverted`'s length record for every document (the layers review's
     revision check 1, finding 1), and F9's version 0, with layers' pinned `digest.txt` re-pinned
     once, citing F21, and T2's schedules run in that commit.

6. **F13-R6. The keys sections: the filter is a candidate, the accounting includes it, and
   `keysim` compares unlike things** (F3, F8; T3, T5).
   - **Accounting.** `key_index` holds the fuse filter (`keys.hh:86-88`), 5.7 MB at 5M keys
     (`phase-3-ratchets.md:190`), and F8 counts `key_index` in the dictionary row
     (format-13.md:216-219). The projection the row was carried on leaves the filter out: about
     61 MB, 0.90× (`phase-3-ratchets.md:242-246`). With the filter and the reordered ordinals F8
     cites (14.5 MB, `reordering.md:306-307`), the same projection is 39.9 + 10.6 + 14.5 + 5.7 =
     70.7 MB, before the key blocks' index.
   - **The reference.** PLAN's reference is the best figure in the set, the smallest for disk
     (PLAN.md:63-65), and the set holds Lucene with and without reordering (PLAN.md:59-60;
     BENCHMARKS.md:60-62). For the dictionary row the smaller keyed figure is Lucene's without
     reordering, 67.9 MB, not `-bp`'s 75.8 MB (`phase-3-ratchets.md:187, 195-196`); keyed Tantivy
     is unmeasured. PLAN's carried text uses 1.36× of keyed Lucene (PLAN.md:78-79), while the
     gate file and T3 use 1.20× of keyed `-bp` (`phase-3-ratchets.md:206, 211`;
     format-13.md:464). The projection is 1.04× of the first and 0.93× of the second: the
     reference decides the row (Q1). Every other section has the same reference under either
     reading.
   - **The filter.** Lucene's counterpart has none. The filter pays only for misses across many
     segments, which T5 measures, and its 5.7 MB sits in a carried row. Keeping it is a
     measurement, not a given.
   - **`keysim`** (`strata_engine.cc:350-446`):
     1. K1's bytes include the filter and the key range (`:365`); K2's and K3's do not (`:379`,
        `:400`), so both read 5.7 MB smaller at 5M than they would be.
     2. K1 is timed through `find_key`: range, filter, mapped blocks, region verification. K3 is
        timed with no filter, from heap strings, with no verification (`:438-444`).
     3. Every timed lookup is a hit (`:424-431`). T5's workload at 50 segments is mostly misses.
     4. K2 is not timed and K4 is not built, though F8 says `keysim` measures each candidate's
        size and lookup cost (format-13.md:213-214). K2's lookup needs a key's position within
        its block; the design says whether `termdict::reader::find` returns it or changes to.
   - F8 has no rule for choosing among candidates that pass.
   - Fix:
     1. Candidates cross the codec with the filter: K1 to K4, each with the filter and with the
        range alone; each sized with the same fields, timed through the same reader path (mapped,
        region-verified), on hits and on misses (keys drawn from another segment), at 1 and 50
        segments through `lookup_key`.
     2. Rule: the smallest candidate whose lookups meet T5; within 1% of size, the faster.
     3. The projection is restated with the filter, and T3's dictionary target names its
        reference by Q1's answer.
     4. BENCHMARKS' counterpart table changes with F8 (BENCHMARKS.md:170-173): `keys` and
        `key_index` join the dictionary row, against `.tim`, `.tip`, and `.tmd`, which hold
        Lucene's id terms; `docids` and `versions` keep the id field's doc values, which the keyed
        builds carry as versions (BENCHMARKS.md:130-131). Phase 4's "`keys` against the id
        field's terms" (phase-4.md:498-499) follows.
     5. F3 and F8 list `key_index`'s key count (`keys.hh:27-29, 80`), which the design's layout
        omits (format-13.md:184-185).

7. **F13-R7. The carried rows are freeze criteria** (SYS-7).
   - SYS-7 is right. The format freezes once (F1), the carried disk rows close in format 13
     (PLAN.md:78-85), and Phase 4's gate does not pass while a carried row misses
     (PLAN.md:75-76). T4's target, format 13 over format 12 at ≤ 1.03 (format-13.md:465), is a
     guard against regression. T3 and T4 run on main after landing (format-13.md:454-458), when a
     miss costs format 14 and its N − 1 reader (format-13.md:56-58).
   - **F10's expected effect is smaller than "Why now" says.** The profile it cites
     (`query-gap-merged.md:24-28`) splits the worst count query's time as postings fill 22%,
     `decode_group` 14%, conjunction fill 10%, `block_index_for` 6%, advance 6%, `or_bitset` 5%,
     `load_bitset` 5%, `check_increasing` 4%, `skip_sized_impacts` 3%. Skip-entry work is
     `decode_group`, `block_index_for`, and `skip_sized_impacts`: about 23%. Removing all of it
     takes the roughly 300 ns per touched block to about 230 ns, against Lucene's roughly 90 ns.
     "Spent in level-0 decoding and validation" (format-13.md:27-29) and the record's "the
     remaining gap is per-block skip-entry decoding and validation" both overstate.
   - Fix: F1's freeze criteria gain:
     1. T3's postings, dictionary, and whole-index rows at ≤ 1.0× their references (F13-R6, Q1)
        on the freeze candidate after a reordered full merge.
     2. The after-merging query rows at ≤ 1.0× by the interval's upper bound; or, per class that
        misses, a profile that attributes the remaining time, every part of it to code outside the
        format, each part assigned to a named Track P item with its projected effect and closed
        before Phase 4's gate. No part of a miss is attributed to the format at the freeze.
     3. T4's ≤ 1.03 against format 12 on every class, as the guard.
     4. The evidence is the owner's SYS-6 kind: branch A/Bs of the freeze candidate against
        main's format 12 under a hold the lead grants, and the section sizes of a branch build's
        reordered full merge, which are exact, recorded in `docs/perf/` as candidate tables and
        never as gate rows. A carried query row is projected as the class's A/B ratio times
        format 12's recorded ratio for that class (`phase-3-ratchets.md`, "Queries after
        merging", re-recorded at the Phase 3 gate's commit), over the full gate query set, not
        the quick sample. The gate rows then run on main at landing and confirm it; since a miss
        there costs format 14, the projection states its interval.

8. **F13-R8. F10 to F13 spend one carried byte budget; the advance entry's widths and flags are
   not load-bearing as written** (F10, F11, F12, F13).
   - **One budget, four separate rules.** The postings are 11.9 MB over keyed `-bp`'s `.doc` at
     5M (845.6 against 833.7 MB, `phase-3-ratchets.md:204`). F10's fixed entries
     (format-13.md:262-266), F12's per-partition lasts (308-309), F13's seeds (339-341), and
     E1 to E5's flags all add bytes to that section, and each is chosen alone: F10's "a
     candidate that passes one by failing the other is not chosen" (497-499), E2 and E3 by T4
     only (296-297), F12's "kept only if it wins" (313), F13 by T6 (342-343). Nothing says what
     happens when no combination meets both T3 and T4.
   - **A1's offsets.** "Offset u32 (bit 0 the bitset flag)" (format-13.md:264) leaves 31 bits:
     2 GiB of blocks per term. Format 12 writes 64-bit offsets when a term needs them
     (`writer.hh:818-829`). A segment holds up to 2^32 − 2 documents (limits.md:16); a term in all
     of them has 2^25 blocks, and at 16 bytes of bitset and a 128-byte block of 8-bit wdfs each,
     that is about 4.8 GB. E1 and E2 take two or three more bits from the same word. F22's
     fixture "with 64-bit skip offsets" (format-13.md:440) implies the wide path stays.
   - **F12's hook at P0.** "Each impact entry carries its own last ordinal" (format-13.md:308)
     repeats the advance entry's last when partitions are blocks, the default, so every segment
     pays for a candidate that may lose.
   - **F10's validation.** "Per entry read (lasts increase, offsets increase ...)"
     (format-13.md:249-250) cannot check order in a binary search, which reads no neighbor. What
     can hold is per block decoded: its ordinals in (previous last, last], its bytes inside the
     term's area.
   - Fix:
     1. One selection over the combination. The postings row at ≤ 1.0× is the constraint; ns per
        touched block on the `COUNT` and pruned top-k rows is the objective; each kept item shows
        its bytes and time against the combination without it. If no combination meets the
        constraint and F13-R7 2., the candidate report says so and the owner decides before the
        freeze.
     2. Advance entries keep a wide form, per term as format 12's `wide`, or per group as a 64-bit
        base with A2's narrow fields. The flags (bitset, E1, a 2-bit codec for E2 and E3) get a
        field of their own, not the offset's low bits.
     3. F12's lasts appear only in terms whose partitions differ from blocks, by a per-term flag.
     4. F10 states its validation per block decoded, and what an advance over a corrupt entry may
        return: a wrong block, then `corrupt_segment` when that block's ordinals disagree with its
        entry, which `fuzz_segment` checks.
     5. Losing candidates leave the format before the freeze, decoders and hooks with them. Only
        choices the writer makes per block by size alone, the codec field, stay.

9. **F13-R9. Version 0 for keyless documents: sound; the writer enforces it too** (F9).
   - The rule removes a real nondeterminism: with no `versions` in a keyless segment, a
     docid-addressed replace would read `current + 1` from the buffer and 0 after a flush
     (format-13.md:227-231).
   - The writer refuses a version only in keyless segments (`writer.hh:571`), so a keyed segment
     accepts a keyless document with version 3 and returns it (`reader.hh:178-188`). Probe:
     `probe_format13.cc`, "F9"; from the code: accepted, version 3. Layers' step-0 fixture writes
     exactly that, `1 + d % 3` for keyless documents (layers `test/fixtures_test.cc`).
   - Fix: `begin_document` refuses a nonzero version without a key in every segment, `check()`
     verifies it, and the fixtures that write one are re-pinned in F13-R5's commit.

10. **F13-R10. The boolean key term: sound, with a writer that refuses where it now drops** (F8).
    - Rejecting a key term with wdf or positions (`index.hh:1533`) is what lets the key leave the
      dictionary without loss, and Xapiand's id term is boolean with no position by default
      ("What holds"). Stage 1 also allows one key-prefixed term per document, so main already
      rejects any second `Q`-leading term.
    - The writer drops a document's key term whatever its wdf and positions
      (`writer.hh:316, 338`), silently, below stage 1.
    - Fix: the writer refuses a key term with wdf or positions (`std::invalid_argument`).
      Condition: the seam inventory confirms that Xapiand writes exactly one `Q`-leading term per
      document, its boolean id term, with no `_position` on the id field; accuracy prefixes come
      from `get_prefix(acc)` (`schema.cc:1603-1611, 4847`; `utils.cc:71-78`), and the inventory
      checks that none of their terms leads with `Q`.

11. **F13-R11. `merge_segments` without `key_prefix`: sound; named options at the rebase.**
    - Removing the parameter is right. Layers' `merge_segments` still takes it, positionally,
      before `region_shift` (layers `merge.hh:367-370`), and every caller passes it that way
      (layers `index.hh:1123`, `tools/parity.cc:225`, and the tests). Today's callers fail to
      compile rather than rebind once it goes; a call that passes `{}` there with nothing after
      it would bind `region_shift` to 0, which only the reader's region-size check catches
      (`format.hh:297`).
    - Fix: at step 7, `merge_segments(sources, id, sink, spill, const merge_options&)`, with the
      region shift, cancellation, memory, order, and records named.

### The rest

12. **F13-R12. Scope: Track P's other format changes are neither in F1 nor reserved.**
    - PLAN bundles every pending format change into format 13 (PLAN.md:80-85). F1 holds F2 to
      F22 "and nothing else" (format-13.md:34) and takes hooks for P3, P4, P13, and P21, but not:
      - P14's dictionary-trained zstd per segment (PLAN.md:391-392): `stored` freezes as format
        12's layout plus one reserved bit (F19).
      - P20's frozen segments: quantized impacts and a static key hash as optional sections that
        "readers that know the sections use" (frozen-index.md:46-49). Under F4 an unknown kind is
        corruption, so frozen-index.md's rule no longer holds.
      - P8's per-block pair summaries (PLAN.md:371-372), which live in skip data.
      - P18's marks for punched stored records (PLAN.md:403-404), if they are segment bytes
        rather than live-docs bytes.
    - Fix: F1 lists every Track P item with an on-disk effect and, for each, a hook reserved in
      format 13 or an explicit "format 14", by the owner's decision (Q2). The owner's points rule
      (C6) serves any item that adds a section of its own: a reserved kind and bit, added later
      as a plugin under L16, with no format 14. That covers P20's quantized impacts and key hash.
      An item that changes an existing section's bytes needs its hook now or format 14: P14's
      dictionary (a length, 0 for none, in `stored`'s trailer), P8 inside the skip data, and P18
      if its marks are segment bytes. frozen-index.md changes with the ruling.

13. **F13-R13. Values, points, term lists, and the index sort: details that freeze.**
    - **`binary` has no column.** F15 uses "`docvalues.hh`'s column codec as built"
      (format-13.md:355-356), whose kinds are `u64`, `i64`, `f64`, and `bytes`
      (`docvalues.hh:63`). F15a declares `binary`, an ordered list with duplicates (D3), which
      StringList sort keys and N7's collapse keys read in stored order. Define its column, or
      drop `binary` from format 13 with the reason.
    - **A second version.** The column carries its own version byte, checked equal to 1
      (`docvalues.hh:109, 269`; format-13.md:368), while S2 rules that section versions follow
      the segment's (LAY:42, 754-761). Drop it, or define it as a check derived from the
      segment's version that is never bumped alone.
    - **Bytes per dimension.** F15a caps them at 8 (format-13.md:365); limits.md says "recorded
      per tree" beside Lucene's 16 (limits.md:36). The mechanism is D3a's 64-bit typed values;
      limits.md's row states it.
    - **Term-list frames.** F17 bounds no frame: a document's list with positions can exceed
      `stored_block_size`, and nothing bounds what reading one frame allocates (R19, C9). F17
      reuses `max_stored_frame` (`format.hh:86`) and rejects a larger list at stage 1. It also
      states whether a document's list holds its key, since Xapian's term list holds the id term
      and Xapiand's info endpoint shows it (FEASIBILITY.md:111).
    - **The index sort's count.** F18's component count is a `u8` (format-13.md:398), and
      limits.md says "no count limit" for sort components (limits.md:40). Use LEB128.
    - **Points.** The owner rules that format 13 freezes with a reserved kind for points and a
      stated rule for adding them later (SYS-4). The kind (15) and the bit (32) are reserved
      (format-13.md:82, 96), and F15a's declaration already carries a slot's points flag and
      dimensions (format-13.md:363-366). The rule is missing: F1 still waits for D15
      (format-13.md:39-40), and F16 says the envelope and the body "both freeze with D15's
      result" (format-13.md:382-384). C6 states what the rule holds.

14. **F13-R14. Libraries whose bytes freeze with format 13.**
    - Format 13's bytes include `termdict`'s blocks and index, `fst`'s automaton, `bitpack`'s FOR
      and PFOR blocks, `fusefilter`'s `filter8` form, `docset`'s frozen form (F15), `varint`'s
      LEB128, zstd frames through `compressors`, and `hashes`' CRC-32C and xxHash64 at seed 0,
      each pinned by commit (`CMakeLists.txt:10-28`). Nothing states that a pin bump that changes
      their bytes is a format change. Detent links `varint` and `compressors` (Detent's
      `docs/LIBRARY_BOUNDARIES.md:48, 130`), whose forms are standard; the others are strata's
      alone.
    - Fix: F22 adds the rule and its check. Each library's README states its on-disk form frozen
      at a named version, and a pin bump keeps every format-13 fixture byte-identical or is
      format 14.

15. **F13-R15. Gate rows and tools: three rows cannot decide what they claim.**
    1. **T1** adds F13-R1's index-level and real format-12 cases, F13-R3's free-and-absent
       cases, and F13-R4's canonical-layout cases.
    2. **T2.** `fuzz_segment` is one target with six bases, so "one CPU-hour per base"
       (format-13.md:463) spreads each hour across about ten decoders, where PLAN wants one per
       decoder (PLAN.md:143-145) and F22 promises one each (format-13.md:445-446). A section
       selector, an input byte naming the kind its mutations stay inside, gives each decoder a
       run of its own. `refresh_checksums` reseals every checksum the reader checks, inner ones
       included (F13-R4).
    3. **T3** names its reference per row (F13-R6, Q1).
    4. **T4** stays as the guard (F13-R7).
    5. **T5 names no measurement.** Phase 3's T4 has the key filters' cost per insert as a
       statement in its gate report (phase-3.md:486-489), still owed
       (`phase-3-ratchets.md:357`). The reference is mine: R6's check (phase-4-design.md:349-350)
       names that row. Fix: record format 12's `lookup_key` time on main now, hits and misses at 1
       and 50 segments, with a named tool (for example `strata_engine keybench`), before format
       13 replaces format 12 there. T5's target is ≤ that by the interval's upper bound, with
       Lucene's id lookup reported beside it from Phase 2's `seekExact` harness
       (`bench/phase2/lucene_seek`).
    6. **T7** gets F13-R4's contiguity assertion.
    7. **T11 has no target and no host.** BENCHMARKS runs cold start on bare metal as a report
       row (BENCHMARKS.md:29-32, 114), and the bare-metal machine is retired (SYS-4). T11 becomes
       a report row of the bare-metal session; T7 is S1's functional check.
    8. **`poststats`' seed counts.** Its counts at df ≥ 10 and ≥ 100 equal its count of terms
       with skip blocks, since only terms with df ≥ 128 reach them (`strata_engine.cc:263-269`).
       It reports instead the bytes F13's pairs would add, and A2's and A3's sizes from the lasts
       and sizes it already walks, so F10's candidates are projected before they are built.

16. **F13-R16. The ledger has no done criteria, and the sanitizer evidence is incomplete.**
    - The ledger has no criteria (format13.md:1-10); the pipeline sets them before building, and
      the preliminary review checks them. Proposed for the lead:
      - K1: T1's cases, F13-R1, R3, and R4's included, pass on all eight CI jobs.
      - K2: every decoder F3 adds has a fuzz entry point and a checked-in corpus, replayed in CI
        under ASan and UBSan.
      - K3: F22's fixtures and `format12.sx` pinned, byte-identical on all eight jobs (T8).
      - K4: T9's lifecycle tests over format 13, keyed and keyless, the `document` profile
        included.
      - K5: the full suite completes under ASan and UBSan, and under TSan.
      - K6: `media_test` covers the header's version bytes.
      - K7: each decision's tests cite its F number in the commit that adds them.
    - At `e2ac3fe` the ASan job stopped at its 60-minute limit with seven tests unrun,
      `fuzz_segment_replay` and `search` among them (run 39060862198), and main's last full run
      took 59 min 57 s (run 39060700799). The ledger reports the clang job only
      (format13.md:8, 10). The job needs splitting or a longer limit on main before a criterion
      cites it.

17. **F13-R17. Text.**
    - format-13.md:27-29: the profile does not put the per-block cost "in level-0 decoding and
      validation" (F13-R7).
    - format-13.md:173-174: "every query path" holds for exact lookups (F13-R2).
    - format-13.md:177-178: the digest has changed for keyless documents already (F13-R5).
    - format-13.md:417-418: F20's "structure identifiers" are L16's plugin identifiers.
    - format-13.md:124, 130: the alignment is `format.hh:87` and the section bound
      `format.hh:289` at `e779835`, not `:79` and `:217`.

## Gate rows

| Row | Target | Procedure | Ruling |
|---|---|---|---|
| T1 Format rules | Yes | Yes | Add F13-R1, R3, and R4's cases |
| T2 Decoders | Yes | Not per decoder; one inner checksum unresealed | F13-R15 2. |
| T3 Disk | The dictionary row's reference is ambiguous | Yes | F13-R6, Q1 |
| T4 Queries | A guard, not the carried target | Yes | F13-R7 |
| T5 Keys | Names no measurement | No tool | F13-R15 5. |
| T6 Seeds | Yes | Yes | Its bytes in F13-R8's budget |
| T7 Open | Too weak | Yes | F13-R4 5. |
| T8 Determinism | Yes | Yes, once all eight jobs are green | — |
| T9 Lifecycle | Yes | Yes | The `document` profile after F13-R3 4. |
| T10 Memory | Yes | Yes | — |
| T11 Cold start | None | No host | A report row (F13-R15 7.) |

## Behavior changes in the ledger

| Change | Where | Ruling |
|---|---|---|
| Format 12 refused as `unsupported_segment` | `reader.hh:66-69` | Acceptable for a lone segment; in an index it surfaces as damage (F13-R1) |
| A new exception, `unsupported_segment` | `format.hh:53-57` | Acceptable; the index and `heal` must classify it (F13-R1) |
| Exact key terms read `keys` | `reader.hh:356, 364-372` | Acceptable in effect; the routing moves to the planner (F13-R2) |
| Dictionary walks no longer return keys | `reader.hh:384-433` | Acceptable for merges, `check`, and the digest; not for multi-term queries (F13-R2 3.) |
| `check_report.terms` no longer counts keys; `keys` does | `check.hh`; `index_test.cc` | Acceptable |
| A key term with wdf or positions is invalid at stage 1 | `index.hh:1533` | Acceptable (F13-R10) |
| A key repeated within a segment throws at the writer | `keys.hh:64` | Acceptable: unreachable through the index |
| A keyless document's version is 0 in every index | `index.hh:1650` | Acceptable (F13-R9) |
| Keyless indexes write keyless segments, without `keys`, `key_index`, or `versions` | `index.hh:1698`; `writer.hh:526-539` | Acceptable |
| A merge's output is keyed when any input is | `merge.hh:157` | Acceptable: an index's key prefix is fixed at creation and checked at open (`index.hh:231, 2023`) |
| `merge_segments` loses `key_prefix` | `merge.hh:55-57` | Acceptable (F13-R11) |
| The writer drops a term equal to the document's key | `writer.hh:316, 338` | Not acceptable while silent: refuse (F13-R2 2., F13-R10) |

## Owner questions

1. **Q1. The dictionary row's reference.** PLAN's text makes it the smallest keyed figure in the
   set, Lucene's without reordering at 67.9 MB today; the gate file and T3 use keyed `-bp`'s
   75.8 MB. On the design's own projection the row misses the first by about 4% and passes the
   second by about 7% (F13-R6). Recommended: PLAN as written, with keyed Tantivy measured, since
   per-section rows exist to catch a section that one configuration flatters.
2. **Q2. Track P's format items** (F13-R12): a hook in format 13 or format 14, per item.
   Recommended: reserve P14's dictionary length and P20's kinds and bits now, which cost a few
   bytes; send P8 and P18 to format 14 with their N − 1 cost stated.
3. **Q3. Freezing with points unwritten** (SYS-4). Answered by the owner, relayed by the lead on
   2026-10-17: format 13 freezes with a reserved kind for points and a stated rule for adding
   them later. The design reserves the kind and lacks the rule (F13-R13; C6).
4. **Q4. Candidate runs on branch builds** (SYS-6). Answered by the owner, relayed the same day:
   branch A/Bs under a granted hold are evidence for choosing an encoding, never a gate row.
   F13-R7 4. uses exactly that, and the lead records it in PLAN (C3).

## Required changes

Revision 1 of the design, each item checked by a re-check appended here:

- **Before format-13 code returns to `reader.hh`, `writer.hh`, `index.hh`, or `merge.hh`**
  (layers' step 7): F13-R1, F13-R2, F13-R3, F13-R4, F13-R5, and F13-R11.
- **Before F10's code:** F13-R7 and F13-R8.
- **Before the freeze:** F13-R6 with Q1, F13-R9, F13-R10, F13-R12 with Q2, F13-R13 with C6's
  rule, F13-R14, F13-R15, and F13-R16, then a full design review of revision 1 with its code.

## Conditions

1. **C1.** The lead writes done criteria into the ledger (K1 to K7 or its own list) before the
   next format-13 commit.
2. **C2.** All eight CI jobs, ASan and UBSan included, are green at every commit a criterion
   cites (calibration, second row).
3. **C3.** Every candidate comparison is a `docs/perf/` table with its raw files, interleaved,
   run under a hold the lead grants, and never cited as a gate row (the owner's SYS-6 ruling),
   which the lead writes into PLAN's "What waits" (PLAN.md:16-18).
4. **C4.** Both branches keep "The rebase onto layers"' constraints until step 7.
5. **C5.** A calibration row, for the lead: my R6 check (phase-4-design.md:349-350) named a row
   that existed only as a statement, and T5 inherited it. The check that catches it: every
   "≤ row X" target cites a recorded number and the tool that produced it.
6. **C6. Points' reserved kind and its rule** (the owner's SYS-4 ruling). The design reserves
   kind 15 and bit 32; revision 1 replaces F1's item 3 and F16's "both freeze with D15's result"
   with the rule for adding points within format 13, which states:
   1. Kind 15 and bit 32 stay reserved for points and are never reused.
   2. The first release that writes `points` fixes its layout, envelope and tree body, for
      format 13; changing it afterwards is format 14 (S2: one version per segment).
   3. A binary writes points only in an index whose manifest lists the `points` plugin, added
      when the index first declares a point slot (F15a already carries the flag and the
      dimensions). This refines L16, whose manifest lists every plugin of the writing binary
      (LAY:748-751): an index that never declares a point slot stays readable by a binary
      without the plugin, which rolling upgrades (Track D8) need.
   4. A binary without the plugin refuses such an index with `unsupported_index` and such a
      segment with `unsupported_segment`, never as corruption and never by skipping the section
      (F13-R3 3.).
   5. Under mixed binaries (Track D8), an index starts writing points only once every replica's
      binary has the plugin.
   6. Points add no digest record (they are built from values, which `V` covers, D17), and the
      release that adds them brings their fixture, a fuzz hour for their decoder, and their
      Phase 4 rows.
   The freeze then waits for no D15 result. D15 still decides the dimension counts and the shape
   path before points' first write.

## Which work starts when

| Work | Starts | Waits for |
|---|---|---|
| Codec units as free functions with unit tests: K2 to K4 in `keys.hh`, F10's entries, F11's codecs, F13's seeds, F15's directory, F17, F18 | Now | — |
| `keysim` and `poststats` fixes (F13-R6, F13-R15 8.) | Now | — |
| Format 12's key-lookup row on main (T5's baseline) | When the host is free | — |
| Revision 1 of the design | Now | — |
| Step 7 on the rebased branch | After layers' steps 0 to 6 pass CI | F13-R1 to R5 and R11 in revision 1 |
| F10 and F11 in the postings path | After step 7's F2 to F8 commits | F13-R7 and F13-R8 in revision 1 |
| Candidate A/Bs on the gate VM, as evidence only (C3) | Under a hold the lead grants | Codec units, the tools' fixes |
| The freeze | — | Every item under "Before the freeze", C6's rule in the design, and the full review |
| Points' first write | After the freeze, by C6's rule | D15's dimension counts and shape path; `bkd`'s on-disk form |

## Verdict and required changes, in one line each

- Verdict: APPROVE WITH CONDITIONS; revision 1 before step 7 and before F10.
- F13-R1: classify format 12 and version damage at the index by L16's manifest range; read the
  version before the footer; keep a real format-12 fixture.
- F13-R2: a key leaf chosen by prefix in the planner; no fallback in the segment; the writer
  keeps keys out of `inverted`; wildcard and prefix expansion over `keys`.
- F13-R3: F4's bits stand; one per-kind table in `format.hh`; L4's mask; a bit without a reader
  refused as unsupported; `holds<P>()` per segment.
- F13-R4: the tail in S1's order with pinned sections only; the length statistics in
  `term_index`'s head; a canonical layout; T7 checks one run.
- F13-R5: one digest change, at step 7.
- F13-R6: the filter as a candidate, the projection with it, a fair `keysim`, a decision rule,
  and the reference by Q1.
- F13-R7: the carried rows as freeze criteria; F10's share stated from its profile.
- F13-R8: one joint selection over F10 to F13; a wide form; a flag field; no bytes at P0;
  validation per block.
- F13-R9: the writer refuses a version without a key.
- F13-R10: the writer refuses a weighted key term; the seam inventory confirms one `Q` term.
- F13-R11: named merge options at the rebase.
- F13-R12: Track P's format items reserved or sent to format 14 (Q2).
- F13-R13: `binary` columns, one version scheme, the limits rows, term-list frames, a LEB128
  sort count; points by C6's rule.
- F13-R14: the libraries' on-disk forms frozen with the format.
- F13-R15: T1, T2, T3, T5, T7, and T11 restated; `poststats`' counts.
- F13-R16: done criteria; an ASan job that completes.
- F13-R17: text.
- C6: the reserved points kind is there; the rule for adding points within format 13 is not.

Not run: no build, test, sanitizer, fuzzing, or benchmark (the host's strict hold until about
01:17), so both probes are written and not compiled, and their outcomes are read from the code;
no VM, bare-metal host, or Xapiand cluster; `poststats` and `keysim` on no segment; run
39066556772's ASan job, unfinished when this was written; layers' code
beyond the reader, the merge signature, and the fixture test; Lucene's and Tantivy's source;
no Xapian or Xapiand GPL source.

## Re-check

- Date: 2026-10-17.
- Reviewed:
  - `git log 4266374..phase4/format13`: `41730a3` (design revision 1, `docs/design/format-13.md`,
    1,145 lines) and `abd5741` (the ledger). Both change documentation only.
  - Layers' revision 3 (`layers.md` at `a874ab0`) at `origin/phase4/layers-refactor` `707066b`,
    revision 1's base, and the branch's tip `1b6a148`, which adds only the layers revision-3
    re-check (`docs/reviews/layers-design.md:417-878`) on the same code tree.
  - The owner's decisions in PLAN at main `ec13933` (PLAN.md:16-20, 31-54).
  - What revision 1 folds in from two live documents: the automata review at `phase5/automata`
    `9b9e151` (AUT-R3, AUT-R4, AUT-R13, its C4 and C5) and the query-gap design at
    `design/query-gap` `8a6503f` (M16 to M23; "What the query path needs from format 13").
- Checked against: `calibration.md` at main, all five rows (a mechanism built twice: F13-R19; CI
  on every platform for the reviewed commit: below; a cited row without a raw file: C9; rulings
  earlier reviews address to this design: below; `HEAD` against `origin` for every branch read:
  below); this review's F13-R1 to F13-R17 and C1 to C6; the layers re-check's R13 to R16 and C7
  where they touch format 13.
- Checked by:
  - Reading revision 1 whole, and the code it cites at `707066b`: `core_segment`'s constructor
    (`reader.hh:99-106`), `load_segment` and `written_segment` (`index.hh:2077-2094, 835-838`),
    `section_rule` (`section_io.hh:220-226`), `inverted`'s open (`reader.hh:260-274`),
    `holds<P>()` (`reader.hh:766-792`), `merge_options` (`merge.hh:226-233`), and the file
    family's reader (`files.hh:30, 110-120`: CRC first, version second).
  - Recomputing F2's prefix CRC-32C with a bitwise implementation checked against the standard
    check value (`0xE3069283` for "123456789"): `0x4ff2df7b` for version 13, as revision 1
    states, and `0x92b775c3` for version 12.
  - CI through `gh`: run 39069758885 at `4266374` (the code of `e779835`): the three Linux builds,
    `linux-tsan`, and `linux-asan-ubsan` (49 min 15 s) pass; `macos-x86_64`, `macos-arm64`, and
    `freebsd-x86_64` fail on main's `std::jthread`, as before. Run 39070316303 at layers'
    `1d60098`: all eight jobs pass. Run 39076417209 at `abd5741` was running.
  - `HEAD` against `origin` for every strata worktree: none is ahead. Layers' worktree is at
    `1b6a148`, newer than revision 1's base, documentation only; the automata worktree holds
    uncommitted edits, not commits.
  - Earlier rulings addressed to format 13: the Phase 4 review's R1, R4, R6, R7, R19, R20, and
    finding 4; the layers review's R6, R7, R9, S1, S2; the reordering review's 14.5 MB
    projection; the Phase 1 review's tail question (AUT-R13). Each is answered in revision 1.
    One ruling is reopened by a candidate: M18 reverses the Phase 2 gate's whole frontiers at
    block level (`phase-2-gate.md:84-92`), which the query-gap design leaves to the architect
    (`query-gap.md:434-437`); that ruling belongs to the query-gap review.
  - The merge probe was not rerun: no code changed since `e779835`.
  - Not run: no build, test, sanitizer, fuzzing, or benchmark (`quiet check` exits 1: the gate
    VM's `night_gate.sh n1017` holds the host strictly until about 01:17); no VM, so the keyed
    `-bp` sizes file revision 1 cites was not read (C9).

### Verdict

**APPROVE WITH CONDITIONS: revision 1 closes F13-R1 to F13-R8, F13-R10 to F13-R12, F13-R14,
F13-R15, F13-R17, and C6 in the design. Both proposals to layers are accepted. The rebase plan
is sound except one ordering defect: 7c-4 writes keyless segments before 7c-5's version rule
(F13-R18). Open: the owner's Q5 and Q6 (F13-R23, F13-R22), the layers re-check's format-13
items (F13-R25), four new findings (F13-R19 to F13-R21, F13-R24), and text (F13-R26).** Step 7c
may start on the rebased branch, once layers lands or
the lead says so, with F13-R18 fixed in the plan and each F13-R25 item in the design before the
commit it names. F10's code may follow 7c's F2 to F8 commits. The first steps may be drafted,
except K3's codec unit (F13-R19) and K4's `termdict` change (F13-R20).

### Status

- **F13-R1: closed in the design.** The 16-byte prefix (format-13.md:165-182, 347-359) gives
  four rules. A real format-12 file has zeros at bytes 12 to 15, so rule 3 refuses it as
  unsupported. A flip from 13 to 12 leaves `0x4ff2df7b` there, so rule 4 calls it damage.
  `read_version` runs before `decode_footer` in all three opens (format-13.md:180-182). At the
  index, the manifest's range and mask classify, and before F20 a manifest-named unsupported
  segment is corruption (183-188). The real format-12 fixtures move to
  `test/fixtures/format12/` (189-196), `media_test` sweeps bytes 0 to 15 (197-199), and F20
  freezes the manifest family's frame (848-853). The code is owed in 7c-1 and 7c-6 (K1, K6, K11;
  `format13.md:6, 11, 16`).
- **F13-R2: closed in the design**, with F13-R25 1 to 3 open. Key routing is the planner's
  (format-13.md:396-403). Terms and keys are disjoint by stage 1 and the builder (390-395).
  Expansion follows AUT-R4's refinement (404-414), the reader scans from a lower bound
  (416-432; PLAN.md:47-48), and `keys` holds the key's bytes exactly (384-386).
- **F13-R3: closed in the design**, with F13-R25 6 open. One per-kind table (205-251), masks per
  plugin (253-278), the three classifications of a set bit (280-292), and the free-and-absent
  cases (311-316).
- **F13-R4: closed.** The tail holds pinned sections only (format-13.md:328-332), the length
  statistics lead `term_index` with no inner CRC (365-378), and the layout is canonical
  (322-327). `lengths` moves after `terms` (333-338), accepted below. T7's procedure is F13-R25 5.
- **F13-R5: closed** (format-13.md:876-890, 1085-1092; K8, `format13.md:13`). One commit changes
  the digest; the keys commit's interim record holds `digest.txt` until then. F13-R18 and
  F13-R25 4 are open against it.
- **F13-R6: closed.** Eight candidates (format-13.md:470-490), one reader path with hits and
  misses at 1 and 50 segments (492-501), a decision rule (503-505), the estimates with the filter
  (507-522), the key count (457-458), and the counterpart row (524-529). K2's ordinals through
  `find_run` answer my `termdict` question (479-484). Q1 is answered: keyed `-bp`
  (PLAN.md:38-41). New against it: F13-R19, F13-R20.
- **F13-R7: closed** (format-13.md:93-105, 125-160, 621-627). The carried rows are freeze
  criteria on SYS-6's evidence, with the projection's interval stated (138-145).
- **F13-R8: closed** (format-13.md:554-627, 660-671, 698-747). Offsets relative to the group's
  base fit 16 bits: 32 blocks of at most 513 + 1,155 bytes span 53,376 bytes (576-584). Level 1
  keeps format 12's wide path, and the flags have a field of their own (568-570). F12's lasts
  appear only in flagged terms (666-668), validation is per block decoded (606-619), and losers
  leave the format (731-732). F13-R21 is new against two of its candidates.
- **F13-R9: partially closed.** The writer refuses a version without a key in every segment, and
  `check()` verifies it (format-13.md:542-546). The width rule fixes only width 64 (547-550):
  F13-R25 7.
- **F13-R10: closed** (format-13.md:390-395, 445-451).
- **F13-R11: closed by layers' 7a** (`d911396`; `merge.hh:226-233` at `707066b`, designated
  initializers), with `merge_options::key_prefix` deleted in 7c-3 (format-13.md:1085).
- **F13-R12: closed** (format-13.md:109-123, 234-239, 269-273, 938-982), with
  `frozen-index.md:46-49`'s change listed (87).
- **F13-R13: partially closed.** Closed: `binary` columns (775-780), one version scheme (781-783),
  term-list frames bounded and keys left out of them (811-817), the LEB128 sort count (826-827),
  and points by F23 (789-802). Reopened by the owner's Q6: bytes per dimension (F13-R22).
- **F13-R14: closed** (format-13.md:908-920), with F13-R24 against its zstd exception.
- **F13-R15: closed** (format-13.md:984-1003, 921-936), with F13-R25 5 against T7's procedure.
- **F13-R16: partially closed.** K1 to K12 are written (`format13.md:5-17`) for the lead to
  confirm (C1). ASan completed in 49 min 15 s at `4266374` (run 39069758885). The margin is still
  thin: one layers code tree took 52 and 38 minutes (the layers re-check's C7), and main's split
  is queued (main's index, "Queued", item 9). See C8.
- **F13-R17: closed** (format-13.md:93-105, 404-414, 318-345, 844-874).
- **C1: open** until the lead confirms or edits K1 to K12.
- **C2: open.** macOS and FreeBSD fail at `4266374` on main's `std::jthread`; the rebase onto
  `707066b`, green on all eight (run 39070316303), brings `adb329e`.
- **C3: closed in PLAN** (PLAN.md:16-20); every candidate table still owes it.
- **C4: held.** `4266374..abd5741` touches only documentation.
- **C5: closed.** `calibration.md`'s third row.
- **C6: closed** (format-13.md:789-802, 938-982). All six of its points are F23's rules. F23's
  rule 3 adds one: a structure may change how its owner's sections decode where its bit is set.
  P14 and P18 need it, and older binaries refuse those segments (rule 5).
- **Q1 to Q4: answered and applied** (format-13.md:1007-1014). **Q5 and Q6: answered by the
  owner** (PLAN.md:51-54): F13-R23 and F13-R22.

### The two proposals to layers

1. **`lengths` after `terms`: accepted** (format-13.md:43, 83, 333-338). It supersedes my
   F13-R4 fix 2, which put `lengths` last only to take it out of the tail. With F7's statistics
   out, an open reads none of `lengths` wherever it sits.
   - After `terms`, `inverted`'s streamed sections stay contiguous and in layout order, S1's
     rule (layers.md:200 at revision 3). The layers re-check's R19 item 9 reads that order as
     the table's rank.
   - It spares the sink from holding the encoded lengths, 10.1 MB at 5M
     (`phase-3-ratchets.md:191`), across `keys`' and `stored`'s sections, charged to the flush.
   - L2's format-13 column (layers.md:200) changes in 7c-1. R15's protected-page test covers
     `lengths` in its new place.
2. **The structure mask in the manifest: accepted** (format-13.md:62, 85, 855-864). It is the
   layers re-check's R14 fix 1 at structure granularity, and what C6's rule 3 asked of L16.
   - A bit joins the mask when the index is configured with its structure. It leaves once no
     segment sets it and the configuration has dropped it. So an index stays readable by a
     binary that lacks a structure the index never used, which L16's "the writing binary's
     plugins" (layers.md:865-867 at revision 3) breaks.
   - Plugin names cannot say whether an index holds `norms`, or P8's, P14's, P18's, or P20's
     sub-structures, which a binary may lack while it has their owner.
   - Conditions:
     - (a) A binary's readable mask is defined once (F13-R25 6).
     - (b) L8's length record is keyed on the mask's `inverted` bit (F13-R25 4).
     - (c) Under mixed binaries a bit joins only when every replica reads it (F23 rule 6).
     - (d) L16's and L7's text and L16's tests change with 7c-6: an unreadable bit at open and
       at `install`, a version range above the binary's, and a segment outside the mask or the
       range.

### The rebase plan

Sound, with one ordering defect (F13-R18).

- **The base.** `707066b` passes all eight jobs at `1d60098`, the same code tree (run
  39070316303). The tip `1b6a148` adds only the layers re-check, so the base stands.
- **The mechanics** are sound (format-13.md:1045-1053): a new branch at the base, the
  documentation cherry-picked, and `format.hh` and `keys.hh` taken from `e779835` as starting
  content. The old commits' tests are the starting cases, and the force push needs the lead's
  approval, with the old head kept by name.
- **The table of old commits** states each commit's fate accurately (1057-1067).
- **7c-1 to 7c-7** follow this review's order (1069-1096). Each commit is green on all eight jobs
  before the next (L14), and only 7c-5 changes `digest.txt` (K8). The split of F9 across 7c-4 and
  7c-5 breaks determinism for one commit (F13-R18).

### Findings

18. **F13-R18. 7c-4 writes keyless segments before 7c-5's version rule** (rebase plan; F9).
    - 7c-4 brings keyless segments (format-13.md:1087-1089), which have no `versions`, so
      `version(ord)` is 0 (535-536). 7c-5 sets a keyless document's version to 0 (1090-1092).
    - In between, a keyless index's buffered document has version 1, or `current + 1` after a
      docid replace, and its flushed copy reads 0. Verdicts and the digest then depend on flush
      timing, which is the nondeterminism F9 exists to remove (538-541). T2's schedules fail on
      a keyless index, or replicas disagree.
    - At `e2ac3fe` both halves were one commit.
    - Fix: keyless segments move into 7c-5 with the version rule, or 7c-4 keeps writing
      `versions` in keyless indexes until 7c-5. Before 7c-4.

19. **F13-R19. K3 is a second dictionary implementation beside `termdict`** (F8; calibration,
    first row).
    - K3's library change is "none" (format-13.md:476). It has front-coded blocks, an `fst` over
      each block's first key, and a scan that "floors the blocks' `fst` and decodes one block"
      (435-436). That is `termdict`'s structure, blocks under an FST of their first keys, with
      another block body.
    - Terms and keys would then have two block codecs, two seek paths (AUT-R3's
      `cursor::seek` is `termdict`'s), and two fuzz targets.
    - By the estimates only K3 passes the dictionary row with the filter (519-522), so it is the
      likely choice.
    - Fix: K3 becomes a `termdict` option, zstd-compressed block bodies with raw blocks for
      oversize keys, under `termdict`'s own index and cursor. P12's dictionary can then be
      measured with the same option. Otherwise the design states why `termdict` cannot host
      it. Before K3's codec unit (format-13.md:1108-1110).

20. **F13-R20. K4's `termdict` parameters change every dictionary's bytes** (F8, F22).
    - K4 records the restart interval and block bounds "in its index" (format-13.md:477,
      1121-1123). `termdict`'s index freezes with format 13 (908-913), and `term_index` uses
      the same form.
    - Fix: the parameters are recorded only when they differ from today's defaults, so today's
      bytes stay. Otherwise F22 states the new form and every fixture re-pins with it. Before
      the `termdict` change.

21. **F13-R21. M17 and M18 need the segment to record the preset they assume** (F11, F7).
    - M17's hook says "no bytes" (format-13.md:649), but its bound holds only under the preset
      the writer ordered by. "A query under the other preset, or under parameters the order did
      not use, evaluates every pair as today" (`query-gap.md:420-428`). M18's argmax pairs are
      per preset likewise (429-431).
    - An index option can change after a segment is written, and the segment cannot say which
      preset it used.
    - Fix: if either is kept, the segment records the preset's scheme and parameters (k1, b,
      `min_normlen`) with `inverted`'s statistics at `term_index`'s head (F7). Readers use a
      list's first pair, or a block's argmax pair, only under the recorded preset. Before
      either is kept.

22. **F13-R22. Q6: bytes per point dimension up to 16** (F15a, F15; the owner's ruling).
    - The owner rules that bytes per dimension stay up to 16, Lucene's maximum, and that the
      points format needs a mechanism for 128-bit values such as IPv6, not a 64-bit cap
      (PLAN.md:52-54).
    - Revision 1 caps them at 8 (format-13.md:769, 772-774) and lists `limits.md:36`'s change
      to 8 (89). My F13-R13 bullet recommended that, and it was wrong under `limits.md:3-5`: a
      limit below the largest reference engine's needs a measured cost, and D3a's 64-bit types
      are a design choice, not a cost (C7).
    - Fix: F15a accepts 1 to 16 bytes per dimension, and the design names the mechanism.
      Recommended: a `bytes` slot declared with points at w bytes per dimension (w ≤ 16) holds
      values of exactly w bytes each, refused otherwise at stage 1, and compared as unsigned
      big-endian bytes. IPv6 addresses and uuids fit, their doc values stay the `bytes` kind's
      dictionary ordinals, and no column kind is added; points' first release (F23) defines the
      tree's byte dimensions. D3a's "bytes ... never compared as values" (`phase-4.md:143-144`)
      then excepts a slot's points. The alternative is a 128-bit logical type with its own
      column kind, gated by a bit reserved under F23.
    - The changes-owed row for `limits.md:36` goes, and `limits.md` keeps "up to 16". Before
      the `values` plugin's code, since F15a's declaration freezes with `values`.

23. **F13-R23. Q5: bit 13 is reserved for `stored_spans`** (F4, F19, F23; the owner's ruling).
    - The owner reserves bit 13 for chunked stored records under F23 (PLAN.md:51-52). Revision
      1 still calls it proposed (format-13.md:274, 277-278, 836-838, 1018-1024).
    - Fix:
      - F4's row 13 reads "reserved (R19)".
      - F19 states that in a segment that sets bit 13, bit 31 of a frame index entry's size
        means "the frame's last record continues", and that elsewhere it is corruption, as now.
      - F23's table gains the row: owner `stored`, no section kind, configured by a `stored`
        option or by the document-size design, no digest record (`stored`'s record holds the
        whole record).
      - T1 sets bit 13 among the reserved bits and treats 14 to 31 as outside the table (993).
    - Before the freeze.

24. **F13-R24. F22's zstd exception needs a coordinated restart** (F22; PLAN.md:31-33).
    - "Every replica moves to it together, since two binaries would write different bytes for
      one merge" (format-13.md:915-917). Under the owner's SYS-3 decision a cluster upgrades
      one node at a time, so replicas on two zstd releases coexist.
    - Fix: F22 states that nothing compares segment bytes across binaries: replicas compare
      digests (L8) and each binary checks its own CRCs, and T8's byte identity is per binary.
      Otherwise zstd's release is pinned for format 13's life. Before the freeze.

25. **F13-R25. The layers re-check's format-13 items, unanswered in revision 1**
    (`layers-design.md:635-746, 806-811` at `1b6a148`). Each goes into the design before the 7c
    commit named.
    1. **The key prefix's home (R13).** F8 and F20 make the prefix the `keys` structure's option
       (format-13.md:388-389, 870-871), and the `inverted` builder refuses key-prefixed terms
       (390-393), so `inverted` reads `keys`' option. That dependency runs in the direction
       F13-R2 removes, and the wording was mine (F13-R2 fix 2). Fix: the prefix stays the
       index's, in the manifest's index fields. The core passes it to `validate_context`, to the
       flush and merge contexts for the builder's backstop, to `check()`, and to the planner. No
       plugin reads another's options. Before 7c-3.
    2. **The add path (R13).** "Every key starts with it" (format-13.md:388-389) names no check
       for an add's `document::key`. Fix: stage 1 refuses an add whose key lacks the prefix, the
       add path's form of `index.hh:1576`. Before 7c-3.
    3. **Buffered documents (R13).** Delete by query (D16) over an id term or wildcard runs L10's
       buffer-path predicates, which have no key leaf. Fix: the key leaf over a buffered
       document's key, under F8's routing. Before 7c-3.
    4. **The length record (R14).** F21 emits `inverted`'s length record for "every document"
       (format-13.md:878-880). Fix: key it on the index's mask, so that every document of an
       index whose mask holds `inverted` emits one and a `search_layout` index without
       `inverted` digests equal to a `document_layout` index (L12). Before 7c-5.
    5. **T7's procedure (R15).** T7 counts verified-region pages (format-13.md:999), which a
       pinned read never touches. Fix: R15's test, an anonymous copy with every page between
       the header page and the tail's first page made inaccessible, opened, with
       `verified_regions() == 0`. Before T7's test.
    6. **Readable bits and one presence (R16).** F4 says what a set bit means (282-292), not
       which bits a binary reads. Fix: the readable bits are the core's, `index_sort` included
       since F18's reader validates it, and each layout entry's `P::structures`.
       `basic_segment` computes presence from the footer before the readers and passes each its
       bit in `open_context`, so `holds<P>()` and the reader's emptiness are one bit. Before
       7c-1 and 7c-4.
    7. **Versions' range (the layers re-check, `layers-design.md:806-811`).** The reader requires
       the minimum to be at most 2^64 − 2^width (`reader.hh:506`). A valid section breaks that at
       any width above 1 when its versions sit near 2^64 − 1. F9's "minimum 0 when the width
       would be 64" (format-13.md:547-550) fixes one case. Fix: the writer writes minimum 0
       whenever minimum + 2^width − 1 would exceed 2^64 − 1, which makes the width 64. Before
       7c-5's writer check.

26. **F13-R26. Text and ledger.**
    - The ledger's state reads "designing (ready for preliminary review)" (`format13.md:4`). It
      waits for this re-check.
    - The plan names `707066b`; layers' tip is `1b6a148`, the same code tree. Say so
      (format-13.md:1038-1041).
    - 7c-3's interim `P` record covers buffered documents' keys as well as segments'
      (1085-1086), since a buffered document's terms no longer hold its key.
    - 7c-3 moves the key from `terms` to `document::key` in every keyed test and tool; name that
      scope.
    - Q5 and Q6 move to "Answered" (1016-1032), and the changes-owed table loses `limits.md:36`'s
      row (89).

### New blocking items

- F13-R18: in the plan, before 7c-4.
- F13-R25: items 1 to 3 before 7c-3; 4 before 7c-5; 5 before T7's test; 6 before 7c-1 and
  7c-4; 7 before 7c-5.
- F13-R19 before K3's codec unit; F13-R20 before K4's `termdict` change.
- F13-R21 before M17 or M18 is kept.
- F13-R22 before the `values` plugin's code; F13-R23, F13-R24, and F13-R26 before the freeze.

### Conditions

- **C1 and C2 carry**, with the status above. C3 to C6 are closed.
- **C7. A calibration row**, for the lead: my F13-R13 bullet accepted an 8-byte cap on bytes per
  point dimension, justified by D3a's 64-bit types, and the owner ruled that the cap breaks the
  limits rule (Q6). The check that catches it: for every limit below the largest reference
  engine's, find the measured cost `limits.md` records; a design choice is not one.
- **C8.** The layers re-check's C7 holds for format 13 too: main's ASan job is split, or its
  limit raised, before K5 is cited.
- **C9.** The freeze's candidate report copies every baseline file it cites into `docs/perf/`
  with its commit (calibration, third row): the keyed `-bp` sizes (`results/keyed-bp-n2.sizes`,
  `phase-3-ratchets.md:195`, on the gate VM, neither in the repository nor on this host),
  format 12's T5 row, and the Phase 3 gate's query rows.

### In one line each

- Verdict: APPROVE WITH CONDITIONS. Step 7c may start with F13-R18 fixed in the plan and each
  F13-R25 item before its commit; F10 after 7c's F2 to F8 commits.
- F13-R1: closed in the design (format-13.md:165-199, 347-359, 848-853).
- F13-R2: closed in the design (382-451); F13-R25 1 to 3 open.
- F13-R3: closed in the design (205-316); F13-R25 6 open.
- F13-R4: closed (318-345, 365-378); T7's procedure is F13-R25 5.
- F13-R5: closed (876-890, 1085-1092); F13-R18 and F13-R25 4 open.
- F13-R6: closed (470-529); F13-R19 and F13-R20 new.
- F13-R7: closed (93-105, 125-160, 621-627).
- F13-R8: closed (554-627, 660-671, 698-747); F13-R21 new.
- F13-R9: partially closed (542-550); F13-R25 7 open.
- F13-R10: closed (390-395, 445-451).
- F13-R11: closed by layers' 7a (`merge.hh:226-233` at `707066b`).
- F13-R12: closed (109-123, 234-239, 269-273, 938-982).
- F13-R13: partially closed (775-827, 789-802); bytes per dimension reopened (F13-R22).
- F13-R14: closed (908-920); F13-R24 open.
- F13-R15: closed (984-1003, 921-936).
- F13-R16: partially closed (`format13.md:5-17`; ASan at 49 min 15 s); C1 and C8.
- F13-R17: closed.
- C1 open, C2 open, C3 to C6 closed; Q1 to Q4 applied; Q5 and Q6 answered (F13-R23, F13-R22).
- `lengths` after `terms`: accepted; L2 changes in 7c-1.
- The manifest's structure mask: accepted, with conditions (a) to (d); L16 and L7 change in 7c-6.
- The rebase plan: sound; F13-R18.
- F13-R18: 7c-4's keyless segments precede 7c-5's version rule.
- F13-R19: K3 duplicates `termdict`; make it a `termdict` option.
- F13-R20: K4's recorded parameters must leave today's bytes unchanged.
- F13-R21: M17 and M18 need the preset recorded in the segment.
- F13-R22: bytes per dimension up to 16, with a mechanism for 16-byte values.
- F13-R23: bit 13 reserved for `stored_spans` in F4, F19, F23, and T1.
- F13-R24: the zstd exception without a coordinated restart.
- F13-R25: the layers re-check's R13 to R16 and its versions note, each before its 7c commit.
- F13-R26: text and ledger.
- New conditions: C7 (a calibration row), C8 (the ASan margin), C9 (baseline files copied).

Not run: no build, test, sanitizer, fuzzing, or benchmark (the host's strict hold until about
01:17); no probe written or compiled in this re-check, since revision 1 changes no code; the
merge probe not rerun; no VM, so the keyed `-bp` sizes file was not read; the query-gap design
and layers' revision 3 read only where format 13 cites them or they cite format 13; Lucene's and
Tantivy's source; no Xapian or Xapiand GPL source.

## Second re-check

- Date: 2026-10-18.
- Reviewed:
  - `git log bae7fc1..phase4/format13` at `1cb2927`, 15 commits: the ledger (`ac255f8`,
    `6f45f68`, `1cb2927`); F13-R18 to F13-R26 in the design (`1db48ab`, `8d23cc0`, `25f73a4`,
    `f90018a`, `072b57b`, `4b67199`, `4bee6fa`, `4e2ed32`, `b362258`, and `0864710`, a second
    commit for F13-R25); main's `std::jthread` fix cherry-picked (`b2f7600`); and the proposed
    copy-merge items (`78bd191`). Only `b2f7600` changes code.
  - The proposal's source: `docs/research/indexing.md` §5.3 and `research/indexing/mergecost.cc`
    on `research/indexing` at `5f956bc`; the tip `e5c3ddc` adds one ledger commit.
  - `docs/design/small-indexes.md` at main `6e9ba58`, and `perf/small-indexes`' plan
    (`docs/perf/small-indexes.md` at `4b179a5`: M0 to M2, written, not yet measured).
  - What other designs now address to format 13: query-gap revision 2 at `design/query-gap`
    `3cba768` ("What format 13 must record", `query-gap.md:808-822`; its review's QG-R4, QG-R5,
    and C7); layers revision 4 at `phase4/layers-refactor` `ea04731` (R12 to R19); automata at
    `de2f5eb` (nothing new since revision 1 folded it); `termdict` at its pin `eaf83b3`.
- Checked against: `calibration.md` at main, all eight rows, each applied where a finding names
  it; PLAN at main `6e9ba58`: the owner decisions (PLAN.md:22-58), Baselines with PISA (99-105,
  `4692934`), Reference (106-108), targeted runs, one calibration, and one confirmation (109-122),
  Carried rows (129-142), and the many-index workload (183-192, `b320132` and `6e9ba58`); this
  review's F13-R18 to F13-R26 and C1 to C9.
- Checked by:
  - CI through `gh`, logs in `.scratch/review-format13-r2/`. Run 39096431335 at `78bd191`: all
    eight jobs pass (ASan 37 min 20 s, TSan 28 min 49 s). Run 39098644329 at `1cb2927`, the same
    code tree: seven pass; ASan was cancelled at its 60-minute limit with 27 of 34 tests done,
    `lifecycle` taking 1,797 s against 1,044 s in the passing run. Run 39089855611 at `b2f7600`:
    attempt 1's ASan cancelled at the limit (02:22:51 to 03:23:07), attempt 2 cancelled by the
    next push.
  - `b2f7600` against `adb329e`: the same patch, line for line. `707066b` and `ea04731` both
    contain `adb329e`.
  - `test/bisection_test.cc` at `1cb2927`, built with Homebrew clang 23 at `-O1` under TSan, and
    under ASan with UBSan (`nice -n 19`, one job): both pass with no report (`bisection_tsan.out`,
    `bisection_asan.out`).
  - `small_seg_probe.cc`: keyed segments of the corpus's first 1, 3, 10, 100, and 1,000
    documents, written by the branch's writer, opened, and split into section content,
    padding, table, and footer (`small_seg_probe.out`). Byte counts only.
  - `py/fullblocks.py`, `py/copyshare.py`, and `py/runbase.py` over research/indexing's 100k
    sample (`corpus100k.json`): the full-block share by source size; the postings copy merges of
    2, 10, and 100 sources leave in short blocks; and the first blocks a copy must re-encode
    without run bases. `fullblocks.py` reproduces the note's 100k figures (8,800,623 postings,
    338,367 terms, 7,673 of them at df ≥ 128 holding 81.5%).
  - Reading `termdict.hh` at `eaf83b3` for F13-R19 and F13-R20, the postings encoder
    (`writer.hh:730-766` at `1cb2927`), and every citation the commits added at `707066b`
    (`search.hh:48`, `reader.hh:501-508, 580`, `files.hh:177`, `index.hh:631-640, 1576`,
    `writer.hh:682-700`): each says what the design says.
  - A grep of strata's and its pinned libraries' non-test code for aligned loads, and for
    `section_alignment`'s users.
  - `HEAD` against `origin` for every branch read: none ahead, every tree clean.
  - Not run: no benchmark or timing (other agents' light holds `inverter-vm-e20`,
    `inverter-vm-e21`, and `si-m0`); no full suite under sanitizers locally, so the CI runs
    above are the suite's evidence; no fuzzing; `research_mergecost` not rerun; no VM, so the
    keyed `-bp` sizes and PISA's index were not read.

### Verdict

**APPROVE WITH CONDITIONS: F13-R18 to F13-R21 and F13-R23 to F13-R26 are closed in the design,
and F13-R22 partially (one `limits.md` row); the cherry-pick is main's fix, line for line. None
of the five copy-merge items enters format 13's frozen grammar: items 1 to 3 wait for a release
under a structure bit reserved now (bit 14), with item 1 replaced by a run table, and items 4 and
5 are merge rules. Small segments get 8-byte alignment now and a reserved core bit (bit 15).
Open: PLAN's PISA references and targeted runs (F13-R29), the many-index rows (F13-R30),
query-gap revision 2's format-13 text (F13-R31), the digest's key (F13-R28), `termdict`'s options
form (F13-R27), the empty key prefix (F13-R33), and text.** Step 7c may start on the base the
lead names: F13-R33 comes before 7c-3 and F13-R28 before 7c-5, and F13-R30's alignment is
cheapest before 7c-1.

### Status

- **F13-R18: closed** (format-13.md:71, 620-624, 1219, 1254-1264). Keyless segments and the
  version rule are one commit again, 7c-5, as they were at `e2ac3fe`. Through 7c-4 every index
  keeps `keys` and writes `versions`, so a keyless document's version reads the same before and
  after a flush and T2's schedules stay deterministic in every commit. The call is right, and it
  keeps F13-R5's single digest change. Its consequence for 7c-3 is F13-R33: until 7c-5,
  keyless indexes carry `keys` with an empty prefix.
- **F13-R19: closed** (528, 537-547, 1281-1283, 1294-1298). K3 keeps `termdict`'s FST, blocks,
  front coding, and runs, so `find_run` (`termdict.hh:463-477`), `scan` (479-493), and
  automata's `cursor::seek` serve it; the codec arrives as a function pair beside the verifier
  (449-453), and the fuzz target is `termdict`'s own (`fuzz/fuzz_reader.cc`, present at
  `eaf83b3`). F13-R27 is against the reader its 128-entry blocks need.
- **F13-R20: closed** (529, 548-557). The developer's call holds in the code: the builder writes
  the alphabet's size as a LEB128 of 0 to 255 (`termdict.hh:130`), and today's reader decodes the
  whole LEB128 and refuses any value above 255 as `termdict::corrupt` (204), so a value above 255
  can announce the options form, and today's settings (32, 32, 64, 1,024; 59-62) keep today's
  bytes. F13-R27 is against its last sentence.
- **F13-R21: closed** (407-416, 731-732, 826-827). One preset is 1 + 1 + 3 × 8 = 26 bytes, the
  scheme codes follow `search.hh:48`'s declaration order, and a reader uses a pair only under a
  bit-equal preset. F13-R31 takes out its M18 half (QG-R4) and adds QG-R1's preconditions.
- **F13-R22: partially closed.** In the design: F15a takes 1 to 16 bytes per dimension with its
  mechanism (849-865), D3a's exception is owed (111), and Q6 is answered (1185-1186); index and
  data dimensions, at most 8 and 16, equal Lucene's, and neither Tantivy nor Xapian has points
  (calibration, sixth row). Open: `limits.md:36` still reads "Recorded per tree" and "a format
  field, chosen by D15's measurement" in strata's columns; its "up to 16" is Lucene's column. My
  F13-R22 fix said `limits.md` "keeps 'up to 16'" and had the owed row dropped, a misread the
  developer's ledger caught (`format13.md`, "Next" 2). Fix: the owed-changes table regains
  `limits.md:35-36`: dimensions per slot declaration (F15a) as well as per tree, bytes per
  dimension 1 to 16, mechanism F15a's byte and the fixed-width `bytes` rule. Before the freeze
  (C10).
- **F13-R23: closed** (296, 299-301, 924-929, 1075, 1160, 1183-1184). In a segment without bit
  13, bit 31 of a frame's size still fails the frame bound (`reader.hh:580` at `707066b`;
  `max_stored_frame` is about 64 MiB).
- **F13-R24: closed** (986-988, 1012-1020, 1167). The developer's call holds: replicas compare
  the digest (`REPLICATION.md:64-66`), physical files differ between replicas (86), `install`
  checks a checkpoint's files against the source's own manifest (`load_files(source, theirs,
  ...)`, `index.hh:631-640` at `707066b`), and a scrub checks each file against its own CRCs
  (`REPLICATION.md:177-178`). No design compares segment bytes across binaries; C11 keeps it so
  for the designs still to come.
- **F13-R25: closed** (items 1 to 7 at 429-438, 451-454, 975-978, 1166, 303-306 and 337-340, and
  625-632; owed changes 104-107; 7c at 1228-1264). The minimum rule's arithmetic holds: at width
  w ≤ 63 its condition gives lo > 2^63, so the new width is 64, and at 64 it gives lo > 0.
  Layers' revision 4 builds R13 to R16 on its side (`ea04731`). F13-R28 is against item 4's
  parenthesis, whose wording was mine.
- **F13-R26: closed** (ledger:4; 1192-1193; 1246-1253, whose list matches every keyed test and
  tool at `707066b` by grep; 1172-1186; 99-113). The `limits.md:36` row it dropped returns under
  F13-R22.
- **`b2f7600`: accepted.** It is `adb329e`'s patch, line for line. The joiner is declared after
  `workers`, so it joins before the vector is destroyed; if starting a later thread throws, the
  started workers drain `parts` without the caller, since `work` returns once the list is empty
  and none is busy (`bisection.hh:511-538, 541-548`), so the join ends. `bisection_test` passes
  under TSan and under ASan with UBSan. It adds no mechanism: the second pool is main's, which
  `phase3/bisection-pool` replaces (calibration, first row). Both rebase bases hold `adb329e`, so
  the rebase drops it (F13-R34).
- **C1: closed** (ledger:5, confirmed by the lead). **C2: carries**: no criterion cites a code
  commit yet, and `78bd191` is green on all eight. **C3 to C7: closed** (C7 is calibration's sixth
  row). **C8: open**: of the three runs at this code tree, ASan passed in one and stopped at its
  limit in two; main's queue item 9 is still queued. **C9: carries**, with PISA's figure added
  (F13-R29).

### The proposed copy-merge items (`78bd191`)

**Ruling: none enters format 13's frozen grammar. Items 1 to 3 change how `postings` and
`positions` decode, so they come within format 13 only under a reserved structure (F23), and
format 13 reserves bit 14 for them now (F13-R32). Items 4 and 5 change no byte: they are the copy
merge's rules (L6) and wait for its design.**

What the case rests on:

- The 95% (indexing.md:406-411) is the merged 5M segment's shape, not a merge's inputs. A copy
  copies its sources' full blocks, which hold 16.0%, 46.0%, 68.9%, and 76.6% of the postings of
  sources of 1,000, 10,000, 50,000, and 100,000 documents (`fullblocks.out`). The many-index
  workload merges small sources, where most postings sit in tails.
- The 20× compares a floor with today's implementation. The floor copies each section once
  (`mergecost.cc:146-155`), where a merge copies per term and per source, and leaves out the
  skip rewrites; today's merge runs at 497 ns a posting. By the note's own stage table, encoding
  costs 13.1 ns a token, 27.2 ns a posting at 2.08 tokens a posting (indexing.md:25-26, 65), and
  the dictionary walk and build, which both paths pay, cost 23.6 ns a posting (452-455). With
  decoding no dearer than encoding, a re-encoding merge at its bound is about 51 to 78 ns a
  posting, 2.1× to 3.2× the copy's floor: an estimate, from two hosts' numbers. Most of the 20×
  is merge code that needs no format change, such as the per-term cost the note's
  recommendation 2 cuts in the merge as in flushes (654-660).
- No copy-merged segment exists, so neither its bytes nor queries over it are measured.

Each item:

1. **A stored list base: does not enter as written, and waits.** A base per list serves a term
   whose list starts in a later source. A term in two sources has its second run's first block
   coded against −1 in its own source (`prev` starts at 2^32 − 1, and a bitset starts at the
   previous last + 1, `writer.hh:730-738`), while the merged list's reader bases it on the
   previous block's last, so it decodes wrong unless it is re-encoded or given its run's base.
   Re-encoding every later run's first block costs 10.3% of the copyable postings at 2 sources
   and 29.6% at 10 (`runbase.out`). Recommended for the release: a per-segment table of run
   starts, each block's base being max(previous last, the start of the run holding its last
   − 1). In a segment of one run that is today's rule; it costs no byte per term; and F10's
   per-block check and M23's 64-bit sum read that base.
2. **Short blocks anywhere: wait, under bit 14.** They cost more than "a few bytes per
   boundary": copy merges of 2, 10, and 100 sources leave 12.0%, 34.4%, and 66.4% of the
   sample's postings in short blocks of terms with skip data, against 4.9% in tails after a
   re-encoding merge (terms under 128 postings, 18.5%, are short either way; `copyshare.out`),
   and a short block spends at least one LEB128 byte on each delta and each wdf
   (`writer.hh:763-766`). A copy never re-blocks, so every flushed source's tail survives every
   later copy. The copy is therefore a per-term choice by a deterministic rule that bounds
   short blocks, and an index that is never force-merged (reordering runs at force merge only,
   `limits.md:23`) keeps what that rule admits, which its disk rows measure. The tail code is a
   third codec code in F10's flags only in segments that set the bit (F23 rule 3), and F10's
   16-bit offsets hold: a short block of 127 postings takes at most 1,270 bytes, under
   513 + 1,155.
3. **Positions: wait with item 2.** A position block holds 128 positions, not 128 postings
   (`writer.hh:35-37`), so a source's position tail sits mid-stream. Level 0's first-position
   fields (801) already locate each run's positions; the release states the rest.
4. **Concatenation order: no format change; waits for the copy merge's design.** Kind 2 exists.
   That design states two limits: in an index with `index_sort`, a copy only where the
   concatenation keeps the order (sources whose sort ranges are disjoint and increasing), since
   bit 7 comes from the configuration and says the ordinals follow the sort (F4, F18); and a
   permuted output gives up the docid-ordered tie skip (PLAN, Phase 3), a query cost to measure.
5. **Re-encoding only sources with deletes: no format change; waits.** P17's premise
   (`reclamation.md:112-114`) is narrower than it reads: a deleted document anywhere in a
   block's ordinal span changes the gaps after it, so "no deleted document" means none in the
   span, and format 12 has no per-block base to rewrite (item 1). P17 is this copy path with
   deletes, one mechanism, so it takes bit 14 too, and F1's P17 row changes (F13-R32).

The developer's questions:

- F10: a third code, under the bit only; the offsets hold (item 2).
- M23 and F12: the run table's base is what "the previous entry's last" becomes (item 1);
  F12's partitions carry their own bounds and copy unchanged.
- M17, M18, and F13: a copy re-sorts each copied frontier at the merged segment's average
  (QG-R2: a sort of a few pairs, no decode); M18 is out (QG-R4), and would have needed every
  block decoded; F13's copied pairs stay valid lower bounds, since at least k postings of the
  merged list dominate each, and the release states whether it recomputes them.
- F21 and T8: the output is a function of the inputs and the index's options once the per-term
  rule is deterministic; `merge_test`'s "a merge equals a flush" becomes digest and reader
  equality for copy merges; the digest is unchanged.
- T3: unaffected, since the gate's index is reordered and re-encoded.
- The joint selection: neither candidates nor rules for every list. It chooses the frozen
  encodings on a re-encoded index, where no run exists, and `copyshare.out` rules out a rule
  for every list.

The version and reader rules: under F23, a format-13 binary without the release refuses a
segment that sets bit 14 as `unsupported_segment` and an index whose mask holds it at open
(rule 5; F20); the index option that enables copy merges is a replicated operation, accepted only
once every replica reads the bit (rules 4 and 6); a segment without the bit decodes as frozen
(rule 3). No format 14 is needed. The release brings T1 cases, fixtures, an hour of fuzzing per
changed decoder, and its rows (rule 8).

The many-index workload: the copy's gain grows with source size, so it pays most where sources
are large and least for the smallest indexes. The release is measured before it lands (C12). The
note's 4.5× projection counts copy merges (indexing.md:632), and bit 14 keeps them within format
13.

### Small segments: what format 13 reserves

Yes: one decision now and one reservation, with F4 changed so that later items need none.
`small_seg_probe` at `1cb2927`, keyed segments of the corpus's first documents, in revision 0's
layout, whose header slot, alignment, table, and footer revision 1 keeps:

| Documents | File | Padding | Table and its padding | Footer | Fixed share | `key_index` | Pinned sections |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1,828 | 430 | 371 | 56 | 46.9% | 313 | 387 |
| 3 | 3,176 | 458 | 425 | 56 | 29.6% | 327 | 416 |
| 10 | 7,656 | 507 | 400 | 56 | 12.6% | 352 | 574 |
| 100 | 66,332 | 425 | 483 | 56 | 1.5% | 513 | 1,660 |
| 1,000 | 566,588 | 393 | 911 | 56 | 0.2% | 1,909 | 7,524 |

Pinned sections are `docids`, `stats`, `norms`, `versions`, `term_index`, and `key_index`.

1. **Sections align to 8 bytes, decided now** (F5, F6, F15). The padding is F5's 64-byte
   alignment over 12 gaps, and nothing needs it: fixed-width reads are `memcpy`s
   (`format.hh:174-179`; `docset`'s frozen view likewise, `docset.hh:1039`), `docset`'s SIMD
   loads are unaligned (`_mm_loadu_si128`, `vld1q`), no aligned load exists in strata's or its
   pinned libraries' non-test code, and `section_alignment`'s only users are the writer
   (`writer.hh:428, 495, 513, 540`) and two tests. At 8 bytes the padding is at most 84 bytes,
   about 390 bytes less on average, 21% of a one-document file. F15's columns keep their 8-byte
   alignment, and F6's "64-byte slot" sentence (390) goes. My F13-R4 fix 4 took 64 from the
   constant without asking what needed it.
2. **Reserved now: bit 15, `compact`, the core's**, for the small-segment profile
   (`small-indexes.md`, direction 4). Its release may change, in segments that set it, the
   table's entry form: offsets the canonical rule already implies, lengths as LEB128, and one
   whole-file CRC in place of the per-section and region CRCs. F23's rule 3 extends to the
   core's table for it; the prefix (F2) and the footer (F6) never change, so a reader reads the
   bit before the table. It is configured per index (F23 rule 4), so F4's "never from the
   documents" holds, and `perf/small-indexes`' M0 to M2 measure it first.
3. **F4: a bit outside the table becomes a bit this binary lacks** (unsupported for a lone
   segment, and by the mask in an index), not corruption (315-316). Then a later item of a
   plugin's own, such as a one-block dictionary without its FST (most of `term_index`'s 47 bytes
   and of `key_index`'s 313 at one document) or a key range the block already bounds, comes by
   F23 with no reservation at the freeze. The footer's CRC still fails a flipped bit as damage,
   and a resealed input is refused either way. Without this, every such item must be reserved
   before the freeze.
4. **What stays resident is the reader's choice** (P15), not the format's. The pinned sections
   pass `small-indexes.md`'s 1 KB working target between 10 and 100 documents (574 and 1,660
   bytes), before the decoded table, so T10 states resident bytes at M0's sizes (F13-R30).

### Findings

27. **F13-R27. `termdict`'s options form: its parameters need validation, and the reader needs
    storage for them** (F8; F13-R19, F13-R20).
    - "A block's restart count stays bounded by its own bytes ... so the parameters need no cap"
      (format-13.md:556-557) does not hold for the reader K3 and K4 change. Today's block decoder
      validates against the constants (`count > max_block_entries`, a restart count equal to
      ⌈count / 32⌉, `termdict.hh:229`), steps its runs by the constant (342, 347, 401), and holds
      restart offsets in a fixed array of ⌈64 / 32⌉ = 2 (437). K4's 96 entries at restart 64 fit
      two restarts but fail the count check; K3's 128 need four at today's interval. A restart
      interval of 0 from a file divides by zero once it is a runtime value (229).
    - Flag bits other than 0 and 1 have no rule.
    - A reader without an option calls the form corruption (`termdict::corrupt`, 204), which F23
      rule 5 forbids for a later binary's structure. A flag the freeze does not write, such as
      P12's `terms` with K3's codec (547), therefore arrives under an F23 bit, not by the options
      form alone.
    - K3 relies on "at most 64 KiB of raw keys" (558-561), but the transform reads the raw length
      from the file.
    - Fix: the options decoder refuses a restart interval or a minimum of 0, a minimum above the
      maximum, and any flag bit but 0 and 1; the reader sizes its restart storage from the
      block's restart count, which the block's bytes bound, or the parameters take a stated cap
      that its storage holds; a raw length above 64 KiB is corruption before anything is
      decompressed, and a frame must decompress to exactly that length; a flag unused at the
      freeze comes later only under a bit. Before `termdict`'s change (first steps 3).

28. **F13-R28. The length record keys on the configuration, not on F20's mask** (F21, F20;
    F13-R25 4).
    - F21 keys the record "on the index's configured structures, which F20's mask records"
      (975-978). F20's mask is the configured bits together with "every bit any of its segments
      sets", and a bit leaves only "once no segment sets it" (946-955); segment files differ
      between replicas (`REPLICATION.md:86`). After a configuration drops `inverted`, which
      nothing forbids, one replica's mask can still hold the bit while another's has merged it
      away, and a digest keyed on the mask then differs between replicas with equal content:
      the honest peer whose view lags (calibration, eighth row). F20 names no record of the
      configured bits apart from that union.
    - The wording is mine (F13-R25 4: "key it on the index's mask"). Layers' R14 keys on the
      plugin set, which joins and never leaves (`layers.md`, revision 4's R14 and L8, at
      `ea04731`), so it held until F20 replaced the set with the mask.
    - Fix: F20 records the configured bits, which F23 rule 4's replicated operation sets, apart
      from the union with the segments' bits; F21 keys the record on the configured bits; and
      F20 states that the union serves only the readability checks (open, `install`). Or F4
      states that `inverted` never leaves a configuration. Before 7c-5, and F20's text before
      7c-6 (C10).

29. **F13-R29. PLAN's ratchets since revision 1: PISA's references and targeted runs** (F1
    items 3 to 6; T3 to T6 and T10; the joint selection's constraint).
    - PISA (PLAN.md:99-105, main `4692934`, committed after `78bd191`) is "the reference for
      query classes and postings bytes". F1 item 3 and the selection's step 2 constrain postings
      by keyed `-bp`'s `.doc` alone (154-157, 796-799), and item 4 projects from format 12's
      ratios against the fastest of Lucene, `-bp`, and Tantivy (160-167). The quick tier put
      format 12 at `db0a891` at 2.30× to 3.10× PISA on `TOP_10` to `TOP_1000`
      (`phase-3-ratchets.md:295-302`), where PISA's `TOP_10` rows are its exact top-k ones
      (309-322). No PISA index size is recorded on any branch (`git grep`), so a postings
      reference would have no raw file (calibration, third row), and `BENCHMARKS.md:58` still
      calls PISA "not a ratchet target".
    - Targeted runs (PLAN.md:109-122): work iterates on samples, one calibration names each
      row's fastest engine, one confirmation sized for minutes is the gate row, and "Full-corpus
      runs never gate a phase". F1 item 3 and T3 measure after a reordered full merge of the 5M
      index and T10 on the merged 5M index, the full corpus; item 4 projects "over the full gate
      query set, not the quick sample"; T4 runs "both query sets".
    - Fix:
      1. The postings constraint is the smaller of keyed `-bp`'s `.doc` and PISA's counterpart,
         its compressed documents and frequencies with its block-max data as the gate builds
         them, at the same corpus size, with its raw file and commit in `docs/perf/` (C9).
         BENCHMARKS' counterpart table places PISA's files.
      2. Item 4's projection takes each row's fastest engine from the one calibration (the
         query-gap design's E0, its D2), PISA included where its answers are exact, and E1's
         attribution covers PISA's lead. Part of that lead is format: block-max data over
         40-posting blocks and quantized scores (`phase-3-ratchets.md:309-313`), which are F12's
         partitions and P20's reserved structure, so "no part of a miss is attributed to the
         format" is checked against both.
      3. F1 items 3 to 6, T3 to T6, and T10 name their sample and their one confirmation as
         PLAN does, every reference calibrated at those sizes; the 5M figures stay Phase 3's
         record.
      4. Q7, below.
    - Before the joint selection's projection (step 1) and before the freeze.

30. **F13-R30. The many-index rows and small segments** (T5, T10; F4, F5, F6, F23;
    PLAN.md:183-192).
    - T5's indexing rows are "1 and 8 threads, keyed" (1164). PLAN now measures indexing per
      index on one thread, as throughput and CPU per document, beside the many-index aggregate
      row and the single-index row on the shared pool with no thread setting.
    - T10 states pinned bytes "at 1,000 indexes per node" (1169); PLAN has tens of thousands open
      per instance and millions per host.
    - A small segment's fixed bytes: "Small segments" above.
    - Fix:
      1. T5's guard is per index on one thread, the single-index row with no thread setting,
         and `perf/small-indexes`' M2 aggregate row, each ≤ 1.03 of format 12's time, with the
         ≥ rows as PLAN states them.
      2. T10 states resident and pinned bytes per open segment at 1, 10, 100, and 1,000
         documents, and per open index and open time at M0's 10,000, against format 12.
      3. F5's alignment is 8 bytes; F6's slot sentence goes; F15's columns stay 8-byte aligned.
      4. Bit 15, `compact`, enters F4's and F23's tables, with F23's rule 3 extended to the
         core's table.
      5. F4's third class merges into the second: T1 sets bits 13 to 15 as reserved bits, and
         bits 16 to 31 take the same classification in place of "outside the table".
    - Item 3 before 7c-1's canonical-layout check, or re-pinned in a later commit; the rest
      before the freeze.

31. **F13-R31. Query-gap revision 2's "What format 13 must record" is not folded** (calibration,
    fourth row).
    - Query-gap revision 2 (`query-gap.md:808-822` at `3cba768`, from its review's QG-R1, QG-R2,
      QG-R4, QG-R5, QG-R25, and C7) takes M18 out of format 13 and states M17's and M23's text.
      The lead's index records it ("M18 is out", main `docs/status/README.md:57`).
    - At `1cb2927` format 13 still lists M18 (F7 at 407-416, F10's second ask at 685-686, F11 at
      732 and 737, the selection's order at 804 and its row at 827); states M23's check as
      "their sum against the entry's last" (694-695), the form QG-R5 showed a block whose gaps
      sum to last − base + 2^32 passes; and cites the query-gap design at revision 0 (10, 92).
    - Fix:
      - F7's record serves M17 only, and a recorded preset outside QG-R1's preconditions (0 ≤ b
        ≤ 1, k1 ≥ 0, `min_normlen` ≥ 0, all finite) is corruption.
      - M18 leaves F10, F11, and the selection, citing QG-R4's five readmission conditions.
      - F11's M17 row: argmax-first at the segment's own clamped average under the recorded
        preset, the rest in `competitive()`'s order, ties by wdf; the bound with c = min(1,
        A / A') and its 2^−40 margin; stored frontiers only; `check()` verifying domination and
        argmax-first; QG-R2's copy rule.
      - F20 records the order preset as an index option, set by a replicated operation.
      - F10's M23 clause is the 64-bit sum S + n = last − base, with the base −1 before a term's
        first block, and the 25-bit lane comparison only where the block's header declares at
        most 25 bits; F22's corpus gains a block whose gaps wrap by 2^32 and one whose header
        declares 26 bits.
      - The design cites query-gap revision 2.
    - Before any M item enters the selection; the text before the freeze.

32. **F13-R32. The copy-merge proposal's disposition** (F1, F4, F23; "The proposed copy-merge
    items" above).
    - Fix: "Proposed: F13 candidates from research/indexing" (1088-1149) gives way to the
      rulings above. F4's and F23's tables gain bit 14, `postings_runs`: owner and requiring bit
      `inverted`, configured by an index option, no digest record (it is derived from postings).
      F1's P17 row (145) moves from "None" to bit 14. L6 gains items 4 and 5 as the copy merge's
      rules when its design is written, and P17 and the copy merge are one implementation
      (calibration, first row). Before the freeze.

33. **F13-R33. An empty key prefix turns F8's rules off** (F8; F13-R18; 7c-3, 7c-4).
    - From 7c-3 to 7c-4 every index carries `keys` (F13-R18), keyless ones with an empty prefix,
      which the code treats as keyless (`index.hh:1529, 1698` at `1cb2927`). F8's routing ("a
      term with the key prefix ... as a key leaf", 445-449) and its disjointness rule (439-442)
      would then route every term to `keys` and refuse every term; only the add-path rule says
      "in an index with a key prefix" (435-438). "In a layout without `inverted`, the prefix may
      be empty" (444) reads as if a layout with `inverted` always had one, which keyless indexes
      and the `log` profile contradict.
    - Fix: F8 states that routing, disjointness, and the add-path rule apply only when the
      index's prefix is nonempty, and that the prefix is empty exactly in a keyless index or a
      layout without `inverted`; 7c-3's tests cover term queries and wildcards over a keyless
      index. Before 7c-3.

34. **F13-R34. Text.**
    - The rebase (1193-1195): layers has folded R12 to R19 (`ea04731`, `d88e85b`), with code in
      `reader.hh`, `index.hh`, and `merge.hh`, so "is folding its R12" changes. When the lead
      names the base, 7c's lines and every `707066b` citation are re-read there, since R16's
      presence before the readers is already on layers.
    - The rebase table (1212-1224) gains `b2f7600`: dropped, since the base holds `adb329e`.

### Owner question

7. **Q7. PISA and the carried rows.** PLAN makes PISA the reference for postings bytes and query
   classes (PLAN.md:99-105); the carried rows' original targets were set against Phase 3's set
   (129-142). Do PISA's references apply to the rows that close in format 13, or from Phase 4's
   own rows on? Recommended: postings bytes against PISA now, one exact measurement, since the
   format freezes once and Phase 6 asks every section for 0.9× its reference; queries as PLAN
   states them on PISA's exact rows, with E1's attribution naming the format's share of PISA's
   lead. `BENCHMARKS.md:58` changes with the answer (the lead's).

### New blocking items

- F13-R33 before 7c-3; F13-R28 before 7c-5.
- F13-R27 before `termdict`'s options change (first steps 3).
- F13-R30 item 3 before 7c-1's canonical-layout check, or re-pinned later.
- F13-R29 before the joint selection's projection, and before the freeze (Q7).
- F13-R31 before any M item enters the selection.
- Before the freeze: F13-R22's row, F13-R31's text, F13-R30's other items, F13-R32, and
  F13-R34.

### Conditions

- C1 closed; C2 carries; C3 to C7 closed; C8 open (the tip's run 39098644329); C9 carries, with
  PISA's postings figure (F13-R29).
- **C10. Two calibration rows, for the lead,** both my misses:
  1. F13-R25 4 keyed the digest on the manifest's mask, which holds each replica's own
     segments' bits (F13-R28). The check: for every digest or verdict input a fix names,
     confirm that replicas compute it from replicated state (the log, the configuration),
     never from which segments a replica holds.
  2. F13-R22 read `limits.md:36`'s Lucene column as strata's and had its owed row dropped. The
     check: quote the row and column of every table cell a fix relies on.
- **C11.** F22's rule that nothing compares segment bytes across binaries (F13-R24) binds the
  designs still to come: D6's object-storage segments and P20's read replicas compare digests,
  or each file against its own CRCs, never segment bytes or content hashes across binaries.
- **C12.** Bit 14's release lands only with SYS-6 evidence: merge time against a re-encoding
  merge brought toward its bound, at sources of 1,000 to 100,000 documents; bytes at each; T4's
  guard over a copy-merged index, whose short blocks decode as tails (AUT-R13's profile prices
  them); and P17 as the same path.

### In one line each

- Verdict: APPROVE WITH CONDITIONS; 7c may start on the lead's base, F13-R33 before 7c-3 and
  F13-R28 before 7c-5.
- F13-R18, R19, R20, R21, R23, R24, R25, and R26: closed. F13-R22: partially closed
  (`limits.md:35-36`). `b2f7600`: main's fix, accepted.
- Copy merges: items 1 to 3 wait under bit 14, item 1 replaced by a run table; items 4 and 5 are
  merge rules with no format change.
- Small segments: 8-byte alignment now; bit 15 reserved for the compact profile; F4's unknown
  bits become unsupported ones.
- F13-R27: `termdict`'s options validated, the reader's restart storage, K3's raw length.
- F13-R28: the length record keys on the configured bits.
- F13-R29: PISA's references and targeted runs in the freeze criteria and rows; Q7.
- F13-R30: T5 and T10 for the many-index rows; alignment, bit 15, and F4.
- F13-R31: query-gap revision 2's format-13 text: M18 out, M17's rules, M23's 64-bit sum.
- F13-R32: the proposal gives way to the rulings; bit 14; F1's P17 row.
- F13-R33: an empty prefix turns F8's rules off.
- F13-R34: text.
- New conditions: C10 (two calibration rows), C11 (no cross-binary byte comparison later), C12
  (bit 14's evidence).

Not run: no benchmark or timing (the host's light holds); no full local suite under sanitizers,
only `bisection_test` under TSan and under ASan with UBSan; no fuzzing; `research_mergecost` not
rerun, and no copy-merged segment built; no VM, so neither the keyed `-bp` sizes nor PISA's index
was read; the query-gap and layers designs read only where they address format 13; Lucene's,
Tantivy's, and PISA's source; no Xapian source and not `sortable_serialise`.

## Third re-check (step 7c)

- Date: 2026-10-18.
- Reviewed:
  - `git log 870494d..f2c19c6` on `phase4/format13`, 22 commits on layers' `d508158`: step 7c's
    code (`b8d9ac9` 7c-1, `e6b2144` 7c-2, `86225aa` 7c-3, `da6e9a6` 7c-4, `bd69714` 7c-5,
    `f92e80b` 7c-6, `2fe7e85` 7c-7, `ae42746` T7, and the sanitizer fixes `d00e587` and
    `82060cc`); the second re-check's findings in the design (`232c319` F13-R30, `b28517f` R33,
    `adfe6d4` R28, `53f0ecc` R34, `6e247d2` R31, `8b8b6a5` R32 with bit 14 in `format.hh`,
    `5fa09db` R27, `55801fe` R22, `f7b40f6` R29); the ledger (`f2c19c6`). Every diff, read at
    `f2c19c6` in a detached worktree (`strata-wt/review-format13-7c`); every line cited below is
    at `f2c19c6`.
  - The fold pushed during this review: `origin/phase4/format13` is `23716c5`, whose tree is
    `f2c19c6`'s (`cf085de` both), so every ruling here holds for it. Its code commits are
    `b8d9ac9`, `e6b2144`, `60b0819` (7c-3), `38a926a` (7c-4), `2bd28e7` (7c-5), `ebbd012` (7c-6),
    `8912b5c` (7c-7), `596e675` (T7), and `8732771` (bit 14); the ledger's old-to-new map is
    `be1aee9`.
- Checked against: `calibration.md` at main `088a64f`, all ten rows (applied where a finding
  names them); PLAN's "What waits" and "Every gate"; the design at `f2c19c6` (F2 to F9, F20,
  F21, F23, T1, T7, "Step 7c"); layers' L14 7c (`layers.md:903`); this review's F13-R1 to
  F13-R34 and C1 to C12; the ledger's criteria and deviations (`format13.md:6-24, 51, 56-57,
  76-79`).
- Checked by:
  - Builds of `f2c19c6` with Homebrew clang 23.1.2 on macOS x86_64: RelWithDebInfo (`-O2 -g`),
    and Debug with `-fsanitize=address,undefined -fno-sanitize-recover=undefined` and libc++'s
    extensive hardening.
  - The full suite in the release build: 36 of 36 pass in 171 s at `--parallel 4` (format13
    0.78 s, fixtures 0.76 s, media 1.78 s, lifecycle 139 s).
  - Under ASan and UBSan, the 16 tests 7c touches pass with no report in 918 s at
    `--parallel 4` (`asan-subset.log`: concepts, segment, docids, reorder, files, format13,
    fixtures, merge, merge_oracle, churn, media, reclaim, index, background_merge, and both fuzz
    replays); lifecycle, search, and the mapped variants ran only in CI.
  - Probes in `strata-wt/format13/.scratch/review-format13-r3/`, each built against `f2c19c6`
    by `build_probe.sh`, each with its `.out`: `prefix_align.py` (an independent parser with its
    own bitwise CRC-32C), `probe_classify.cc`, `probe_absent.cc` (under ASan and UBSan),
    `probe_t7.cc`, `probe_keys_check.cc`, `probe_index.cc`, `probe_text.sh`, and two mutants of
    the headers made by `make_mutants.py` (the bit check moved before the table; an open that
    touches `lengths`).
  - The format-12 refusal fixtures against `d508158`'s blobs: all 11 identical.
  - CI through `gh`. Before the fold: run 39112211269 at `b28517f` (7c-1's code) and
    39116928553 at `e6b2144` pass all eight jobs; 39116930589 (`86225aa`), 39116858962
    (`da6e9a6`), 39116860672 (`bd69714`), 39118107502 (`f92e80b`), and 39121686252 (`f7b40f6`)
    pass the other seven and fail ASan; 39126687560 at `82060cc` passes all eight (ASan
    38 min, TSan 32 min). The folded series (runs 39130760664 to 39130775497, and 39130677071 at
    `23716c5`):
    at 07:11 PDT the six non-sanitizer jobs pass at 7c-3 to 7c-6 and at `23716c5`, TSan passes
    at 7c-3 to 7c-7 and at `23716c5`, and the rest run, ASan on every commit.
  - `HEAD` against `origin`: the format13 worktree is one ledger commit ahead (`be1aee9`).
  - Not run: no benchmark or timing; no fuzzing; TSan, Linux, and FreeBSD only through CI; the
    other 20 tests under ASan and UBSan locally (the whole suite was started and stopped when a
    strict hold began 8 s later; CI's run at `82060cc`, the same tree, is that evidence);
    `k8_steps.sh`, a per-step `fixtures_test` build, stopped at its first build by a strict hold
    (the folded series' CI is that evidence). My runs overlapped the start of three of
    `small-indexes`' strict holds on the gate VM: the ASan build's last 7 s and a 3 s
    `format13_test` at 06:31 (`si-m2b`), a `fixtures_test` build from 06:43:38 for about 50 s
    (`si-m2b2`, whose rerun `si-m2b3` notes a drifted anchor), and 30 s of the ASan suite at
    06:49 (`si-m2b3`). No VM or Xapiand cluster; Lucene's, Tantivy's, and PISA's source; no Xapian
    or Xapiand GPL source.

### Verdict

**APPROVE WITH CONDITIONS: step 7c builds format 13's framing as the design states it, and
format 12's layout code is gone. One defect blocks landing: a lone segment that a later
format-13 release writes is called corruption, not unsupported, because the reader decodes the
table before it reads the structure bits (F13-R35). K9's claim fails for a segment without norms
(F13-R36). The five deviations are accepted, the fifth with a correction. Format 13 lands when
F13-R35 is fixed and F1's freeze criteria hold.**

What holds, and reproduces:

- **The frozen prefix.** Bytes 0 to 15 are `STRSEG01`, version 13 little-endian, and the CRC-32C
  of bytes 0 to 11, `0x4ff2df7b`, in all nine format-13 fixtures by an independent parser
  (`prefix_align.out`). `read_version`'s four rules (`format.hh:232-241`) run before any footer
  in `core_segment`, `load_segment`, and `written_segment` (`reader.hh:126`;
  `index.hh:2107, 822`). Every bit of bytes 8 to 15 flipped is damage, versions 12 and 14 with a
  good CRC are unsupported, and format 12's zero word is unsupported
  (`format13_test.cc:228-271`).
- **The 8-byte canonical layout.** `section_alignment` is 8 (`format.hh:90`); the header entry
  sits at 0 with 36 bytes, each later entry at the previous one's aligned end in rank order, and
  the table at the last aligned end (`format.hh:359-371`). Every fixture is canonical, with 17
  to 55 bytes of padding against F5's bound of 84 (`prefix_align.out`). Header 36 bytes and
  footer 56 match F6 field by field (`format.hh:72-73, 316-327`).
- **Format 12 refused.** The eight format-12 segments and the three version-1 files under
  `test/fixtures/format12/` are `d508158`'s blobs; each segment is `unsupported_segment`
  (`format13_test.cc:264-270`) and each file `unsupported_index` (`files_test.cc:214-216`).
- **One digest change.** `git log d508158..f2c19c6 -- test/fixtures/digest.txt` names `bd69714`
  alone (`2bd28e7` after the fold). `fixtures_test` pins the digest of a fixed script to
  `digest.txt` (`fixtures_test.cc:156-203`) and passes on every non-sanitizer CI job of 7c-1 to
  7c-6 and at `f7b40f6`, which holds 7c-7 and T7.
- **Absent structures.** All 12 buildable combinations of `inverted`, `norms`, `keys`, and
  `stored`, at 0, 1, and 300 documents, through every reader call, `check_segment`, a key leaf,
  and a term query, and 144 merges across unlike profiles: no failure and no sanitizer report
  (`probe_absent.out`).
- **7c-7.** `docids_section`, `search_layout_12`, `document_12`, and `reader_of<docids_section>`
  are deleted; no format-12 identifier remains in `include/`, `tools/`, or `fuzz/` but the
  retired `pk_filter` row (`format.hh:106, 169`).
- **T7 has teeth.** A mutant whose open touches `lengths`' first byte makes `format13_test` die
  with SIGBUS (exit 138).
- **F20 at the index.** Install with segment versions up to 14 is `unsupported_index`; a
  manifest-named segment that sets bit 8 is `corrupt_segment` when the mask lacks the bit and
  `unsupported_index` when it holds it (`probe_index.out`).

What blocks:

- F13-R35: classify a segment's structure bits before its table, before format 13 lands.

### Status

- **F13-R1: closed.** `read_version` runs first in all three opens (above); a manifest-named
  unsupported segment is corruption (`index.hh:2086-2091`; `format13_test.cc:520-560`); the real
  format-12 files are refused (above); `media_test` sweeps bytes 0 to 15 of every segment
  (`media_test.cc:101-108`).
- **F13-R2: closed for exact terms.** The planner routes a key-prefixed term to a key leaf
  (`searcher.hh:103-117`); `keys_plugin::depends_on` is empty (`reader.hh:507`); stage 1
  (`reader.hh:237, 496-501`; `index.hh:1563`), the builder (`writer.hh:394-396`), and `check()`
  (`check.hh:34`) keep keys out of the dictionary; `keys` holds the key's bytes as given
  (`writer.hh:678-681`). Expansion over `keys` waits for automata (deviation 4).
- **F13-R3: closed,** with F13-R35 against its classification order. One table
  (`format.hh:157-180`) replaces `section_rule`; each plugin owns a mask (`reader.hh:261-262,
  503-505, 591-593`); `holds<P>()` reads the footer's bit before any reader opens
  (`reader.hh:851, 870-878, 889-893`); the free-and-absent cases are tested
  (`format13_test.cc:562-657`).
- **F13-R4: closed.** Pinned sections are held and written by rank (`section_io.hh:306-321`),
  `inverted`'s 20 bytes of statistics lead `term_index` (`writer.hh:326`; `reader.hh:273-276`),
  and the layout is canonical (above). F13-R36 is against T7's claim for one profile.
- **F13-R5: closed** (above, and K8 below).
- **F13-R6, F13-R7, F13-R8: closed in the design.** `keysim`'s and `poststats`' fixes are the
  first steps' (`format13.md:73`), and F10 to F13 follow 7c.
- **F13-R9: closed.** The builder refuses a nonzero version without a key (`writer.hh:677`),
  `check()` refuses one (`check.hh:44-53`), and the minimum's rule (`writer.hh:700-707`) holds
  at width 2 near 2^64 − 1 (`format13_test.cc:674-685`).
- **F13-R10: closed** (`writer.hh:269, 394-396, 409`; `format13_test.cc:817-829`). The seam
  inventory stays Phase 7's.
- **F13-R11: closed,** with `merge_options::key_prefix` kept as the backstop's (deviation 1);
  the text is F13-R38.
- **F13-R12: closed** (`format.hh:113-117, 124`: kinds 18 to 22, bits 8 to 12).
- **F13-R13, F13-R14, F13-R15: closed in the design.** Values, points, term lists, and the index
  sort have no code yet; the libraries' READMEs state their frozen forms before the freeze;
  T1's code is K1's.
- **F13-R16: closed.** CI's ASan limit is 120 minutes (`.github/workflows/ci.yml:18`), and the
  job takes 38 to 69 minutes across this series.
- **F13-R17: closed.**
- **F13-R18: closed.** A keyless index is configured without `keys` from 7c-5
  (`index.hh:231`); a keyless document's version is 0 in the verdict, the buffer, and every
  segment (`index.hh:1615`; `format13_test.cc:707-733`); T2's schedules run over a keyless index
  too (`lifecycle_test.cc:143`).
- **F13-R19, F13-R20, F13-R21, F13-R24, F13-R27: closed in the design**
  (`format-13.md:585-616, 449-458, 1085-1102`); `termdict`'s change is the first steps'.
- **F13-R22: closed in the design** (`format-13.md:130`); the `limits.md` edit is the lead's,
  before the freeze.
- **F13-R23: closed** (`format.hh:124, 133`; `format13_test.cc:380`).
- **F13-R25: closed except item 3,** which waits for D16 (deviation 3). Item 1: the core passes
  the index's prefix to stage 1 (`index.hh:1512`), the flush (`index.hh:1664`), the merge
  (`index.hh:885, 1111`), `check()` (`index.hh:584`), and the planner (`searcher.hh:134-135`).
  Item 2: `reader.hh:496-501`; `format13_test.cc:750-753`. Item 4: `index.hh:539, 552, 558`.
  Item 5: `format13_test.cc:273-306`, with F13-R36. Item 6: `reader.hh:845, 851, 889-893`.
  Item 7: `writer.hh:700-707`.
- **F13-R26: closed.**
- **F13-R28: closed.** The manifest records the configured bits apart from the mask
  (`files.hh:79-84, 230`); the mask is recomputed from them and every segment's bits at each
  commit (`index.hh:1796-1803`); the digest keys on the configured bits (`index.hh:539-558`);
  install adopts the source's (`index.hh:672`).
- **F13-R29, F13-R31: closed in the design** (`format-13.md:177-208, 1211-1213`;
  `format-13.md:449-458, 745, 753-759, 786-800, 1035-1037`). BENCHMARKS' PISA line and C9's raw
  files stay owed.
- **F13-R30: closed,** with F13-R35 against bit 15's rule: 8-byte alignment (above), bit 15
  reserved (`format.hh:124-125`), unlisted bits unsupported (`format.hh:382-384`;
  `format13_test.cc:384-390`).
- **F13-R32: closed** (`format-13.md:1171-1198`; `format.hh:125, 132`).
- **F13-R33: closed for term queries** (`searcher.hh:108`; `reader.hh:237, 498`; `check.hh:34`;
  `writer.hh:395`; `format13_test.cc:831-849`). Its wildcard tests wait for automata (deviation
  4).
- **F13-R34: partially closed.** The base and the dropped cherry-pick are in the design
  (`format-13.md:1242-1251, 1278`). The `707066b` citations are not re-read at `d508158`
  (`format13.md:72`), and the header still cites `707066b` as the base (`format-13.md:29-31`).
- **C1: closed. C2: open:** the landing series is the folded one, and its per-commit runs were
  still running. **C3 to C7: closed. C8: closed** (F13-R16). **C9: carries. C10: closed**
  (main's `calibration.md:16-17`). **C11 and C12: carry.**

### The deviations (`format13.md:51, 56-57, 78`)

1. **`merge_options::key_prefix` stays: accepted.** It is the one channel from the index into a
   merge (`index.hh:885, 1111` to `merge.hh:237, 352` to `merge_context::key_prefix`,
   `merge.hh:68, 83`), it now means only the backstop's prefix, and keys reach the merge through
   the keys reader's scan from the empty key (`merge.hh:262-272`), so F13-R11's concern, keys
   found by prefix, is gone. The design and layers still say "deleted" (F13-R38).
2. **`index_sort` is not a readable bit yet: accepted.** F4 makes it readable "since F18's
   reader validates it" (`format-13.md:339-342`); with no such reader it is a bit this binary
   lacks, unsupported in a lone segment and refused by the mask in an index (`reader.hh:161`;
   `format.hh:382-384`; `format13_test.cc:391`). F18's commit adds it (C14).
3. **F13-R25 item 3 waits for D16: accepted.** Nothing at `f2c19c6` evaluates a predicate over
   buffered documents: there is no delete by query and no query path over the buffer
   (`aggregate.hh:414`'s predicate is over aggregation documents), and key operations reach
   buffered copies through the key map (`index.hh:1561-1570`). The obligation stays in F8
   (`format-13.md:499-502`) and layers' R13; D16's reviews check it (C14).
4. **F13-R33's wildcard tests wait for automata: accepted.** The base has no multi-term
   expansion in `include/` or `tools/`, and term queries over a keyless index are tested
   (`format13_test.cc:831-849`). Automata adds the tests when it rebases onto format 13 (C14).
5. **The version-range check cannot be exercised: accepted with a correction.** Its outcome is
   exercised: a version-14 segment with a good prefix CRC in a [13, 13] manifest is
   `corrupt_segment` (`format13_test.cc:520-560`), through `read_version`'s refusal and
   `load_segment`'s conversion (`index.hh:2086-2091, 2107`). Only the explicit comparison
   (`index.hh:2111-2115`) is unreachable: `decode_manifest` refuses a minimum above the maximum
   (`files.hh:230`), and `check_readable` confines a manifest with segments to [13, 13]
   (`index.hh:2064`), so every segment that reaches it is version 13 and in range. It becomes
   reachable when `min_read_version` is below `version`, in format 14, whose first commit owes
   its test (C14). K11's evidence should cite the version-14 case as its range case (F13-R38).

### The ticked criteria

- **K6: met.** The sweep faults each of bytes 0 to 15 of every segment, both ways, transient and
  sticky (`media_test.cc:101-108`); it passes here and on all eight CI jobs at `b28517f` and
  `82060cc`.
- **K8: met.** `digest.txt` changes in 7c-5 alone, and `fixtures_test` passes on every
  non-sanitizer job of 7c-1 to 7c-6 and at `f7b40f6` (above). The evidence names `bd69714`,
  whose ASan job failed (run 39116860672); in the folded series `2bd28e7`'s run carries it (C2).
- **K9: partly met.** T7 passes and fails when the open touches `lengths` (above), and the
  profiles an index writes today open without a fault (`probe_t7.out`). A segment without
  `norms` reads `lengths` at open (F13-R36). The tick should name the profiles it covers.
- **K11: met.** `format13_test.cc:433-518` and `files_test.cc:191-238`, and `probe_index`'s
  install case, which the suite lacks; the range case is deviation 5's test.
- **K13: met,** unchanged since the first re-check.
- K1 to K5, K7, K10, K12, and K14 are unticked, as the repository shows. K1 gains F13-R35's
  cases.

### Findings

35. **F13-R35. A lone segment from a later format-13 release is corruption, not unsupported**
    (F4, F23 rules 3 and 5; T1).
    - `core_segment` decodes the table before it classifies the footer's bits
      (`reader.hh:127-130`). `decode_toc` checks each kind and rank against this binary's table
      (`format.hh:350, 359-360`), where the reserved kinds are unranked
      (`format.hh:173, 176-180`) and kinds above 22 are unknown, so a section a later release
      adds fails before its bit is read.
    - `probe_classify.out`, every checksum good, each case what a later release writes:
      - bit 8 with `pair_summaries` after `lengths`, F3's placement for an `inverted` regions
        kind: corrupt ("section order");
      - bit 12 with `key_hash` after `keys`: corrupt ("section order");
      - bit 5 with `points` after `stored` (F16: its own plugin, appended): corrupt ("section
        order");
      - bit 16 with a kind-23 section: corrupt ("unknown section kind");
      - bit 15 with a table form this binary cannot parse: corrupt ("region size"), where F23
        rule 3 says "a reader reads the bit before the table".
      The same bits with no section are unsupported, and that is all T1's bit loop sets
      (`format13_test.cc:384-390`), so T1 passes without the case F23 rule 5 is about.
    - In an index the mask refuses first (`index.hh:1992, 2060-2063`), so today only lone opens
      see it: the tools, the fuzzers, and any later single-file path. But the reader frozen with
      format 13 is the one every later release's segment meets; F2 says the prefix lets "each
      binary tell a newer segment from a damaged one without its manifest" (`format-13.md:256-257`).
    - Fix: `core_segment` checks the requirements and the lacked bits right after
      `decode_footer`, before `decode_toc`; presence stays after the table. The 4-line mutant
      (`mutant-classify`) gives unsupported in all five cases and passes `format13_test`
      (`probe_classify_mutant.out`, `format13_mutant_classify.out`). T1 gains the five cases.
      F4's presence sentence (`format-13.md:364-366`) applies "in a segment whose bits this
      binary reads". Before format 13 lands; cheapest now, in 7c-1's code.

36. **F13-R36. T7 does not hold for a segment without norms** (F4, F7; T7, K9).
    - With `inverted` set and `norms` clear, the reader derives every norm from `lengths` at
      open (`reader.hh:286-292`): the open reads `lengths` whole and allocates doc_count bytes of
      heap. `probe_t7.out`, T7's procedure: with norms, and the document profile, no fault;
      without norms the open faults (SIGBUS) on `lengths`' pages.
    - T7 and K9 state "no byte of `lengths`" for every fresh open (`format-13.md:1215`;
      `format13.md:16`), F7 says "an open reads none of it" (`format-13.md:459-461`), and F4
      defines the profile, `norms` absent with its bit clear (`format-13.md:382-384`; D2's
      optional norms). No index writes one today: `index.hh:231` always configures `norms`.
    - Fix: derive the norms on first use, under a once-flag as `lengths` has, or state the
      exception in F7, T7, and K9 with its cost (an open's read of `lengths`, and doc_count heap
      bytes per segment in T10). T7 gains the profile. Before the freeze.

37. **F13-R37. `check()` accepts inconsistent `keys` sections, and the fuzzer never decodes
    their blocks** (F8; K2, T2).
    - `probe_keys_check.out`, checksums resealed as `fuzz_segment`'s inputs are:
      - two keys naming one ordinal, versions 0: accepted; `find("Q0")` and `find("Q1")` both
        give ordinal 0;
      - `key_index`'s maximum below the last key: accepted; `find("Q9")` misses a held key;
      - a filter built over other keys: accepted; `find` misses 10 of 10 held keys.
    - `check_segment` walks the keys cursor (`check.hh:44-53`), which checks order and count
      only (`keys.hh:161-174`). By the code, not probed at the index: `lookup_key` skips a
      segment whose `may_hold` is false (`index.hh:1401-1411`), so an upsert of a missed key
      writes a second live copy, and the digest then throws `std::logic_error`
      (`index.hh:532`).
    - `fuzz_segment`'s `exercise` calls only `version` and `may_hold` on `keys`
      (`fuzz_segment.cc:109, 111`), never `find`, `scan`, or the cursor, so the blocks of the new
      `keys` section get no fuzzing at all.
    - Fix: `check_segment` refuses an ordinal named twice, a range other than the first and last
      key, and a key `may_hold` rejects; `exercise` calls `find` on sampled keys, `scan` from a
      few bounds, and walks the cursor. Before the freeze, with `keys`' corpus (K2).

38. **F13-R38. Text the step left owed** (`probe_text.out` prints each line).
    - `merge_options::key_prefix` stays (deviation 1); `format-13.md:50, 1271, 1300-1301` and
      `layers.md:903` still say it is deleted.
    - F3 ranks `values`, `term_lists`, and `index_sort` 7, 8, and 11 (`format-13.md:278-282`);
      `format.hh:172-175` leaves them unranked until their plugins land, which the ledger's
      deviations should list.
    - The 7c-6 deviation says the range check cannot be exercised (`format13.md:51`); its
      outcome is (deviation 5), and K11's evidence should cite that test.
    - Comments still describe format 12: `fixtures_test.cc:1, 143`, `segment_test.cc:307, 467`,
      `media_test.cc:140`, and `strata_engine.cc:619`.
    - `index.hh:230` says "Every structure of the layout", and line 231 hard-codes the search
      layout's bits, wrong for `basic_index<document_layout>` once L12's profile gate builds it.
    - F13-R34's citation re-read (above).

39. **F13-R39. A keyless index drops an add's key without a word** (F8, F13-R33).
    - With an empty prefix `key_of` returns no key (`reader.hh:498`), so an add that carries
      `document::key` is accepted and its key discarded; the suite's own case is the probe
      (`format13_test.cc:838-847` asserts the discard).
      F8 says only that such a key "is not checked against" the prefix
      (`format-13.md:481-482`).
    - Refusing it as `invalid` in stage 1 is one condition, deterministic on every replica, and
      makes a host that sends keys to a keyless index fail loudly instead of losing them.
      Recommended; the lead's call, recorded in F8. Before the freeze.

### New blocking items

- F13-R35, before format 13 lands.
- Before the freeze: F13-R36, F13-R37, F13-R38, and F13-R39's ruling.

### Conditions

- **C2 carries:** every code commit of the folded landing series, the nine above, green on all
  eight jobs; runs 39130760664 to 39130775497 and 39130677071 were running.
- **C9, C11, and C12 carry.**
- **C13.** Before the freeze, the manifest's `slots` reference is read (D3) or refused nonzero,
  as the values generation is (`files.hh:225, 248`); today it is decoded, carried, and never
  checked.
- **C14. The deviations' follow-ups.** F18's commit adds `index_sort` to `core_segment::reads`
  with rank 11 and its T1 cases; D16's design and code review check the buffer path's key leaf
  (F13-R25 item 3); automata, rebased onto format 13, tests wildcards over a keyless index and
  over the key prefix (AUT-R4's keys-equivalence row); format 14's first commit tests
  `load_segment`'s explicit range comparison.
- **C15. A calibration row, for the lead.** My second re-check accepted T1's reserved-bit cases
  as bits set on today's table (F13-R30 item 5), and the first accepted the same for unlisted
  bits (F13-R3 fix 3), without asking what a later release writes. The check: for every rule of
  the form "a later binary's X is classified Y", build X as that binary writes it, its sections
  placed and its tables changed, and open it with this binary.

### In one line each

- Verdict: APPROVE WITH CONDITIONS; F13-R35 blocks landing.
- F13-R1, R3 to R5, R9 to R12, R16 to R18, R23, R26, R28, R30, R32: closed (R3 and R30 with
  F13-R35 against them).
- F13-R6 to R8, R13 to R15, R19 to R22, R24, R27, R29, R31: closed in the design; their code
  follows 7c.
- F13-R2, R25, R33: closed except what deviations 3 and 4 defer. F13-R34: partially closed.
- Deviations 1 to 4: accepted. Deviation 5: accepted with a correction.
- K6, K8, K11, K13: met. K9: partly met (F13-R36).
- F13-R35: classify a segment's bits before its table.
- F13-R36: T7 and K9 for a segment without norms.
- F13-R37: `check()` over inconsistent `keys` sections; the fuzzer never decodes `keys`' blocks.
- F13-R38: the text the step left owed.
- F13-R39: a keyless index drops an add's key.
- C2 carries (the folded series' CI); C13 (`slots`), C14 (the deviations' follow-ups), and C15
  (a calibration row) are new.

Not run: no benchmark or timing; no fuzzing; TSan, Linux, and FreeBSD only through CI; 20 of the
36 tests under ASan and UBSan locally (lifecycle, search, and the mapped variants among them;
CI's run at `82060cc`, the same tree, is the evidence); the per-step `fixtures_test` builds
(stopped by a strict hold; the folded series' CI is the evidence); the folded series' CI, still
running when this was written; no VM or Xapiand cluster; Lucene's, Tantivy's, and PISA's source;
no Xapian or Xapiand GPL source.

## Fourth re-check and freeze readiness

- Date: 2026-10-18.
- Reviewed:
  - `git log 0100465..bb1e90a` on `phase4/format13`, ten commits: CR12 `b7e56b6`, CR4 `ee18c40`,
    CR5 `b7b758d`, CR6 `b88ee36`, CR2 `953d907`, CR7 and CR8 `653d5c7`, CR11 `967883c`, and the
    ledger (`d26a26a`, `a681347`, `bb1e90a`). With them, my third re-check's findings in
    `b9cf1b3..aa5fdbd`: F13-R35 `1c76aff`, F13-R36 `5f9d297`, F13-R37 `3cf6bfa`, F13-R39 `a83b06e`,
    F13-R38 `aa5fdbd`. Every diff, read at `bb1e90a` in a detached worktree
    (`strata-wt/review-format13`), which equals `origin/phase4/format13`; every line cited below is
    at `bb1e90a` unless another commit is named.
  - The code review, `docs/reviews/format-13-code.md` at `0100465` (CR1 to CR12).
  - For the freeze: `Kronuz/cpp-libraries` `docs/research/foundations-vision.md` at `8814481`;
    `design/durability-modes` at `1fa9336`, its ledger (`docs/status/durability-modes.md`, cited
    as "durability ledger") and draft design (`docs/design/durability-modes.md`, D1 to D11);
    `perf/encode-once` at `dbf49be`, `docs/perf/encode-once.md`; `design/query-gap` at `e272ec2`,
    its ledger (`docs/status/query-gap.md`, cited as "query-gap ledger": the lead's rulings and
    the quick-tier calibration) and its review's QG-R27 to QG-R31 (`query-gap-design.md`); main
    at `734949e` (PLAN, `track-c.md` D18, `calibration.md`). Each branch read equals its `origin`
    except `design/durability-modes`, whose local `c6df449` merges main and changes neither
    durability file.
- Checked against: `calibration.md` at main `734949e`, all ten rows; this review's F13-R1 to
  F13-R39 and C1 to C15; the code review's CR1 to CR12; F1's freeze criteria; the ledger's "Before
  the freeze" (`format13.md:107-117`).
- Checked by:
  - CI through `gh`: the twelve runs the ledger names (`format13.md:96`), each completed with
    success on 8 of 8 jobs at its own commit, 39160473341 at `1c76aff` through 39163730187 at
    `d26a26a`, whose code is `967883c`'s.
  - Builds of `bb1e90a` with Homebrew clang 23.1.2 on macOS 15.6, x86_64: RelWithDebInfo
    (`-O2 -g`), and Debug with `-fsanitize=address,undefined -fno-sanitize-recover=undefined` and
    libc++'s extensive hardening.
  - The full suite in the release build: 36 of 36 pass in 474 s at `--parallel 4` (beside the
    ASan build; format13 1.6 s, fixtures 1.2 s, fuzz_segment_replay 198 s).
  - The full suite under ASan and UBSan with leak detection, at `--parallel 4`: 26 of 36 pass with
    no report and none fails, among them every test whose file the fixes change (format13,
    segment, files, fixtures, reorder, media) but the segment fuzz replay, whose new seeds ran
    under ASan on their own (below), and index, merge, docids, and background_merge. I stopped
    the run at 23:11 for a strict hold (below): lifecycle, lifecycle_reordered, search, and
    index_mapped unfinished, and the other mapped variants, engine_query, and both fuzz replays
    never started (CI's `linux-asan-ubsan` ran all 36 at every commit in scope).
  - Every build and run used `nice -n 19`: the two builds at eight jobs after `quiet check` exited
    0 (the ASan build started on the release build's check, minutes old, without its own), the
    suites at four, under light holds from 22:44. One strict hold, `scale-threads-mac`, began at
    23:07:13 during the ASan suite and the last half minute of CR2's ASan replay; the suite's four
    processes ran into it for 3 min 20 s before I paused them (SIGSTOP at 23:10:33) and then
    stopped them (23:11).
  - Probes in `strata-wt/review-format13/.scratch/review-format13-r4/`, each with its `.out`:
    `probe_identity.cc` (F13-R41), `probe_manifest_later.cc` (F13-R42), `probe_ram.cc` under ASan
    and UBSan (memory-resident segments), `crc_prefix.py`, `fxdiff.py`, `hdrcrc.py`, and
    `residue.py` (the prefix and CR11's re-pin, with an independent CRC-32C), `cr2_seeds.py` over
    CR2's 96 seeds replayed with `STRATA_FUZZ_VERDICTS=1` (`cr2-verdicts.out`; under ASan and
    UBSan, the same 96 verdicts and no report, `cr2-asan.out`), and
    `git merge-tree` of `bb1e90a` onto main `734949e` (`merge_main.out`).
  - Not run: no benchmark, timing, or A/B; 10 of the 36 tests under ASan and UBSan locally (the
    hold above); no libFuzzer run; TSan, Linux, FreeBSD, and arm64 only through CI; the code
    review's mutation harness (deleted; its reviewer rebuilds it); no
    rebase, only `merge-tree`; no VM, so the calibration's raw files (query-gap ledger:75-77) were
    not read and its figures are the ledger's; `strata_engine verify` and `parity`; Lucene's,
    Tantivy's, and PISA's source (their segment ids from their documentation); no Xapian or
    Xapiand GPL source.

### Verdict

**APPROVE WITH CONDITIONS for landing step 7c and its fixes on main under the pre-freeze
version; NOT READY TO FREEZE.** F13-R35, the item that blocked landing, is closed, as are F13-R36
to F13-R39 and the code review's CR2, CR4 to CR7, CR11, and CR12; CR8 is closed but for its
fourth item. CR11 changes my third re-check's landing rule: every later build refuses a
pre-freeze segment as unsupported (F2 rule 2), and its manifest by the segments' range (C17 item
2 keeps that true across manifest changes), so landing freezes nothing, and the freeze is the
commit that writes 13. Landing waits for the rebase onto main and C16. The freeze waits for the
pre-freeze list: of the nine needs, five have their place now (memory-resident segments, TTL, HLC
per version, doc values and sketches, and vectors with F13-R47's text), four need a change first
(physical identity, F13-R41; time bounds outside the segment, F13-R43; bit 14's sketch, F13-R46;
the query work's criteria, F13-R44 and F13-R45), and two rules bind every later release
(F13-R40, F13-R42).

What holds, and reproduces:

- **The open order** (F13-R35): the prefix, the footer, the bits, then the table, then presence
  (`reader.hh:127-132`; `format.hh:382-401`). T1's five later-release segments are unsupported,
  and the same kind with its bit clear is damage (`format13_test.cc:461-475`).
- **T7 in all three profiles**: a fresh open reads no byte of `lengths` in the search,
  norms-less, and document profiles (`format13_test.cc:308-351`), and derived norms equal built
  ones (`:352-370`).
- **The keys section checked and fuzzed** (`check.hh:47-61`; `fuzz_segment.cc:104-135`). CR2's
  96 seeds cover every section of both keyed bases, resealed, `keys` and `key_index` included:
  47 answer, 47 are `corrupt_segment`, 2 are `unsupported_segment` (`cr2-verdicts.out`).
- **The pre-freeze version**: all nine fixtures carry `0x80000001` and prefix CRC `0x4062b616`,
  recomputed by an independent CRC-32C (`crc_prefix.out`); CR11's re-pin changed 6 bytes of each,
  all in the prefix (`fxdiff.out`); 7c's 13 is refused at the segment and at the manifest
  (`format13_test.cc:286-287, 570`).
- **Memory-resident segments need nothing new**: one 42,632-byte segment at heap addresses 0, 1,
  3, 5, and 7 mod 8, and through a read-only file mapping, gives the same check report, top-10s,
  keys, versions, and records, with no ASan or UBSan report, alignment checks included
  (`probe_ram.out`); `written_segment` already serves a segment from heap bytes
  (`index.hh:820-825`).
- **F23's place works for segments**: any later structure, even one that changes the table, is
  unsupported in this binary (F13-R35), within the room F13-R40 counts.

What blocks the freeze: F13-R40 to F13-R46, and F13-R47's items 1 to 4.

### Status

My third re-check:

- **F13-R35: closed.** `classify_structures` runs after `decode_footer` and before `decode_toc`;
  `check_presence` after the table (`reader.hh:127-132`; `format.hh:382-401`); T1's later-release
  rows (`format13_test.cc:461-475`); F4's presence sentence (`format-13.md:374-376`).
- **F13-R36: closed.** Norms are taken from their section only when built (`reader.hh:290-293`)
  and otherwise derived once, under a mutex and an acquire flag, on first use (`:308-311,
  437-448`); T7 over three profiles, derived norms equal built ones, and a flipped `lengths` byte
  fails that first use (`format13_test.cc:308-370`). For T4: `norm()` now tests a pointer on
  every call, and in a norms-less segment loads the flag on every call, since `derived_norms()`
  never publishes into `norms_at_`: a per-posting cost in a profile no configuration writes yet;
  unmeasured (C16 item 3).
- **F13-R37: closed.** `check_segment` refuses an ordinal named twice, a key its range or filter
  misses, and a range other than the first and last key (`check.hh:47-61`); `exercise` calls
  `find`, scans from four bounds, and walks the cursor (`fuzz_segment.cc:117-128`);
  `format13_test.cc:934-991` builds the three sections, resealed.
- **F13-R38: closed.** The citation base (`format-13.md:29-31`), F13-R11's row (`:51`), 7c-3's
  line (`:1322-1324`), and `layers.md:903` as an owed change (`:136`); the ledger's deviations and
  K11 (`format13.md:20, 67, 73`); the comments (`fixtures_test.cc:1-3, 143`;
  `segment_test.cc:307, 467`; `media_test.cc:140`; `strata_engine.cc:619`); the configured bits
  from the layout (`index.hh:215, 231`). `index.hh:46`'s "key term" is CR9's.
- **F13-R39: closed.** `key_of` refuses a key in a keyless index (`reader.hh:515-525`); an add and
  a docid replace carrying one are `invalid` (`format13_test.cc:1051-1057`); F8 states it
  (`format-13.md:500-504`).

The code review's items (its reviewer re-checks them too):

- **CR2: closed.** `exercise` adds `find` with live bits, a hit and an in-range miss, and
  `check_segment` with the prefix; `std::invalid_argument` is no longer a fence
  (`fuzz_segment.cc:104-135, 220-226`); the 96 seeds as above. The libFuzzer corpus and the
  CPU-hour stay with K2.
- **CR4: closed** (`keys.hh:126-131`; `segment_test.cc:678-687`).
- **CR5: closed.** Configured bits are refused without their owner, and when `keys` and the
  prefix disagree, at open and at `install` (`index.hh:2067-2073`); a nonzero slots reference is
  refused (`files.hh:226`, C13); tests at `format13_test.cc:572-577, 609-627` and
  `files_test.cc:234`. F13-R47 item 4 is against the rule's reach.
- **CR6: closed as the review asked**, with tests for M2 (`segment_test.cc:688-716`), M4
  (`format13_test.cc:844-850`), M33 (`:993-1012`), M5 (`:1014-1022`), and M28 (`:1064-1081`). M1
  and M3 are left to CR2's walk, which runs their checks, but the replay asserts no verdict
  (`replay.cc:27-30` passes whatever each input does), so a mutant that accepts disordered or
  miscounted keys still passes. A resealed `keys` section out of order, and one whose count
  disagrees with `key_index`'s, each expecting `corrupt_segment` in `segment_test`, would kill
  them; the rebuilt harness will say.
- **CR7: closed.** One hash per lookup and one probe per segment (`index.hh:1402-1406`;
  `keys.hh:138-150`; `reader.hh:561-566`). Unmeasured (T5, C16 item 3).
- **CR8: partially closed.** Items 1 and 2 by F13-R36 (no norms at open); item 3 by
  `merge.hh:369`; item 4, a keyless segment's `keys_builder` charging versions it never writes,
  is the ledger's Next 2 (`format13.md:88`).
- **CR11: closed.** `format.hh:67-72`; F2 (`format-13.md:230-238`); the prefix CRC and the
  refusals (`format13_test.cc:263-264, 286-287, 570`); T8's hash re-pinned; `digest.txt`
  unchanged (`git log d508158..bb1e90a -- test/fixtures/digest.txt` names `2bd28e7` alone). C17.
- **CR12: closed.** The key counts in `part_bytes` and `disk_estimate` (`reader.hh:772-773`) and
  stage 1 refuses one past `max_term_bytes` (`:518`); `buffered::key` is gone (`index.hh:781-785`);
  `format13_test.cc:1084-1091`.
- CR1 is closed by F13-R35. CR3's `check()` half is closed by F13-R37; its other half, the
  merge's key collection refusing a repeated ordinal, is not in the code (`merge.hh:263-271`
  keeps the last key per ordinal), though the ledger says "nothing further" (`format13.md:51`):
  the code reviewer's. CR9 and CR10 stay open, at convenience.

Conditions: C1 closed. C2: the twelve commits in scope are green on 8 of 8; it carries to the
landing series (C16). C3 to C8 and C10 closed. C9 and C11 carry. C12 carries, with F13-R46's
result. C13 closed (`files.hh:226`; `files_test.cc:234`). C14 carries. C15 open: its row is not
in main's `calibration.md`, and F13-R42 widens its check.

Criteria: K6, K8, K9, K11, and K13 met, K9 now for all three profiles. K1, K3 to K5, K7, and K10
bear on landing (C16); K2, K12, and K14 on the freeze.

### Freeze readiness

The ledger's last item (`format13.md:107-117`): each need of the stack has a place format 13 can
add later (a structure bit and its kinds, by F23) or gets its field before the freeze.

Two facts frame the table:

- A later segment structure is safe since F13-R35: the bits are classified before the table, and
  a bit this binary lacks is unsupported (`format.hh:382-389`), so a later structure may change
  even the table in segments that set its bit (F23 rule 3). The room is 16 bits (F13-R40).
- What must be readable without opening a segment, or the same in every segment from the freeze
  on, cannot wait for a bit: the manifest (F20, its one change) and the header's segment id (F6).

| Need | What the stack asks | Ruling | Before the freeze |
|---|---|---|---|
| Memory-resident segments | Built in RAM, searchable at once, tiered to disk; restarts from peers, the disk tier, or shared memory (`foundations-vision.md:46-48`; durability ledger:49-53) | Holds now: offsets are file-relative, fixed-width reads are `memcpy`s, and the reader takes any view (`probe_ram.out`; `index.hh:820-825`; T7). Verifying and holding a RAM-built segment is reader policy (P15) | Nothing in segments; a per-segment tier mark is F13-R43's |
| Time bounds per segment | Windows dropped as whole segments, merges inside a window (`foundations-vision.md:49-51`; durability ledger:54-63) | In the segment the place exists: a values column records its minimum and maximum (`docvalues.hh:27-28`; F15, bit 4). Readable without opening the segment: only the manifest, whose entry has no field (`files.hh:58-67`) | F13-R43 |
| TTL | Per document, a filter until its window drops (durability ledger:59) | A `date` slot (F15) an index option names; the filter compares it with the read's snapshot time; a drop is a replicated delete (whole segments where the slot's maximum is below the cutoff), never a choice per replica, whose segment files differ (`REPLICATION.md:86`) | Nothing; its bounds ride on F13-R43 |
| HLC per version | LWW merges of minority-side writes on healing, opt-in, free when unused (durability ledger:41-45) | F9's `versions` hold any u64, the minimum rule included (F13-R25 item 7), so a 64-bit HLC can be the version; or a `keys` sub-structure under an unlisted bit with its own field in the `D` record (F21 lets content structures digest). Zero bytes in an index not configured with it. LWW also needs a delete's HLC until the conflict window closes, and live-docs bits hold none: tombstones as keyed documents, or the same sub-structure | Nothing; the durability design chooses |
| Physical identity | Followers install only the files they lack, verified by id, size, and CRC; ids never collide across producers, "a format-13 item" (`durability-modes.md:122-134`; `track-c.md:783-789`) | The header's 16-byte id is the only place, and today it collides across replicas | F13-R41 |
| Doc values and sketches | Columns and sketches for real-time analytics (`foundations-vision.md:27`) | Doc values are F15, in the frozen grammar: the `values` plugin is built before the freeze (`format-13.md:1350-1352`). Sketches: an F23 structure, derived, so no digest record (rule 7), built with seeded hashes so T8 holds | Nothing |
| Vectors | Vector search inside the search library (`foundations-vision.md:31`) | An F23 plugin under an unlisted bit with kinds above 22; content, so it adds digest records (F21); its first release fixes its kinds' rank and alignment | F13-R47 item 2 |
| Bit 14's copy merges | The three gaps encode-once found (`encode-once.md:171-189`) | Reserved; every gap lies inside bit 14's own sections (F23 rule 3), so no frozen field is needed, but the design states a rule the prototype disproves | F13-R46 |
| Query codec and bounds | F10 to F13, H22; the calibration (query-gap ledger:65-87) | F10 to F13 freeze with format 13, chosen by the joint selection (F1 item 2); P8's and P20's bounds are reserved (bits 8 and 11). H22 is no freeze criterion, and PISA's role has two rulings | F13-R44, F13-R45 |

Across every row: F13-R40, the room for later structures, and F13-R42, a later release's manifest
read as damage.

### Findings

40. **F13-R40. Sixteen unlisted bits are all the room format 13 has for later structures** (F4,
    F6, F20, F23).
    - The footer holds [structures u32][reserved u32 = 0] (`format.hh:310-311`). `decode_footer`
      refuses a nonzero reserved word as `corrupt_segment` (`format.hh:326`) before the bits are
      classified (`reader.hh:128-129`), and T1 pins it: the word resealed to 1 is damage
      (`format13_test.cc:417-423`). Bits 0 to 15 are defined or reserved (`format.hh:125-129`),
      so bits 16 to 31 are all a later format-13 release has.
    - Each later structure takes a bit, and so does each later change to how a frozen section
      decodes (F23 rule 3; F13-R27 for `termdict`'s flags). This review's places draw on them: an
      HLC sub-structure, a vector plugin and its sub-structures, sketches, a time-series column
      codec, encryption at rest (`foundations-vision.md:63`), F13-R30's one-block dictionary, and
      a postings codec if H22 lands after the freeze (F13-R44). When they run out the next step
      is format 14, and the reserved word cannot help, since every format-13 binary reads it as
      damage.
    - Fix, before the freeze: the reserved word holds structure bits 32 to 63, classified as bits
      16 to 31 are (lacked by every format-13 binary today: `unsupported_segment`, and by the mask
      `unsupported_index`), never as corruption; F4's table and F6's layout name them; F20's
      configured bits and mask take 64 bits (`files.hh:227-228` decode them with `u32()`, which
      calls a wider value damage); T1 sets each new bit. No byte changes.

41. **F13-R41. The segment id is not an identity** (F6, F20; I13; durability D8).
    - F6 keeps format 12's meaning (`format-13.md:454`): the index's 64-bit local segment number,
      then the first 8 bytes of the index's UUID (`phase-3.md:98-99`; `index.hh:880-882,
      1672-1674`). `install` copies each segment's bytes under a fresh local number and adopts the
      source's UUID (`index.hh:637-640, 667`), so replicas share the UUID and their numbers
      overlap. `probe_identity.out`: B installs A's checkpoint, then each commits one upsert of its
      own; A's segment 2 and B's segment 2 both carry id `0200000000000000 4141414141414141`,
      732 bytes with footer CRC `54307053` against 740 bytes with `be694606`.
    - Nothing checks the id. I13's check at open is Phase 3's condition 8, due "before format 12
      freezes" (`phase-3-lifecycle.md:379-380`), carried and never closed; format 13 inherited it
      without asking (C18).
    - Segment mode installs "only the files they lack, each verified whole by id, size, and CRC"
      and asks that "segment ids never collide across producers ... a format-13 item"
      (`durability-modes.md:122-131`); Track C's catch-up sends "only the files the receiver
      lacks" (`track-c.md:786`). A segment written before the meaning changes keeps its colliding
      id as long as it exists, and a frozen index (P20) keeps its segments while it stays frozen.
    - The footer's CRC does not cover the version either: CRC-32C over a valid 16-byte prefix is
      the constant `0x48674bc7` at every version (`residue.out`), so the header's table CRC, the
      table's, and the footer's are the same at 13 and at `pre_freeze` (`hdrcrc.out`; CR11's
      re-pin changed prefix bytes alone, `fxdiff.out`). An identity of (id, size, footer CRC)
      leaves the version to `read_version`, which every open runs first.
    - Fix, before the freeze: F6 defines the id as the segment's physical identity: unique across
      every producer of the index's segments (replicas, terms, installs), written once by its
      producer, never rewritten by a copy (install, segment shipping, repair, object storage).
      F6 states its construction: 128 bits from the writer's seeded random source, as Lucene's
      and Tantivy's segment ids are random, or a per-node incarnation with a counter (D8's
      producing term alone repeats across replicas, which flush in the same term in document
      mode). F20's segment entry records it beside the local number, and open checks it against
      the header (I13, closing Phase 3's condition 8). D8's "verified whole" includes the prefix,
      which no CRC above it binds. T8 keeps passing the id in, and no segment byte changes.

42. **F13-R42. A later release's manifest reads as damage** (F20; F23 rules 4 and 5).
    - `decode_manifest` checks every field before open and `install` classify the mask: it
      refuses a nonzero values generation (`files.hh:249`), and configured bits or a mask past
      32 bits (`files.hh:227-228`), as `corrupt_index`. `check_readable`, which calls a mask bit
      this binary lacks `unsupported_index`, runs only after it (`index.hh:1988-1991`; at
      `install`, `:620-622`).
    - `probe_manifest_later.out`, each manifest resealed: a mask with bit 20 is
      `unsupported_index`; a values generation of 1, which field updates will write (F20,
      `format-13.md:1053-1054`; `workload-profiles.md` item 4), is `corrupt_index`; both together
      are `corrupt_index`, "manifest: values generation", though the mask says a later release
      wrote it.
    - The same holds for the index's other files. F23 lets P18's marks live in the live-docs file
      with bit 10 "reserved and unused" (`format-13.md:1184-1188`), and `decode_live_docs` refuses
      an unknown encoding as damage (`files.hh:326`) with no bit to classify it first. A field
      with no mask bit also escapes F23 rule 6, which accepts a structure's configuration only
      once every replica reads it; and a replica that calls it damage takes Track C's repair
      (D10, `track-c.md:421-431`), fetching an image from members that hold the same newer files.
    - My third re-check fixed this order in the segment (F13-R35) and did not ask it of the
      index's files, which C15's check covers.
    - Fix, before the freeze: the manifest's readability is classified first (its version, then
      the mask and the segment versions), then every other field. Each field or encoding a later
      release may write in the manifest, live-docs, or metadata files arrives with a mask bit (F23
      rule 4), field updates' values generations and P18's marks included, whichever file holds
      them. F20 and F23 state it, and T1's index cases gain the probe's two rows.

43. **F13-R43. A segment's time bounds have no place readable without opening it** (F20, F15).
    - The owner's need: "time-windowed retention (each segment's time bounds readable without
      opening it)" (`format13.md:113`). Retention by window drops whole segments and keeps merges
      inside a window (durability ledger:56-61; `foundations-vision.md:49-51`), decided per
      segment, across millions of indexes per host, most of them closed (`PLAN.md:192-193`).
    - Inside a segment the bounds have a place: each values column records its minimum and
      maximum (`docvalues.hh:27-28`), so a time slot's column bounds the segment. Outside it only
      the manifest is read, and its entry holds the id, size, footer CRC, counts, live docs, and a
      values generation (`files.hh:58-67`).
    - F20 is the manifest's one change with format 13 (`format-13.md:1025`). A field added later
      is files version 3, which every format-13 binary refuses, and F20 states no rule that a
      later binary keeps writing version 2 where it can, so one commit by a newer binary would
      lock older ones out of the index, windows or not. F20 also records index options in the
      manifest (the order preset, `:1057-1059`), and each new option needs a field.
    - Fix, before the freeze: F20's segment entry ends with an attribute list ([count LEB][per
      attribute: tag LEB, length LEB, bytes], tags strictly increasing, one byte when empty), F20
      keeping the tag table. A tag says whether a binary without it refuses the index
      (`unsupported_index`, classified with the mask, F13-R42) or drops it at its next commit (a
      cache, such as bounds recomputable from the segment). The first tag, defined with the
      retention design, is the [min, max] of the index's retention slot, written at flush, merge,
      and install; HLC bounds and a memory tier's "not yet on disk" mark take tags later, not
      versions. F20 states where F23 rule 4's index options live, so a new option needs no field:
      the metadata file under a reserved prefix is replicated and digested already
      (`files.hh:100`; `index.hh:520-522`), or an index-level list of the same form.

44. **F13-R44. H22's codec verdict is not among the freeze criteria** (F1, F11; QG-R31; query-gap
    C11).
    - The query-gap review made H22, decode per posting against SIMD-BP128, PISA's codec, "a
      freeze-set item: a docid codec is a format change ... so H22's verdict comes before format
      13 freezes, or a codec change waits for the next format" (`query-gap-design.md:940-947` at
      `e272ec2`); the lead adopted it (query-gap ledger:72-74), and its C11 holds it to "before
      format 13 freezes" (`:984-985`). F1 (`format-13.md:175-221`) and F11's candidates
      (`:806-822`) name neither, and the design never mentions SIMD-BP128.
    - After the freeze a block codec comes only under an F23 bit (rule 3), so FOR and the new
      codec would both decode `postings` for format 13's life and format 14's N − 1: one mechanism
      twice (calibration, first row).
    - The verdict may cost no byte: bitpack's block is already SIMD-BP128's vertical layout, 128
      integers in four 32-bit lanes in 16 × b bytes (`bitpack.hh:25-29` at the pinned `abf8972`),
      so the price may be the decoder alone (its prefix sum, exceptions, and `check_increasing`)
      unless E6 finds a layout cost. Either answer needs the number before the freeze.
    - Fix: F1 item 2 adds H22's verdict, E6's microbenchmark of bitpack's FOR and exception
      decode against SIMD-BP128 over E1's block mix; F11 gains SIMD-BP128 as a candidate, kept by
      the joint selection's rule, ported into bitpack under PISA's notice as QG-R31 allows, the
      losing decoder leaving before the freeze.

45. **F13-R45. Two rulings on PISA for the carried query rows** (F1 item 4, T4; QG-R28).
    - Format 13's ledger: "PISA's reference applies to the carried rows that close in format 13:
      postings bytes now, and PISA's exact rows for query classes" (`format13.md:100-101`). So F1
      item 4 projects against "the row's fastest engine ..., PISA included where its answers are
      exact" (`format-13.md:191-201`), and T4 holds the carried rows to it (`:1234`).
    - Query-gap's ledger, at `3405289`, after format 13's ruling at `870494d`: "Phase 4's T5
      closes the carried rows against Lucene, Lucene -bp and Tantivy, with PISA reported beside
      every row; PISA binds from Phase 6's 0.7x" (query-gap ledger:68-70).
    - The lead's quick-tier calibration (query-gap ledger:75-87) gives the stakes, strata
      reordered on the global class against the three and against PISA: `TOP_10` 1.12× [1.11,
      1.19] and 1.75×, `TOP_100` 1.13× and 2.04×, `TOP_1000` 1.10× and 2.49×, `COUNT` 0.93× and
      0.33×; `TOP_100_COUNT` 1.11×, where PISA ranks nothing. The two readings of F1 item 4 differ
      by 1.6× to 2.3× at `TOP_k`.
    - Recommended: the rows close as QG-R28 rules, since PLAN closes a carried row at "its
      original target" (`PLAN.md:142`), set against the three; and the freeze keeps PISA in the
      attribution, since Phase 6's 0.7× binds PISA on this same frozen format: every part of
      PISA's lead that E1 assigns to the format (F12's partitions, P20's quantized impacts, H22's
      codec) has its place in format 13, a kept candidate or a reserved structure. Fix: one
      ruling by the lead, in PLAN and both designs, before the joint selection's projection
      (step 1).

46. **F13-R46. Bit 14's sketch states a base rule the encode-once prototype disproves** (F23's
    table, "Copy merges"; C12).
    - F23's row and "Copy merges" item 1 (`format-13.md:1179, 1200-1203`), my second re-check's
      wording (F13-R32): each block's base is max(previous last, the start of the run holding its
      last − 1). For a block that spans a run start s, with ordinals from s − 3 to s + 5, that base
      is s − 1, past the block's first ordinal. It holds for copied blocks and fails for a
      re-encoded term whose blocks cross runs (`encode-once.md:173-177`).
    - Two more have no field: a short block's posting count, since F10's flags carry only the
      codec (the prototype prefixes a LEB128 inside the block, `:178-179`), and a position tail's
      offset in the 128-position grid for a reader that jumps through level 1 (`:180-184`). F23's
      row gives bit 14 no kind ("none"), but its run table must live in a section.
    - None needs a field in the frozen grammar: all four lie inside `postings`, `positions`, and a
      section bit 14's release adds (F23 rule 3), and a format-13 binary refuses a bit-14 segment
      before it reads any of them (F13-R35). The prototype also settles where bit 14 pays: at a
      flush 41.5% of postings land in short blocks and the segment grows 10.3%, for 4% to 7% of
      one-thread indexing CPU (`encode-once.md:7-10, 151-169, 191-201`), so bit 14 is for copy
      merges, where C12 measures it.
    - Fix (text, before the freeze): F23's row and "Copy merges" state the base rule for blocks
      inside one run and list bit 14's items for its release: re-encoded terms cut at run starts or
      given a per-term marker, the short block's count, the position grid's field or rule, and the
      run table's kind and rank. C12 records encode-once's flush result.

47. **F13-R47. Text the freeze needs** (F1, F8, F15, F23).
    1. A "Freeze readiness" section maps each need of the stack to its place, as the table above
       does, with each later structure's owner and digest rule; F1 lists it among the criteria
       (the owner's ruling, `format13.md:107-117`).
    2. F23 rule 2 counts a kind's alignment in the layout its first release fixes (F5 applies 8
       bytes to every kind today, `format.hh:93, 362`; vectors may take more), and rule 3 lets a
       core structure change how every section is stored in segments that set it (encryption at
       rest, `foundations-vision.md:63`), as `compact` already replaces every section's CRCs
       (`format-13.md:1180`).
    3. One rule for frozen fields: a value the freeze does not write arrives only under a
       structure bit, so a format-13 binary refuses it as unsupported, never as corrupt. F13-R27
       states it for `termdict`'s flags alone; F15's f64 accumulator width (`format-13.md:963-965`)
       and F7's scheme codes above 1 (`:474-476`) are two more.
    4. F8 says the prefix is empty "exactly in a keyless index ... or in a layout without
       `inverted`" (`format-13.md:495-497`), but configured `keys` requires a nonempty prefix at
       creation (`index.hh:231`) and at open (`index.hh:2073`, CR5): a document-layout index
       without a prefix is created keyless, and one whose manifest configures `keys` is refused as
       `corrupt_index`. Either F8 drops the clause and such an index takes a prefix, or both checks
       exempt a layout without `inverted`.

### Pre-freeze list

What must change before `pre_freeze` becomes 13, in addition to F1's criteria:

1. **F13-R40:** the footer's reserved word as structure bits 32 to 63 (unsupported, never
   corrupt); the manifest's configured bits and mask at 64 bits; T1's cases.
2. **F13-R41:** the segment id as physical identity, its construction stated, kept by every copy,
   recorded in the manifest, checked at open (I13).
3. **F13-R42:** the index's files classified by version and mask before any field; every later
   field or encoding under a mask bit, P18's live-docs marks included; T1's index cases.
4. **F13-R43:** the manifest's per-segment attribute list, with its refuse-or-drop rule, for
   time bounds and later per-segment state; where index options live.
5. **F13-R44:** H22's verdict in F1 item 2 and SIMD-BP128 among F11's candidates.
6. **F13-R45:** one ruling on PISA for the carried query rows, before the projection.
7. **F13-R46:** bit 14's rule and its four items, as text.
8. **F13-R47:** items 1 to 4.

Owed already, unchanged by this re-check: F1's items 1 to 7 (the joint selection with C3's
tables, the carried disk and query rows, T4 and T5, the tail profile, fixtures and one CPU-hour
per decoder at the freeze commit, all eight CI jobs); the `values`, `term_lists`, and `index_sort`
plugins with their ranks and `index_sort` readable (C14); K2's libFuzzer corpus, and K1 to K5,
K7, K10, K12, and K14; C9's raw files; C10's `limits.md` and BENCHMARKS edits and C15's row (the
lead's); CR8's fourth item; C17's two items. Then the freeze commit itself: `pre_freeze` to 13,
the files family to 2 (C17 item 2), and the fixtures, the prefix CRC (`0x4ff2df7b`), and T8's
hash re-pinned.

### New blocking items

- For landing: none in the code; C16.
- For the freeze: F13-R40 to F13-R46, and F13-R47's items 1 to 4.

### Conditions

- **C2, C9, C11, and C14 carry. C12 carries**, with encode-once's result (F13-R46). **C13 is
  closed. C15 is open**, and its check covers every file a later release writes, the manifest,
  live-docs, and metadata files with segments (F13-R42, my miss).
- **C16. Landing on main**, before step 7c lands:
  1. The rebase onto main `734949e`, 25 commits past `d508158` (layers' landing review, LC1 to
     LC10, and the tuple-stream inverter): `git merge-tree` gives 16 conflict hunks with 283
     lines inside their markers (`reader.hh` 9 and 166, `index.hh` 5 and 60, `strata_engine.cc` 1
     and 47, `merge.hh` 1 and 10), and main deletes `merge_oracle.hh` and `merge_oracle_test.cc`
     (LC4), which format 13 changes (`merge_main.out`). Main changed no fixture since `d508158`,
     so every fixture and `digest.txt` should hold through it. Every rebased commit green on all
     eight jobs (C2), and the code reviewer re-checks the resolutions.
  2. PLAN records that landing under a pre-freeze version freezes nothing (`PLAN.md:16-17` holds
     a format off main "since it freezes once") and that the freeze is the commit that writes 13.
  3. T4's guard and T5's lookups as A/Bs of the rebased tip against main under a hold the lead
     grants (SYS-6), since CR7's probe, CR12's accounting, and F13-R36's test in `norm()` changed
     per-lookup and per-posting paths unmeasured; the format-12 baselines of T4, T5, and T10
     named at main's last format-12 commit, with their raw files.
  4. The landing criteria ticked with evidence: K1, K3, K4, K5, K7, and K10, whose `verify` and
     `parity` runs no review has run.
- **C17. CR11's rule has no check, and covers segments only.**
  1. A byte-changing commit that re-pins the fixtures and does not bump `pre_freeze` passes every
     test, and the previous build's index is then misread (CR11's scenario). `fixtures_test` pins
     `pre_freeze` beside a hash of every fixture, so a re-pin without a bump fails. Before the
     next byte-changing commit (F10).
  2. The files family stays at version 2 (`files.hh:37`) through every pre-freeze change, and
     F13-R41 and F13-R43 change the manifest's layout: a manifest from an earlier pre-freeze build
     would then parse as damage, or misparse, before its segment range is read. The family takes a
     pre-freeze number as segments do, bumped with each layout change of the manifest, live-docs,
     or metadata files, and 2 at the freeze. Before the first such change.
- **C18. A calibration row, for the lead.** Format 13's reviews accepted "the segment id keeps
  format 12's meaning (I13)" (`format-13.md:454`) while Phase 3's condition 8 on I13 stood open
  (`phase-3-lifecycle.md:379-380`): F13-R41. The check: for every meaning a design keeps from an
  earlier format, find the earlier reviews' open conditions on it.

### In one line each

- Verdict: APPROVE WITH CONDITIONS for landing 7c under the pre-freeze version (C16); NOT READY
  TO FREEZE.
- F13-R35 to F13-R39: closed. CR2, CR4 to CR7, CR11, CR12: closed; CR8: partially closed (item
  4); CR3's merge half: the code reviewer's.
- Freeze readiness: memory-resident segments, TTL, HLC per version, doc values and sketches, and
  vectors have their place; physical identity, time bounds, bit 14, and the query criteria need
  changes first.
- F13-R40: the footer's reserved word as structure bits 32 to 63.
- F13-R41: the segment id as a physical identity; I13.
- F13-R42: the index's files classified by mask before any field.
- F13-R43: a per-segment attribute list in the manifest, time bounds first.
- F13-R44: H22's verdict among the freeze criteria.
- F13-R45: one PISA ruling for the carried query rows.
- F13-R46: bit 14's base rule and its four open items.
- F13-R47: text (freeze readiness, F23's reach, frozen fields, F8 and CR5).
- C16 (landing), C17 (the bump's check; a pre-freeze files version), and C18 (a calibration row)
  are new.

Not run: no benchmark, timing, or A/B (T4, T5, T10, and CR7's and F13-R36's costs are by
inspection); 10 of the 36 tests under ASan and UBSan locally, stopped for a strict hold (CI ran
them); no libFuzzer run, only the corpus replays and CR2's seeds; TSan, Linux, FreeBSD, and
arm64 only through CI; the mutation harness (its reviewer rebuilds it); the rebase onto main,
only `merge-tree`; `strata_engine verify` and `parity`; no VM or Xapiand cluster, so the
calibration's raw files were not read; Lucene's, Tantivy's, and PISA's source; no Xapian or
Xapiand GPL source.

## Fifth re-check (pre-freeze commits)

- Date: 2026-10-19.
- Reviewed: `git log ff5392e..f127670` on `phase4/format13-on-main` (19 commits, the remote
  branch's tip), the eleven in scope: `9fcd823` (MR1 to MR3, CR3's merge half), `492cd6e` (C17),
  `d9a54f1` (F13-R40), `739edc1` (F13-R42), `77751a0` (F13-R41), `85348eb` (F13-R43), `77b705e`
  (CR8 item 4), `6daf93d` (F13-R46), `7371cff` (F13-R47), `13f7033` (F13-R44, F13-R45), and
  `831459e` (bit 16), read at `f127670` in a detached worktree (`strata-wt/review-f13-r5`); every
  line cited is at `f127670`. With them: the ledger (`docs/status/format13.md` at `f127670`), the
  mutants review (`format-13-mutants.md`, MR1 to MR4), `design/durability-modes` at `7ffc16b`
  (`docs/design/durability-modes.md`, cited "DM-design"; `docs/reviews/durability-modes-design.md`,
  DM-R1 to DM-R18), `research/no-stalls` at `467b5b3` (`docs/research/no-stalls.md`), and
  `design/query-gap` at `e272ec2` for F13-R45.
- Checked against: my fourth re-check's F13-R40 to F13-R47, its pre-freeze list and C16 to C18;
  the code review's CR3 and CR8; F1's criteria; the owner's freeze rule (`format13.md:122-132`).
- Checked by:
  - CI through `gh`: every code commit in scope green on 8 of 8 jobs, `9fcd823` (39219508358),
    `492cd6e` (39219514464), `d9a54f1` (39219520670), `739edc1` (39219457027), `77751a0`
    (39226627463), `85348eb` (39226634331), `831459e` (39226641553), `77b705e` (39226438079), and
    the merge `ff5392e` (39211631315). The four text and ledger commits after `77b705e` change no
    code.
  - Builds of `f127670` with Homebrew clang 23.1.2 on macOS 15.6, x86_64: RelWithDebInfo, and
    Debug with ASan, UBSan (`-fno-sanitize-recover=undefined`), and libc++'s extensive hardening,
    after `quiet check` exited 0, at `nice -n 19`, eight jobs.
  - The full suite in the release build: 36 of 36 pass in 127 s at `--parallel 4`
    (`rel-ctest.log`). Under ASan and UBSan with leak detection, also at
    `--parallel 4`: 36 of 36 pass in 3,034 s with no sanitizer report (`asan-ctest.log`;
    fuzz_segment_replay 1,529 s). No hold above light came up during either run.
  - Probes and mutants in `strata-wt/review-f13-r5/.scratch/review-format13-r5/`, each with its
    `.out`, built by `build_probe.sh`:
    - `probe_r5.cc`: later-release manifests (mask bits 20, 40, 63; bit 20 with a values
      generation; bit 20 with a live-docs reference that matches no file; a required and a cache
      attribute), a cache attribute through one commit, the identity (term, install, two replicas
      in one term, a flipped header byte), and a host's `strata.` metadata key.
    - The fourth re-check's `probe_identity.cc` and `probe_manifest_later.cc`, rerun at `f127670`.
    - `c17/run_c17.sh`: `fixtures_test` and `files_test` built against fixture copies, unmutated,
      with the metadata magic changed and the files version kept, and with it bumped (`c17.out`).
    - `mut/run_mut.sh`: three mutants against `format13_test`, the identity check removed, the
      manifest's mask check moved after every field, and the footer's second word refused as
      damage (`mut.out`).
  - Not run: listed at the end.

### Verdict

**NOT READY TO FREEZE, by five small items (F13-R48 to F13-R52); with them in, READY TO FREEZE
once H22 is decided.** Every finding of the fourth re-check is closed in code or text but F13-R45,
which is partly closed: PLAN still binds PISA (below). The mutants review's MR1 to MR3, CR3's
merge half, CR8's fourth item, and C17 are closed, and each claim I could kill with a mutant
fails its test when mutated. The durability design and the no-stalls note need no field that is
missing from the frozen bytes, the 16-byte prefix included. One place is still missing, the
manifest's index-level extension (F13-R51). On the developer's two calls: F8 dropping the
empty-prefix clause stands, and the reserved metadata prefix is enforced now (F13-R49).

What holds, and reproduces:

- **One checked walk of keys** (MR1, MR2, CR3): `each_key` walks the cursor and refuses an
  ordinal named twice (`reader.hh:725-735`), used by `check()` (`check.hh:52`), the merge
  (`merge.hh:275`), and the digest (`index.hh:687`). No other key walk remains: `grep` finds
  `scan("")` only over `inverted`'s dictionary (`reader.hh:505`; `strata_engine.cc:261`).
- **Bits 32 to 63** (F13-R40): one u64 word (`format.hh:313`) classified like bits 0 to 31
  (`format.hh:383-391`); manifests with mask bit 40 or 63 are `unsupported_index`
  (`probe_r5.out`); refusing the second word as damage fails T1 at bit 63
  (`format13_test.cc:460`; `mut.out`, "words").
- **Readability first** (F13-R42): the mask and the segment versions are classified at
  `files.hh:268-273`, before the slots reference, the values generations, and the attributes;
  bit 20 with a values generation, or with a live-docs reference that matches no file, is
  `unsupported_index`, and a values generation with no mask bit stays damage (F23 rule 4;
  `probe_r5.out`). Moving the check after every field fails `format13_test.cc:580, 585`
  (`mut.out`, "order").
- **Physical identity** (F13-R41): term 7 in bytes 0 to 7, the header equal to the manifest's
  entry, kept by `install`, and distinct ids for two replicas committing in one term
  (`070000…d51c8f5f24857a88` and `070000…7e80fe4fca1e3c9e`); one flipped header byte is
  `corrupt_index` (`index.hh:2473`; `probe_r5.out`). Removing the check fails
  `format13_test.cc:666` (`mut.out`, "identity"). The fourth re-check's probe now finds no
  collision between different files (`probe_identity.out`).
- **The attribute list** (F13-R43): tags increasing, a required tag `unsupported_index`, a cache
  tag dropped (`files.hh:297-307`); after one commit by this binary a cache tag is gone
  (`probe_r5.out`, B).
- **The checked bump** (C17): with the metadata magic changed and `files::pre_freeze` kept, the
  re-pin by `files_test` makes `fixtures_test` fail ("the files fixtures changed without a
  pre-freeze bump"), with and without `STRATA_WRITE_FIXTURES`; with the version bumped it passes
  and records `files 80000004` (`c17.out`). `files_test.cc:236` also fails in both mutants, since
  changing the magic, which is frozen, makes the format-12 metadata damage: an artifact of the
  mutant, not of C17.
- **Unconfigured builders** (CR8 item 4): `writer.hh:912-918`; stage 1 assigns version 0 to a
  keyless document (`index.hh:1802`), so the new refusal is a backstop no index path reaches.

### Status

My fourth re-check:

- **F13-R40: closed.** `format.hh:125-131, 296-326, 383-391`; F4 and F6 (`format-13.md:332-370,
  452-466`); F20's 64-bit configured bits and mask; T1 per byte of the second word.
- **F13-R41: closed.** F6 (`format-13.md:470-484`); `new_identity` and `seed_identities`
  (`index.hh:1838-1856`); `set_producer_term` (`:657`); the manifest's identity
  (`files.hh:76-90, 286, 316-322`); the check at open and install (`index.hh:2473`); I13 and
  Phase 3's condition 8 close with it. On the term in bytes 0 to 7, the coordinator's request:
  sound. It makes ids from different terms disjoint by construction, as DM-design:404-406 asks,
  and the low 64 bits carry the uniqueness inside a term, the case DM-R16 raised (document mode).
  Their n² / 2⁶⁵ bound (`format-13.md:478-480`) applies per index per term, so it is the one
  F6 should state. 64 bits rather than Lucene's 128 is enough there, since a collision needs the
  same index, the same term, and two opens' sequences meeting. A collision inside one manifest
  is damage (`files.hh:321`), which is right.
- **F13-R42: closed.** `files.hh:255-273`; F20's "Readability first" and F23 rule 4
  (`format-13.md:1121-1129, 1236-1239`); `format13_test.cc:575-627`.
- **F13-R43: closed in code and in F20** (`files.hh:64-74, 297-307`; `format-13.md:1099-1117`),
  with F13-R48 to F13-R51 on its text and its reach.
- **F13-R44: closed as text** (`format-13.md:179-182, 854`). The verdict is the freeze criterion
  it now is, owed by the query work.
- **F13-R45: partially closed.** F1 item 4 carries the lead's ruling (`format-13.md:192-199`) and
  query-gap's ledger agrees (`query-gap.md:68-70` in its status at `e272ec2`), but PLAN still
  lists PISA in each gate's baseline set as "the reference for query classes"
  (`PLAN.md:99-104`), and its "Reference" bullet takes the fastest engine of that set
  (`:105-107`), so PLAN binds PISA from Phase 4 where both designs say Phase 6. The query-gap
  design's own text (not its ledger) is silent. One PLAN sentence, the lead's, before F1 item
  4's projection.
- **F13-R46: closed.** F23's row (`format-13.md:1263`), "Copy merges" item 1 (`:1317-1324`), the
  four release items (`:1337-1349`), and C12's flush result (`:1351-1354`).
- **F13-R47: closed**, items 1 to 4 (`format-13.md:1285-1308`, `:215`; rule 2 `:1220-1225`; rule 3
  `:1230-1232`; rule 9 `:1248-1252`; F8 `:525-531`).
- **Bit 16, `version_clocks`** (`831459e`): accepted. `format.hh:131, 140`; F4's row
  (`format-13.md:358`); F21's `D` record only where configured; F23's row and bullet. F13-R52
  corrects one sentence of the bullet.

The mutants review and the code review:

- **MR1: closed.** The M1 and M3 rows (`format13_test.cc:1055-1116`), each failing under its
  mutant per `9fcd823`'s message. I did not rerun the mutants review's harness.
- **MR2: closed.** A merge of a repeated key is `corrupt_segment`, never `std::invalid_argument`
  (`merge.hh:275`; the merge rows).
- **MR3: closed.** M1b and M3b stay equivalent, since every walk is now the cursor's
  (`reader.hh:725-735`); the comment is updated (`keys.hh:159-160`).
- **CR3: closed**, its merge half by the same walk (`merge.hh:275`).
- **CR8: closed**, its fourth item in `77b705e` (`writer.hh:912-918`;
  `format13_test.cc:1218-1244`).
- **C17: closed.** Item 1: `fixtures_test.cc:211-254` and `test/fixtures/pre_freeze.txt` (`c17.out`).
  Item 2: `files::pre_freeze`, now `0x80000003` after two layout changes (`files.hh:42-43`), with
  F2 and F20 (`format-13.md:244-247, 1060-1067`).

Conditions: C2 holds for every code commit in scope. C9, C11, and C14 carry. C12 carries, with
encode-once's result recorded. C15 open (the lead's). C16: items 1 and 3 done per the ledger, the
merge's resolutions for the code reviewer; item 2 (PLAN) and item 4 carry. C18 open (the lead's).

### The developer's two calls

1. **F13-R47 item 4: F8 drops the empty-prefix clause. Upheld.** It is one rule, and creation and
   open already enforce it (`index.hh:368, 2428`), so the text now matches the code with no new
   check. The exemption would put a layout test in two checks, a second rule for one mechanism.
   The cost is at most the prefix's bytes in each key, and nothing for Xapiand, whose keys carry
   `Q` already (F8). A `document_layout` host that wants keys passes a prefix; one that passes none gets a
   keyless index that refuses keys loudly (F13-R39).
2. **F13-R43's reserved metadata prefix: enforce it now** (F13-R49). The freeze commit is the
   last possible moment, and the cost is one stage-1 check, so waiting buys nothing.

### Do the durability and no-stalls needs want frozen fields?

No field is missing from the 16-byte prefix, the segment grammar, or the manifest's segment
entry. One place is missing at the manifest's index level (F13-R51).

| Need | Source | Place at `f127670` | Missing |
|---|---|---|---|
| Segment identity, the producing term in it | DM-design:404-406, D16's last row; DM-R16 | F6 and F20, checked at open (F13-R41) | Nothing |
| "Files it lacks" verified whole | DM-design:385-391; DM-R8, DM-R16 | `Image` names size, CRC-32C, and SHA-256 (a log command, D16); the manifest binds identity, size, and footer CRC, and the identity check covers the header | Nothing; a whole-file digest kept beside a segment is a cache attribute if wanted |
| A per-version HLC | DM-design:559-561 (D12 items 3 and 4) | Bit 16 and F21's `D` record | Nothing |
| A delete's HLC in the conflict window | DM-R16; F23's bit-16 bullet | Tombstones as keyed documents, or a live-docs encoding under mask bit 16 (F23 rule 4, F13-R42) | Nothing; F13-R52 corrects the text |
| Time bounds per segment | DM-design:837-840; DM-R15 item 3 | A cache attribute tag (F13-R43) | Nothing; F13-R50 states absence |
| The memory tier's "not yet on disk" mark | DM-design:829-834; DM-R15 | A required attribute tag, since a binary that dropped it would take a RAM segment for a durable one | Nothing |
| Retention drops stamped in the log | DM-design:843-844; DM-R15 item 4 | A log command; replicated metadata if it leaves state | Nothing |
| Per-replica index state of Part B (the disk tier's position, a shared-memory restart's valid mark) | DM-design:829-834 | None: manifest index fields are fixed, metadata is replicated and digested, attributes are per segment | **F13-R51** |
| Copy merges | no-stalls §6 item 6 and §7 ("For the format 13 designer"; the prompt's "item 7") | Bit 14 and the four release items (F13-R46) | Nothing |
| A checkpoint per posting block, resumable merges | no-stalls §6 item 4, §7 | A merge loop's yield and in-memory state, not bytes; with copy merges a block is copied whole, so the yield sits at block boundaries either way | Nothing |

### Findings

48. **F13-R48. F20 gives the order preset two homes, and states a stale files version** (F20,
    F13-R31, C17).
    - "Index options (… the order preset, retention's slot) live in the metadata file under a
      reserved key prefix" (`format-13.md:1111-1115`), and five lines later "The order preset, if
      M17 is kept: an index option recorded in the manifest, set by a replicated operation"
      (`:1118-1120`). M17's row cites F20 for it too (`:853`).
    - Fix (text): one home. The metadata file fits, since it is replicated and digested (F21's `M`
      records) and readers take the preset from each segment's statistics (F7), not from the
      index. The manifest bullet is then deleted or points at the metadata key.
    - F20's version bullet still gives `files::pre_freeze` as `0x80000001`
      (`format-13.md:1060-1062`); the code is at `0x80000003` (`files.hh:42`), and two later
      bullets record the bumps. Fix: the bullet names the constant, not its value, as F2 does
      for segments.

49. **F13-R49. The reserved metadata prefix is not reserved** (F20, F13-R43; the developer's call
    2).
    - F20 proposes `strata.` and refuses a host's `set_meta` under it "from the first option's
      release, not before" (`format-13.md:1111-1113`). Stage 1 checks no metadata key
      (`index.hh:1721-1735`). `probe_r5.out`, D: a host's `set_meta("strata.order_preset")` is
      accepted and held after the commit.
    - Metadata is replicated, digested, and lasts as long as the index, and the files family is 2
      from the freeze. A host key written under the prefix between the freeze and the first
      option's release stays in every replica, and that release then reads it as its own option
      or must refuse an index it should read. Only format 14 or a migration could take the
      prefix back.
    - Fix, now: F20 fixes the prefix (drop "proposed"); stage 1 refuses `set_meta` and
      `delete_meta` under it as `invalid`, before the undo step; the engine writes its options
      through their own operation; a test. Decoding stays permissive: a binary carries a
      `strata.` key it does not know as an opaque entry, since an option that changes what a
      reader must do arrives with a mask bit (F23 rule 4). F20 says so.

50. **F13-R50. A dropped cache attribute has no stated meaning when absent** (F20, F13-R43).
    - A binary without a cache tag drops it, and its next commit writes the entry without it
      (`files.hh:305-307`; `probe_r5.out`, B: 0 attributes on 2 segments). A newer binary then
      reads entries with the tag and entries without it. F20 never says what absence means, and
      retention would drop a window's segments by these bounds.
    - Fix (text, a rule every later release follows, so before the freeze): an absent cache tag is
      unknown, never empty. A reader recomputes it from the segment or acts as if it were
      unbounded (retention keeps the segment), and a writer that knows the tag restores it at its
      next commit. A tag whose absence would be unsafe is required (odd), as the tier mark must
      be.

51. **F13-R51. The manifest has no place for later per-replica index state** (F20, F13-R43; the
    owner's freeze rule).
    - After F13-R43 a per-segment field is a tag, and an index option is a metadata key, which is
      replicated and digested. Per-replica index state has neither, because the manifest's index
      fields are fixed (`files.hh:92-113`) and the metadata would make replicas' digests differ.
      Adding such a field is then files version 3, which every format-13 binary refuses, the
      lockout F13-R43 removed for segments. Part B names candidates: the disk tier's position and
      a shared-memory restart's valid mark (DM-design:829-834). My fourth re-check offered "an
      index-level list of the same form" (F13-R43), and only the per-segment half landed.
    - Fix, before the freeze: an index-level attribute list of the same form and tag rule, at the
      manifest's end (one byte when empty); the files family's pre-freeze version bumped; T1's
      index cases for a required and a cache tag. F20 states which state goes where: per segment,
      a segment tag; replicated, a metadata key; per replica, an index tag.

52. **F13-R52. Bit 16's bullet puts a delete's HLC in a segment** (F23, `831459e`).
    - "Tombstones as keyed documents, or a field of this sub-structure" (`format-13.md:1277-1279`).
      `version_clocks` is a sub-structure of `keys` in segments (`format.hh:140`), which never
      change after they are written. A delete of a document in an older segment lives in that
      segment's live-docs file, so a segment field can hold a delete's HLC only as a record in a
      new segment, which is the tombstone option again.
    - Fix (text): the two places are tombstones as keyed documents, or a live-docs encoding
      carrying each delete's HLC under mask bit 16, which F23 rule 4 and F13-R42 already admit
      (refused by the mask before the file is read). DM-R16 asks the durability design to choose
      before the freeze, and both places exist, so the freeze does not wait on that choice.

### Pre-freeze list

What remains before `pre_freeze` becomes 13, with F1's criteria:

1. **F13-R44's verdict** (H22, F1 item 2): the query work's.
2. **F13-R48:** text.
3. **F13-R49:** the metadata prefix fixed and refused in stage 1, with a test.
4. **F13-R50:** text.
5. **F13-R51:** the manifest's index-level attribute list, the files bump, and T1's index cases.
6. **F13-R52:** text.
7. **F13-R45's PLAN sentence** (the lead's), before F1 item 4's projection.

Owed already, unchanged: F1's items 1 to 8; the `values`, `term_lists`, and `index_sort` plugins
(C14); K2's libFuzzer corpus, K1 to K5, K7, K10, K12, and K14; C9's raw files; C10's and C15's
(the lead's); C16 items 2 and 4; then the freeze commit (`pre_freeze` to 13, the files family to
2, fixtures, the prefix CRC, T8's hash, and `pre_freeze.txt` re-pinned).

### New blocking items

- For landing: none from this re-check; C16 as before.
- For the freeze: H22's verdict and F13-R48 to F13-R52.

### Conditions

- C2, C9, C11, C12, C14, C15, C16 (items 2 and 4), and C18 carry. C13 and C17 are closed.

### In one line each

- Verdict: NOT READY TO FREEZE until F13-R48 to F13-R52 land; then READY TO FREEZE once H22 is
  decided.
- Closed: F13-R40 to F13-R44, F13-R46, F13-R47, MR1 to MR3, CR3, CR8, C17; bit 16 accepted.
  F13-R45 partially (PLAN).
- Calls: F8 without the empty-prefix clause stands; the metadata prefix is enforced now.
- Durability and no-stalls: no missing field in segments or the prefix; the manifest's index
  level lacks a place (F13-R51).
- F13-R48: one home for the order preset; F20 names the files version by its constant.
- F13-R49: the `strata.` prefix fixed and refused in stage 1.
- F13-R50: an absent cache tag means unknown.
- F13-R51: an index-level attribute list.
- F13-R52: a delete's HLC lives in live-docs or tombstones, not in a segment field.

Not run: no benchmark, timing, or A/B; no libFuzzer run and no fuzz corpus beyond the
suite's replays; TSan, Linux, FreeBSD, and arm64 only through CI; the mutants review's harness
(MR1's kills are the commit's claim, checked against the rows, not rerun); the code reviewer's
check of the merge's resolutions (C16 item 1); `strata_engine verify` and `parity`; no VM or
cluster; Lucene's, Tantivy's, and PISA's source; no Xapian or Xapiand GPL source.

## Re-check of F13-R48 to F13-R52

- Date: 2026-10-19. Rechecker, from the fifth re-check's findings; read at `01ab63a` in the
  detached worktree `strata-wt/recheck-f13`, after `quiet check` exited 0; builds at `nice -n 19`,
  four jobs, RelWithDebInfo. Fixes `aada0db`, `540ae6b`, `637cfe3`, `825c516`, `19ae896`.
- Run: `files_test`, `index_test`, `format13_test`, `fixtures_test`, and `fuzz_files_replay` all
  pass. The architect's `probe_r5.cc` (the copy in `format13/.scratch/f13-r5-fix/`, rebuilt
  against this tree by `.scratch/recheck-f13/build_probe.sh`; output `probe_r5.out`): 0 failures,
  and its section D now reads "refused; held after commit: 0".

Verdicts:

1. **F13-R48: CLOSED.** The order preset has one home, a metadata key under the reserved prefix,
   read by the segments' F7 record, never from the index (`format-13.md:1135-1138`; M17's row
   `:853` cites F20 for the same). The manifest bullet is gone. The version bullet names
   `files::pre_freeze` and says the code holds the value (`:1060-1063`); the only literal values
   left are the dated bump records (`:1096, 1115, 1123`) and the segments' constant (`:237`).
2. **F13-R49: CLOSED.** `index.hh:1723` returns `invalid` for `set_meta` and `delete_meta` under
   `files::reserved_meta_prefix` (`files.hh:128`), before the undo step is pushed; F20 drops
   "proposed" and states that decoding stays permissive (`format-13.md:1124-1134`). Test:
   `index_test.cc:224-238` (invalid at op 1 with the earlier `m` undone, `delete_meta` invalid,
   `strata` and `stratum.x` accepted). Reverting the one line at `index.hh:1723` makes
   `index_test` print `index: FAILED`; restored, it prints `index: ok`, and the tree is clean.
3. **F13-R50: CLOSED.** F20 states "absent means unknown, never empty", the reader recomputes or
   acts as unbounded (retention keeps the segment), the writer restores at its next commit, and
   a tag whose absence is unsafe is required (`format-13.md:1110-1114`). Text only, as asked.
4. **F13-R51: CLOSED.** The manifest ends with an index-level list through the helper pair shared
   with the segment entries (`files.hh:206-224`; written `:265`, read `:335`), with
   `known_index_attribute` (`:76`) and the required/cache rule from the segment list; `install`
   never touches `m_.attributes`, so the target keeps its own (`index.hh:768-835`, checked by
   `format13_test.cc:675-709`). The files version is `0x80000004` (`files.hh:42`) and F20 says
   where each kind of state goes (`format-13.md:1116-1123`).
   - Fixtures: `manifest.bin`, `live.bin`, `meta.bin` differ from `825c516^` in the version byte
     (`83` to `84`) and the trailing CRC; `manifest.bin` also gains one `00` byte, the empty
     list, before the CRC (267 to 268 bytes). `pre_freeze.txt` changes the `files` line only
     (`80000003 37c3e31e4c12b339` to `80000004 9392dc6fa6cff923`). No `.sx` fixture and not
     `digest.txt` change in `825c516^..01ab63a`, and `segments 80000001 e68d6726da05d544` is the
     same.
   - Tests: `files_test.cc:118-136`, `format13_test.cc:671-709` (required tag at open and install
     unsupported, cache tag opens, disorder corrupt, a cache tag gone after the next commit).
     Mutant: `known_index_attribute` returning true fails `files_test` and `format13_test`;
     restored, both pass. `fuzz_files`' generator emits both lists (`fuzz_files.cc:18-54`), and
     its replay passes.
5. **F13-R52: CLOSED.** F23's bit-16 bullet names tombstones as keyed documents or a live-docs
   encoding under mask bit 16, says a segment never changes after it is written, and says the
   freeze does not wait on DM-R16 (`format-13.md:1293-1302`); no "field of this sub-structure"
   remains.

New findings: none. One line for the lead: F13-R45's PLAN sentence and H22 are still open (not
in scope here).

Not run: the ASan/UBSan suite and the rest of ctest (only the five targets above), TSan, Linux,
FreeBSD, arm64 (CI), libFuzzer, benchmarks, Lucene/Tantivy/PISA source; no Xapian or Xapiand GPL
source.
