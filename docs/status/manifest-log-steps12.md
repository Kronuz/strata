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
- State: step 1 built and pushed (`journal:b1b7368`), hardening owed; step 2 not started (handed off by budget)
- Done criteria (set by the lead; contributors tick, never edit), from the lead's prompt:
  - [x] Unit tests with fixtures (step 1: `test/framing_test.cc`, fixtures `test/fixtures/framing/`; step 2 owed).
  - [x] tail_model's single-bit-flip property as a test (0 silent): step 1, 219,672 flips, 0 silent, 584 in padding.
  - [x] Every honest crash image as a test: step 1, 1,020 images, 672 torn, 0 misread.
  - [ ] A libFuzzer target for the decoder, 5 minutes under ASan+UBSan (Homebrew LLVM): step 1's target written (`fuzz/fuzz_framing.cc`, replay test green), the 5-minute run not yet done; step 2 owed.
  - [x] A revert check per rule, and mutants (step 1): five mutants, each fails `journal_framing` (below).
  - [ ] TSan if anything is concurrent (nothing is: the framing and codec are pure functions).
  - [ ] Linux through `tools/vm/aws-linux.sh` with the local VM (`STRATA_AWS_VM_ALLOW` unset).
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
- Doing: nothing (handed off at the lead's ~250k budget).
- Next (for a fresh developer; read this ledger and `journal:b1b7368`):
  1. Step 1 hardening: an ASan+UBSan build of journal (own build directory) and its ctest; the
     libFuzzer build (`-DJOURNAL_FUZZ=ON`, `CXX=/usr/local/opt/llvm/bin/clang++`) and
     `journal_fuzz_framing -max_len=16384` for 5 minutes seeded with `test/fixtures/framing/log-*`
     (inputs need a leading mode byte: 0 for the manifest-log stream); Linux on a local VM only
     (owner, 2026-11-04: no AWS at all).
  2. Journal CI does not run on branch pushes; a draft PR (the lead's call) or the lead's merge
     to `main` runs it. Tell Detent's lead before it lands (design step 1).
  3. Step 2, per the plan below, plus C7 (below).
- Blocked on: nothing.
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
