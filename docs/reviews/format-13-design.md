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
