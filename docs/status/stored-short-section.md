# stored-short-section (SR2)

Branch `fix/stored-short-section` from origin/main 03afa3a.

## State

SR2 (`docs/reviews/f14-step1-decoder.md`): a stored section under 4 bytes in a segment with documents read as
"every record empty". The refusal already sits on main (21ca549): `stored` `reader`'s constructor throws
`corrupt_segment("stored section")` when the structure is present, `doc_count > 0` and the section is under
4 + 16 + 1 bytes (a frame, one index entry, the count). That runs at open, before `check()`, `stored()`,
`stored_frames()` or `stored_frame_of()` can be reached, so no lazy path is left: those two keep their
`size() < 4` shortcuts, which then only serve a segment without the structure or without documents.

This branch adds what was missing around it: coverage and fuzz reach. No format change, no reader change.

## Done

- `test/format14_test.cc`: truncated stored sections of 0, 1, 2, 3, 4, 5 and 20 bytes, and the section's 4-byte
  tail alone, are rejected; a segment without documents still opens.
- `fuzz/fuzz_stored.cc`: `data[1] & 4` cuts the chosen section short (to under 24 bytes, or anywhere); an oracle
  traps when a segment with documents and a stored structure opens on a section under 21 bytes; 25 seeds added
  under `fuzz/corpus/stored/` (one a 2-byte input that read `data[9]` past the end in the first draft of the cut).

## Evidence

Clang 23 ASan+UBSan, x86_64 (`.scratch/build-asan`):
- Constructor check disabled: `format14_test` prints 7 failures (sizes 0, 1, 2, 3, 4, 5, 20) and exits 1; the
  stored replay traps on the new oracle (exit 132). Restored: both pass.
- Mutant, threshold `< 4` instead of `< 21`: 3 failures (sizes 4, 5, 20).
- Fuzz: replay of 599 + 25 inputs clean; `fuzz_stored` 8 minutes, 115,516 runs, no report.

## Next

Lead dispatches CI.

## Blocked on

Nothing. SR4 is in `index.hh`, owned by another branch.
