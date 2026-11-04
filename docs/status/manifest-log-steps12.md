> Status: CURRENT (2026-11-04) the ledger of the manifest log's first steps 1 and 2; design
> `docs/design/manifest-log.md` revision 1 at `strata:1c8ff50` (branch `design/manifest-log`).

# manifest-log-steps12

- Design: `docs/design/manifest-log.md` revision 1 (`strata:1c8ff50`), "First steps" 1 and 2;
  review `docs/reviews/manifest-log-design.md` (`strata:057c47e`: steps 1 and 2 may start, step 1
  with L1's tail rule). The architect's re-check of revision 1 runs in parallel; its findings come
  here. The security reviewer reviews step 2's decoder (and step 1's reader) before step 3 lands.
- Branches / worktrees:
  - Step 1: journal `phase/manifest-log-steps12` at `~/code/Kronuz/journal-wt/manifest-log-steps12`,
    from journal `main` `journal:bfad97f` (the design's cited commit).
  - Step 2 and this ledger: strata `phase/manifest-log-steps12` at
    `~/code/Kronuz/strata-wt/manifest-log-steps12`, from strata `main` `strata:9e344d8`.
- State: steps 1 and 2 built and hardened, nothing merged. Journal at `journal:5336512` (draft PR
  https://github.com/Kronuz/journal/pull/2 runs its CI); strata at this branch's head, pinning that
  journal commit. Both decoders await the security review before step 3 lands.
- Done criteria (set by the lead; contributors tick, never edit), from the lead's prompt:
  - [x] Unit tests with fixtures (step 1: `test/framing_test.cc`, fixtures `test/fixtures/framing/`; step 2 owed).
  - [x] tail_model's single-bit-flip property as a test (0 silent): step 1, 219,672 flips, 0 silent, 584 in padding.
  - [x] Every honest crash image as a test: step 1, 1,020 images, 672 torn, 0 misread.
  - [x] A libFuzzer target for the decoder, 5 minutes under ASan+UBSan (Homebrew LLVM): step 1's first run found a header the reader accepted wrongly (fixed, `journal:5336512`), the rerun 481,901 inputs clean; step 2's `fuzz_manifest_log` 339,203 inputs clean.
  - [x] A revert check per rule, and mutants (step 1): five mutants, each fails `journal_framing` (below).
  - [ ] TSan if anything is concurrent (nothing is: the framing and codec are pure functions).
  - [x] Linux on the local VM (Ubuntu 24.04, g++ 13.3): journal 12 of 12 at `journal:5336512`; strata's codec tests 7 of 7. Run with `linux.sh ssh` into a task directory (`~/mlog2`), since `aws-linux.sh run`'s local path wipes the shared `~/work`.
  - [x] A how-it-works note in `docs/mechanisms/` (step 1: journal `docs/mechanisms/framing.md`; step 2 owed).
- Done:
  - Step 1 (`journal:b1b7368`, pushed): `include/journal/framing.h` (stream, emitter, header and
    fragment checks, `read_record`, `read_file`, `scan` with D6's table as revision 1 words it);
    the turn log rebased on it; `test/framing_test.cc` ports tail_model's checks 1, 2, 4, 5 and 6
    with the model's exact counts and one case per table row; `crc_fixture_test` now checks the
    RFTW0001 fixture regenerates byte for byte; `fuzz/fuzz_framing.cc` plus its replay test;
    the power test (T9) migrated to the `framing::` names only. Local RelWithDebInfo build, macOS:
    all 11 journal tests pass (`journal_framing` 2.3 s, `journal_crc_fixture`,
    `journal_turn_log_power`, `journal_fuzz_framing_replay` included).
  - Mutants (`.scratch/mutants.sh` in the journal worktree), each killed by `journal_framing`:
    M1 a cut read as torn (revision 0's rule): short writes 21,845 of 21,962 damage;
    M2 no whole-record-after rule: two in flight 0 of 76 damage, the row case fails;
    M3 zeros checked past the failing piece: 256 honest images misread;
    M4 an open record at a sector end read as damage: 128 honest images misread;
    M5 manifests written on a no-manifest stream: the emitter no longer rebuilds the model's bytes.
- Done (second developer, 2026-11-04):
  - Step 1 hardening. ASan+UBSan (Homebrew LLVM, Debug, `build-asan`): 10 of 11 under `ctest -j4`;
    `journal_crc_fixture` hit its 30 s timeout there and passes alone in 6.3 s (87 checks, 0
    failures): its syncs contend under parallel ctest, not a sanitizer finding. `journal_framing` takes
    27.3 s under ASan, so its timeout is now 120 s. Linux (local VM, Ubuntu 24.04, g++, RelWithDebInfo,
    task dir `~/mlog2`): 12 of 12 pass, `journal_linux_completion` included. Draft PR #2 opened to run
    journal's CI (no attribution lines).
  - `journal:9a3734f`: `framing::record_end(stream, position, size)`, the emitter's end in constant
    time for streams without manifests (C7's framing part). Its test matches the emitter on 397,475
    (position, size) cases at 256- and 512-byte blocks; the mutant without the padding rule makes
    25,139 differ.
  - Step 2 (`include/strata/core/manifest_log.hh`, nothing uses it yet): `log_state`, `log_edit`,
    `encode_snapshot`, `encode_edit`, `decode_snapshot`, `decode_edit`, `record_header`, `apply` (D3's
    replay checks, all before any change), `diff`, and C7's `log_sizes`/`snapshot_size`/
    `snapshot_bytes`. strata now fetches journal (pinned at `journal:9a3734f`, a branch commit until
    PR #2 merges) for the framing; `check_core_includes` allows `journal/framing.h`.
    `files::detail::reader` gains a raw-body constructor that refuses overlong LEB128.
    How it works: `docs/mechanisms/manifest-log-codec.md`.
  - Step 2 proof (macOS, RelWithDebInfo): `manifest_log_test` stages layout (bytes spelled out),
    fixtures (`test/fixtures/manifest_log/`: a snapshot and 9 edits: scalars, pins, flush, deletes,
    merge, metadata, fields, segment attributes, index attributes), round trip (40 seeds x 60
    successive random pairs: decode(encode(diff)) == diff, replay gives c, snapshots encode equal,
    snapshot decodes to c, C7's size and framed size equal the encoding's), 21 replay-check
    rejections each leaving the state unchanged, decoder (every prefix, trailing bytes, overlong
    integer, mask bits, empty sections, readability, the canonical deleted encoding, counts past the
    bytes left, a 2^62-delete claim), tracked size over 200 edits. `fuzz_manifest_log_replay` over 33
    seeds, `core_boundary`, `core_includes`: pass.
  - Step 2 mutants (`.scratch/mutants.py`), 17 of 17 killed: generations contiguous; removed id
    present; added id absent; added id below the next id; added identity new; ordinal below the
    document count; ordinal not yet deleted; removed key present; table size extended; pins at most
    the generation; totals as running sums; C7 (S from the last snapshot written); shortest-form
    integers; one encoding per deleted set; deltas bounded by their bytes; the bitset's size checked
    first; every check before any change (the totals check moved after the mutation). A first run
    left two deletes mutants alive (the totals check caught their cases too) and showed a redundant
    bitset bound, now removed.
  - Step 1's fuzz finding (`journal:5336512`): the first 5-minute `journal_fuzz_framing` run aborted
    after 3,532 inputs: a no-manifest header with a nonzero manifest block under a valid CRC read as
    ok, so re-emitting the records did not rebuild the input. `parse_header` now refuses nonzero
    manifest fields when the count is `0xffff`. Regression: `framing_test` forges such a header and
    expects damage; with the check reverted it reads clean and fails (46 checks, 1 failure). Crash
    input kept in journal's main checkout `.scratch/manifest-log-steps12/`. Rerun: 481,901 inputs,
    peak RSS 516 MB, clean; journal release 11 of 11, ASan `journal_framing` and `journal_turn_log`
    pass, Linux 12 of 12.
  - Step 2 under ASan+UBSan (Homebrew LLVM, Debug, `build-asan`): `manifest_log`, `files`, `fields`,
    `fuzz_manifest_log_replay`, `fuzz_files_replay` pass. `fuzz_manifest_log`, 5 minutes from the 33
    seeds: 339,203 inputs, coverage 5,130, peak RSS 431 MB, no finding. Linux: `manifest_log`,
    `files`, `fields`, both replays, `core_boundary`, `core_includes` pass; journal's `framing_test`
    45 of 45 there.
- Doing: nothing.
- Next:
  1. The lead: journal's CI on PR #2 and strata's on this branch; the security review of both
     decoders (`framing::scan` and its checks; `decode_snapshot`, `decode_edit`, `apply`).
  2. After PR #2 merges, repin strata's journal to that main commit (it pins a branch commit now).
  3. Step 3 (C4, C5, C7's writer side: track S with `apply`'s sizes, T5's per-platform counts).
- Blocked on: nothing.
- fsync counts per platform: steps 1 and 2 issue no syncs (the framing and the codec are pure).
  The platform fact stands for step 3's T2 and T5: completion's grouped `SyncFiles` fsyncs every file
  on Linux and issues one device flush per group on macOS, so per-commit counts are reported per
  platform there.
- Design vs code (step 2):
  - A segment's deleted set is ascending ordinals (as `files::live_docs`), not the plan's 64-bit
    words: a deltas body over a huge document count would make words cost memory the input does
    not bound. Step 3 converts to and from the index's `deleted_bits`.
  - Records are canonical beyond D3: integers in shortest form, one deleted-set encoding (bitset on
    a tie, none exactly without deletes), no empty section, no unknown mask bit. A snapshot then
    encodes byte for byte (T6), and the fuzz target checks re-encoding.
  - Replay checks D3 does not list, kept because they are invariants: a removed pin present, a
    segment's live count after its deletes equal to its count less them and its live length not
    above the old, an added identity new (I13), segment attributes only for a present segment, a key
    not both set and removed, the manifest's scalar checks (structures within the mask, versions with
    segments, live length within total length).
  - The scalars hold min and max versions as 64-bit values checked to fit 32 bits; the edit's
    readability checks the carried mask and versions only.
  - Unknown attribute tags are the manifest's rule: an even one dropped on reading (so an edit
    carrying only it reads as an empty set), an odd one `unsupported_index`.
- Design vs code (step 1): the turn log's whole-block scanner `scan_segment` stays in
  `turn_log.h` (its gaps, epochs and manifests are its recovery model) and shares the framing's
  header and fragment checks; the design lists "the scanner" in the component. "Torn at a 512-byte
  multiple" uses the sector size whatever the block size. The snapshot-must-be-whole rule (D6.3) is
  the caller's (strata), not the component's.

## The architect's re-check (strata:79beaf4): conditions for later steps

- C7: the writer's tracked S equals the encoded snapshot's size, framing included, with a property
  test and a mutant. Step 2 can provide `snapshot_bytes(state)` from the encoder and the framing
  (`framing::Emitter` over the snapshot record); the tracked S and its property test are step 3's
  writer.
- ML-R15/C4: the open does one `sync_data` before it reports a durable head. Step 3 (recovery).
- ML-R16/C5: the edit bound becomes max(R, n, S/c); T5 reports bytes written per commit. Step 3
  (the trigger, D7).
- Known residual, ML-R17: a lost write can drop up to 3 edits per zeroed sector, and up to 39 on a
  reverted size (Q3's row, read silently as torn).
- Platform fact (Detent, via the lead): completion's grouped `SyncFiles` issues one device flush
  per group on macOS but fsyncs every file on Linux. "One device flush per commit with a new
  segment" holds on macOS only; T2 and T5 count fsyncs per commit per platform, and no macOS figure
  is claimed for Linux.

## Step 2 plan (the codec, from `files.hh` and `fields.hh` at `strata:9e344d8`)

A new header `include/strata/core/manifest_log.hh`, used by nothing yet (no behavior change):

1. State: `log_state` (D3's snapshot) holds uuid, generation, revision, last docid, next segment,
   applied position, schema version, configured, mask, min and max segment versions, document
   count, total length, live count, live length, pins (ascending), segments by id (each:
   `segment_ref`'s fields less `live`, plus its deleted set as 64-bit words, one bit per
   ordinal), metadata (`std::map`), the field declarations, index attributes. The retired
   `next_live`, `next_meta`, `next_fields`, `meta`, `fields` and `segment_ref::live` have no
   place in it; step 3 migrates `files::manifest` to it and deletes them.
2. Record: kind (1 snapshot, 2 edit), generation (LEB128), payload. Snapshot: files version
   first, then every scalar, then the sections in order. Edit: a presence mask of the scalars
   that differ from the previous record's state (never the UUID), a section mask, then pins
   added/removed, segment ids removed, entries added (as a snapshot's), new deletes per segment
   (id, ascending ordinal deltas, live count and live length after), metadata set/removed, the
   table size the declarations extend then the declarations (`fields_detail::put_decl`),
   segment attributes per segment, index attributes.
3. Encoders: `encode_snapshot(const log_state&)`; `encode_edit(const log_edit&)`; a
   `diff(d, c)` building the full `log_edit` (tests and fuzz seeds; step 3's writer builds
   deletes and metadata from the change list instead).
4. Decoder: readability first as `decode_manifest_for` (files version, then mask and segment
   versions against `readable_by`, `unsupported_index`), every count and length bounded by the
   bytes left (`detail::reader`'s `count(min_bytes)`), then the values. Replay checks (D3):
   generations contiguous; removed id present; added id absent, below next segment, in id
   order; new ordinal below its segment's document count and not yet deleted; removed key
   present; the table size extended equals the table's; pins at most the generation; document,
   live and live-length totals as running sums compared when carried. Violations are
   `corrupt_index`.
5. Tests: fixtures (a snapshot and one edit of each kind, bytes committed), a round-trip
   property over random pairs of states (replaying `diff(d, c)` onto d gives c; snapshots
   encode equal), one test per replay check with its mutant; a libFuzzer target over the
   decoder plus replay seeded with the fixtures, 5 minutes under ASan+UBSan.

## Log

- 2026-11-04: worktrees created (journal from `journal:bfad97f`, strata from `strata:9e344d8`).
  Strata has no journal dependency today (`CMakeLists.txt:26` takes `journal::IO` from
  completion), so step 2's codec is the record payload alone and does not wait on step 1's
  landing. Journal's CI runs on pushes to `main` and pull requests only
  (`.github/workflows/*.yml`), so a branch push there runs no CI.
- 2026-11-04: step 1 written; the first local build waited on light slots and was lost when the
  lead stopped the session; rebuilt and tested (11 of 11 pass); five mutants killed; committed and
  pushed as `journal:b1b7368`. The re-check's conditions, ML-R17 and the Linux fsync fact recorded
  above. Handed off at about 260k tokens, before step 2.
