# format13
- Design: docs/design/format-13.md at 1bc0a38 (revision 0; F7 text amended with the lengths header CRC); reviews: none yet (docs/reviews/format-13-design.md not written)
- Branch / worktree: phase4/format13 at /Users/kronuz/code/Kronuz/strata-wt/format13
- State: designing (ready for preliminary review)
- Done: design revision 0 (F1–F22, T1–T11) 1bc0a38; F2–F6 version, kinds, structure flags, footer, file order d1da0c9; F7 statistics split 96083a4; F8 keys and key_index codec (K1), writer, reader, check(), pk_filter retired (this commit)
- Doing: F8's second half: index.hh lookup_key through segment::find_key; merge.hh's key collection through keys::reader::cursor; then the key term leaving the term dictionary (index stops adding the key as a term; key_of reads the operation's key)
- Next: 1. index key routing and merge key collection; 2. the key term out of the term dictionary, with Q-term query routing; 3. F9 keyless presence; 4. F22 tools (poststats, keysim); 5. F10/F11/F13 behind presets; 6. ASan+UBSan run of the decoders (preflight.sh mac)
- Blocked on: nothing yet. Open for the lead: which of phase4/format13 and phase4/layers-refactor rebases on the other (design, "The layers branch")
- Findings folded in: none yet
- Last verified: 2026-10-17, macOS arm64 RelWithDebInfo: ctest 34/34 pass after F8 codec (fixtures and T7 hash re-pinned; fuzz_segment replay 1090 inputs)
