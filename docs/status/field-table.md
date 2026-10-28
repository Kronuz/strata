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

- State: building. The table, its file, stage 1, the tests and the fuzz target are in and pass in a release build; proofs (mutants, sanitizers, T8, CI) and the mechanism note are next.
- Done: see the log.
- Doing: mutants, T8, sanitizers.
- Next: the mechanism note, then push and hand to the security review (decoder).
- Blocked on: nothing.
- Ticks: none yet.

## Log

- 2026-10-27: branch from main 0078bb6. `core/fields.hh` (table, chain, file codec), manifest `fields` replaces `slots` (files family 0x80000006), stage 1 and `fields()` in `index.hh`, `test/fields_test.cc`, `fuzz/fuzz_fields.cc`.
