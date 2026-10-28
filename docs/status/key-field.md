> Status: CURRENT (2026-10-28) F3 of fields-and-keys, strata side: delete `key_prefix`. Branch `phase8/key-field`, worktree `strata-wt/f3`; prospect's side is `prospect-wt/f3` on the same branch name.

# key-field

- Design: docs/design/fields-and-keys.md (D6, D7, D8, C1, T5). Prover: developer. Builds under `quiet light f3`.
- Branch / worktree: phase8/key-field from main 5e6af07 at /Users/kronuz/code/Kronuz/strata-wt/f3.

## Done criteria (the lead's brief; the lead alone changes them)

1. `index_options::keyed` replaces `key_prefix`; the manifest, `validate_context`, `merge_options`, `builder_settings`, `segment_writer`, `check_segment` and every plugin `check` drop the prefix; stage 1 has no prefix test; `files::pre_freeze` bumps; no shims.
2. A keyed index is one created keyed; the flag comes back from the manifest on open; a key is any 1 to `max_term_bytes` bytes.
3. Tests: every first byte (0xff, `Q`, NUL) as a key through replace, erase, versions, flush, reopen, merge, scan; each rule has a dying mutant; each fix a revert check.
4. Fixtures and digests re-pinned, proven to differ only in the format bytes.
5. Release, ASan+UBSan and TSan suites pass; manifest decoder fuzzed; kv rows screened against docs/perf/regression-2026-10-28.md.
6. CI green on all 8 jobs; prospect's branch the same.

## State

- State: in progress.

## Log

- 2026-10-28: branch created; core, tests, fixtures migrated; Release suite passing.
