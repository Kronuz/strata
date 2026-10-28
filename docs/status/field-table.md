> Status: CURRENT (2026-10-27) F1 of fields-and-keys: the field table in strata core. Branch `phase8/field-table`, worktree `strata-wt/field-table`.

# field-table

- Design: docs/design/fields-and-keys.md (revision 1), section 7 "F1" and D2, D7; review: docs/reviews/fields-and-keys-design.md, "Re-check 1" (two suggestions folded into the criteria below: the build-time-size mutant, and out-of-range `declared(i)` among stage 1's rejections).
- Branch / worktree: phase8/field-table from main 0078bb6, at /Users/kronuz/code/Kronuz/strata-wt/field-table
- Prover: developer. Builds under `quiet light f1-fields`.

## Done criteria (copied exactly from the design's section 7; the lead alone changes them)


1. `core/fields.hh`: the table (declarations, name-to-id map, id-to-declaration, the chain), its
   encode and decode with CRC, its generation file, and the manifest's `fields` reference replacing
   `slots` with the manifest's version bumped; open reads it and recomputes the chain, `check()`
   verifies it, the digest covers it.
2. Transactions carry `fields` and `expected_fields`; stage 1 applies D2's three steps and D7's rules
   (name length, a known role and postings kind, at most one key field and only in a keyed index, a
   conflicting declaration rejected naming the entry, an identical one answering the id); the undo
   log rolls declarations back; `fields()` answers the last applied table with its mark;
   `validate_context` carries the resolution.
3. Tests (FK-R1): a property test that pipelines transactions built against stale tables, with
   distinct and identical new names and mixed `id`/`declared` references, against a model, checking
   that every resolved term's field is the one the host named; a rejected declaring transaction
   followed by one that reuses the ids; a lost tail (a declaring commit dropped, the ids redeclared
   in another order, then a transaction under the old mark) that rejects `fields_conflict` and never
   misattributes, with a mutant that skips the chain check failing it; replay, `install` and a
   checkpoint round trip; two replicas applying the same log reach equal tables, chains and digests;
   crash points around a declaring commit; the decoder fuzzed; a revert check per fix and a dying
   mutant per rule; ASan with UBSan and TSan in separate builds; CI green.
4. T8 measured, and the generation encoding chosen against it (C6).
5. `docs/mechanisms/field-table.md`. `key_prefix` and segments untouched.

## State

- State: in review (security review of the decoder next); CI pushed, not awaited.
- Done (commits strata:f3b7594, strata:4f3c1e7 and the proof commit after it):
  - `include/strata/core/fields.hh`; manifest `fields` replaces `slots`, `next_fields` added, files family 0x80000006; `index.hh`: stage 1, undo, `fields()`/`fields_mark()`, views, digest, `check()`, install, retention, commit allowance.
  - Fixtures regenerated for the version bump (`test/fixtures/{manifest,live,meta}.bin`).
- Next: the lead's CI watch; security review of `decode_fields` and the append-only file; F2.
- Blocked on: nothing.
- Decision for the architect/lead: the table's file is append-only in place (T8 needs it; see criterion 4). Bytes in a fields file are never overwritten, but a file that grows after a manifest names a prefix is new to the core (everything else is write-once).

## Ticks (evidence)

1. Ticked. `core/fields.hh` (table, chain, codec); manifest `fields`/`next_fields`, version 0x80000006; open reads and recomputes (`read_fields`, `load_files`), `check()` compares the file with the table (mutant `check-skips-the-file` killed), the digest has an `F` record per declaration (mutant `digest-ignores-fields` killed).
2. Ticked. Stage 1 in `evaluate()`; every rule has a killed mutant (`tools/mutants/fields.py`: 25 mutants, all killed, the unmutated copy passes). Out-of-range `declared(i)` rejects (`declared-out-of-range-resolves`), Re-check 1's two suggestions are in.
3. Ticked, each part with its test in `test/fields_test.cc`:
   - Property test against a model, 12 seeds of 300 steps (stale tables, repeated and conflicting names, mixed refs); mutant `declared-as-build-size-plus-i` killed.
   - Rejected declaring transaction then reuse: `rollback_then_reuse`; mutant `rollback-keeps-declarations` killed. A bad_alloc sweep with declarations in `apply_atomic_test` (142 allocation points, with and without fields).
   - Lost tail: `lost_tail` (a shorter table, and the ids reused under another name); mutants `chain-check-skipped` and `shorter-table-accepted` killed.
   - Replay (skipped), checkpoints (each prefix), install, two replicas (tables, chains, digests): `file_and_replicas`.
   - Crash at every mutating operation of a declaring commit, appending and after a torn tail: `crash_points`; `torn_tail_and_check`.
   - Decoder: `decoder` (every prefix and bit flip refused, each rule) and `fuzz/fuzz_fields.cc`: 11.2M runs in 8 minutes, no finding; 203 corpus entries added.
   - ASan+UBSan (Debug -O1, build-asan) and TSan (build-tsan), separate dependency trees, LLVM 22: fields, files, core_kv, index, apply_atomic, install_own, shared_view, shared_read, lifecycle and the fuzz replays pass (install_errors and directory under ASan too). Release: all 57 tests pass.
   - CI: pushed to `phase8/field-table` (the `phase*/**` trigger covers it); not awaited.
4. Ticked. `bench/field_table` (release, Intel Mac, faultfs): 2,000 fields one per commit write 36,903 B against a final table file of 26,909 B (1.37x); 100,000 fields write 1,988,903 B against 1,488,910 B (1.34x), in one file; resident table 171 B a field at 2,000 and 167 B at 100,000. Chosen encoding: an append-only block per commit, the manifest naming the prefix; rewriting the table whole each commit would write about F^2/2 x 15 B (75 GB at 100,000). Within T8's 4x.
5. Ticked. `docs/mechanisms/field-table.md`. `key_prefix` and the segments are untouched.

## Log

- 2026-10-27: branch from main 0078bb6. `core/fields.hh`, manifest, stage 1, `fields()`, tests, fuzz target (strata:f3b7594).
- 2026-10-27: mutants, torn tail, T8 bench, the mechanism note (strata:4f3c1e7).
- 2026-10-28: sanitizer builds, fuzz run and corpus, apply-atomic sweep with declarations.
