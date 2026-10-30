> Status: HISTORY (2026-10-19) code not on main; its attribution predates the prospect query wins, verify before citing.

# Query gap: the base check (QG-R24)

Whether the experiments' base prints E0's bytes, so `dump` identity can be read on it (D4,
D16). Untimed; `nice -n 19` on the gate VM, 2026-10-19.

- E0's binary: `~/libs/strata-p1/build/strata_engine`, built at `aec8d11` (the binary the
  quick-tier calibration served), SHA-256 `181abfbe…e97e9`.
- The base: this branch at `5b67776` (main `69a3f8c` with the query counters compiled out,
  whose objects are byte-identical to main's per `docs/status/query-counters.md`), Release,
  clang 18, `-march=native`; SHA-256 `55506adb…432cc`.
- Indexes: the reordered merged (`engines/strata-reordered/idx`, `manifest-3`), the
  docid-ordered merged (`engines/strata-docid/idx`, `manifest-3`), and the 16-segment
  (`manifest-2`'s segments, copied to `~/bench/results/qexp-e1/idx16`). The
  `STRATA_DELETE_EVERY` index is not on the VM, so its pair is owed.
- Query files: the gate queries (`queries.txt`, 962, SHA-256 `a8e8f2c1…54bd`) and the
  stratified terms (`terms-stratified.txt`, 200, `79255c23…8e65`).
- E0 made no `dump` references on the VM, so E0's binary made them here, beside the base's.

| Index | Query file | E0 (`aec8d11`) | Base (`5b67776`) | Equal |
|---|---|---|---|---|
| reordered merged | gate queries | `3d669aeb…f78e` | `3d669aeb…f78e` | yes |
| reordered merged | stratified terms | `094a507f…a4c8` | `094a507f…a4c8` | yes |
| docid-ordered merged | gate queries | `3d669aeb…f78e` | `3d669aeb…f78e` | yes |
| docid-ordered merged | stratified terms | `094a507f…a4c8` | `094a507f…a4c8` | yes |
| 16-segment | gate queries | `3d669aeb…f78e` | `3d669aeb…f78e` | yes |
| 16-segment | stratified terms | `094a507f…a4c8` | `094a507f…a4c8` | yes |

Full SHA-256s: `3d669aeb2314f97f40b1b99c98c5f89066e5dc01f367842629d4a542c6dfd78e` (5,772
lines) and `094a507f3246172b99843c795331bbbe8d88c985381b3e62ea0673ecc45ca4c8` (1,200 lines).
The six indexes-by-files print one pair of bytes: the base prints E0's, and the three indexes
print each other's (the 16-segment index's check, D4's "E0 checks the 16-segment one once").
Raw: `~/bench/results/qexp-base/` (`SHA256SUMS`, `phases.log`: 50–71 s per gate-queries
dump, 1–2 s per terms dump).

Verdict: the base passes. Items' `dump` identity is read against these references.
