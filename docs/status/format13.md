# format13
- Design: docs/design/format-13.md at 1bc0a38 (revision 0; F7 text amended with the lengths header CRC); reviews: none yet (docs/reviews/format-13-design.md not written)
- Branch / worktree: phase4/format13 at /Users/kronuz/code/Kronuz/strata-wt/format13
- State: designing (ready for preliminary review)
- Done: design revision 0 (F1–F22, T1–T11) 1bc0a38; F2–F6 version, kinds, structure flags, footer, file order d1da0c9; F7 statistics split (this commit)
- Doing: F8 keys and key_index (K1): format codec in include/strata/keys.hh, writer builds them from add()'s key, reader may_hold_key/find/cursor; pk_filter (kind 11) retired in the same commit
- Next: 1. F8 codec, writer, reader; 2. index key routing (lookup_key through keys) and merge key collection through the keys cursor; the key term leaving the term dictionary; 3. F9 keyless presence; 4. F22 tools (poststats, keysim); 5. F10/F11/F13 behind presets; 6. ASan+UBSan run of the decoders (preflight.sh mac)
- Blocked on: nothing yet. Open for the lead: which of phase4/format13 and phase4/layers-refactor rebases on the other (design, "The layers branch")
- Findings folded in: none yet
- Last verified: 2026-10-17, macOS arm64 RelWithDebInfo: ctest 35/35 pass after F7 (format13_test new; small.sx, permuted.sx, T7 hash re-pinned)
