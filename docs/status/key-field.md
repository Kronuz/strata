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

- State: in review (decoder-safety review of the manifest change next); CI pushed, not awaited.
- Done (commits strata:1c782e9 core + tests + fixtures, strata:8494ca3 proofs; later commits on the branch for the ledger and the ab.py fix):
  - `index_options::keyed` replaces `key_prefix`; manifest drops the field (files family 0x80000007); `validate_context::keyed`; `merge_options`, `builder_settings`, `segment_writer`, `check_segment` and the plugins' `check` lose the prefix; stage 1 has no prefix test (keys: keyed index, 1 to `max_term_bytes` bytes; `replace_key`/`delete_key` need a nonempty key); open takes the flag from the manifest and refuses `keyed` on a keyless index. The write buffer's sort head is `key_head`. `git grep key_prefix -- include test bench fuzz tools` is empty (the docs name it as history).
  - `git grep -n key_prefix` in strata's docs/ only: design history, labelled in format-13.md and layers.md.
- Consequence to know: a manifest whose configured bits lack `keys` is now a valid keyless index (the old key-prefix/keys-bit disagreement check is gone with the prefix, per D8); configured bits are set only at create and install.
- Next: the lead's CI watch; decoder-safety review of the manifest (`files.hh`); prospect's branch pins this branch's head.
- Blocked on: nothing.

## Ticks (evidence)

1. Ticked. Above; every caller in strata tests, benches (field_table, idle_index, kv, shared_reader), fuzz targets migrated, no shim.
2. Ticked. `index_test`: keys of all 256 first bytes (plus NUL pair and a Q key) through add, replace with expected version, erase, flush, reopen with default options, force_merge, scan in byte order, `check()`; open mismatch.
3. Ticked. `tools/mutants/keys.py` (8 mutants plus the unmutated copy, `quiet light f3 40 ... tools/mutants/keys.py .scratch/build-rel`): key-first-byte-refused (0xff), key-q-refused, keyless-index-takes-a-key, empty-key-addresses, keyless-index-addressed-by-key, created-keyless-when-asked-keyed, open-keyed-on-keyless, manifest-keeps-a-prefix-field: all killed, the unmutated copy passes. The first run had two survivors: a keyless index addressed by `delete_key`/`replace_key` (test added), and the options write-back on open, which nothing read (deleted).
4. Ticked. Fixtures `test/fixtures/{manifest,live,meta}.bin`: an independent script rebuilds each from the old file as: version byte 0x86 to 0x87, the manifest's `01 51` (length 1, "Q") removed, CRC-32C recomputed; byte-equal to the regenerated fixtures for all three. `files_test` pins 0x80000007 and the 0x80000006 manifest as unsupported. The segment digests and `.sx` fixtures do not change (strata has none for the manifest).
5. Ticked. Release 57/57; ASan+UBSan (LLVM 22, Debug -O1, own tree) 57/57; TSan (own tree) 57/57. Fuzz: `fuzz_files` (manifest, live, meta decoders; libFuzzer with ASan+UBSan), 7 minutes, 9,336,100 runs, no finding; 187 new corpus entries. kv screen (`bench/kv/ab.py rows points`, strict hold, main strata:5e6af07 against this branch, release, Intel Mac): 6 trials, all ten rows tie (B/A 0.956 to 1.053, CIs include 1 or touch it; fillrandom 100 B 0.956 [0.863, 1.052], overwrite 1.053, readrandom 1.020, ycsb_a 0.987, ycsb_f 1.019); a first 3-trial run showed ycsb_f 0.957 [0.928, 0.987] and it did not reproduce at 6; peak RSS and disk equal. Raw: `.scratch/kv2/ab.jsonl` (copied to the main checkout's `.scratch/f3-kv/`). `bench/kv/ab.py` called `row_args` with the old signature; fixed (pre-existing).
6. CI: see Log.

## Log

- 2026-10-28: branch created; core, tests, fixtures migrated; Release suite passing.
- 2026-10-28: mutants, fuzz, sanitizers, kv screen; ledger.
