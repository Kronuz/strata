# format13
- Design: docs/design/format-13.md at 1bc0a38 (revision 0; F7 amended with the lengths header CRC at 96083a4; F8 amended with the boolean key term, routing, and digest records at this commit); reviews: none yet (docs/reviews/format-13-design.md not written)
- Branch / worktree: phase4/format13 at /Users/kronuz/code/Kronuz/strata-wt/format13
- State: designing (ready for preliminary review)
- Done: design revision 0 (F1–F22, T1–T11) 1bc0a38; F2–F6 version, kinds, structure flags, footer, file order d1da0c9; F7 statistics split 96083a4; F8 keys and key_index codec (K1), writer, reader, check(), pk_filter retired 360dc7f; F8 key routing: the key leaves the term dictionary, lookups and merges read keys, a boolean key term enforced at stage 1 7c2c9bd; F9 keyless segments and version 0 for keyless documents (this commit)
- Doing: ASan+UBSan run of the decoders added so far (preflight.sh mac, local only; no VMs)
- Next: 1. ASan+UBSan; 2. F22 tools (poststats, keysim); 5. F10/F11/F13 behind presets; 6. ASan+UBSan run of the decoders (preflight.sh mac)
- Blocked on: nothing yet. Open for the lead: which of phase4/format13 and phase4/layers-refactor rebases on the other (design, "The layers branch")
- Findings folded in: none yet
- Last verified: 2026-10-17, macOS arm64 RelWithDebInfo: ctest 34/34 pass after F9; fuzz_segment replay 1090 inputs; CI on the branch pending. Host holds via `quiet` on PATH (the coordinator moved it from cpp-libraries/tools)
