# f1-ci-runner: why phase8/field-table kills the CI runners

Status: cause found, test-only fix verified (explore/f1-ci).

## Cause

`format::max_term_bytes` is `termdict::max_key_bytes`, about 4 GiB (fst 32-bit depth), and the
field name limit is that value (`include/strata/core/fields.hh:110,213`). `test/fields_test.cc`
built names of exactly that size to probe the boundary:

- rules stage, `std::string(max_term_bytes + 1, 'n')` and `std::string(max_term_bytes, 'n')`
  (the latter is accepted, so the index keeps and copies a 4 GiB name);
- decoder stage, `block({d(std::string(max_term_bytes + 1, 'n'))})`, built by string appends.

Release, macOS, `fields_test` alone: 50 s, 15.0 GB max RSS (21.5 GB footprint). Under ASan/TSan
shadow memory or on a 7 to 16 GB runner it is the OOM killer, reported as "runner has received a
shutdown signal" (exit 143), and it takes the concurrently running tests (ctest -j4: lifecycle,
background_merge, index) down with it. In the failed logs `fields` and `format14` are always
started and never finished.

## Not the cause (Release, main vs branch, same host, strict hold)

| test | main RSS / real | branch RSS / real |
|---|---|---|
| lifecycle | 9.0 MB / 20.9 s | 8.9 MB / 21.0 s |
| background_merge | 22.7 MB / 2.75 s | 17.8 MB / 2.75 s |

No disk growth (faultfs is in memory); per-publication table copies are not visible.

## Fix

Test only: drop the three max-size names; `k` becomes id 0 (`fields_test.cc:325`). With it:
`fields_test` 0.27 s, 2.2 MB. The boundary itself is then untested; a field-name cap like the
65,530 bytes of Tantivy terms (a design call, a format limit) would make it testable at 64 KiB.
