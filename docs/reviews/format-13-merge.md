# Format 13 merge onto main: code review (C16 item 1)

Branch `phase4/format13-on-main` at `d5bab6b` (detached worktree `strata-wt/review-f13-merge`).
`origin` is at `43ee403`, one ledger-only commit ahead (`docs/status/format13.md`, 7 lines); no
unpushed code. Scope: the merge `ff5392e` (`0954761` with main `69a3f8c`), `76b6b5a`, and the code
commits through `19ae896`. CI run 39238598579 at `09bf626` is green on all eight jobs (FreeBSD,
macOS x86_64 and arm64, Linux gcc/clang/arm64, TSan, ASan+UBSan).

Probes, mutants and logs are in `.scratch/fm/` of this worktree:

- `cr18_shift_probe.cc`: main's CR18 test (70 log pages of 1 MiB + 1) against this branch.
- `keys_mem_probe.cc`, `keys_mem_probe.out`: heap held versus charged on the merge's keys path.
- `mutants/` (`build_mutant.sh`, `m1`-`m4` patches, each mutant's `run.log`): the digest resolution.
- `asan/*.log`: 13 tests under ASan+UBSan. `parity*.out`, `verify-*.out`, `index10k*.out`.
- The ASan build tree is `.scratch/build-asan` (Apple clang, `-O1`, as CI).

## Findings

- **FM1 (high, blocking): main's inverter follow-ups are not on the branch.** The merge's main
  side is `69a3f8c`. The follow-ups landed on main afterwards, in `1cf89c7` (the merge of
  `phase4/inverter-followups`), and `git merge-base --is-ancestor 1cf89c7 HEAD` is false. None of
  the four the review was asked to confirm is present:
  1. The run cap as a test hook (`37ee946`, R28.1, CR9). The public `index_options::max_run_bytes`
     is still at `index.hh:323-324` and is copied unchecked at `index.hh:1894`, so CR9's 4 GiB
     wrap is still reachable from a public field.
  2. CR18's capped shift (`9ce600a`). `tuple_buffer.hh:96` (log pages) and `:648` (arena chunks)
     still compute `4096 << n`. `cr18_shift_probe` fails under UBSan with "shift exponent 64 is
     too large", at `tuple_buffer.hh:96:73`.
  3. C11's buffer floor (`7bca610`).
  4. CR16's split-text limit (`2518aae`).

  R27, R28, R29, R32.1 (`4b15eeb`, `5db24ad`) and CR17's tests (`e9d6f65`) are missing too. A
  trial `git merge-tree HEAD main` (`be64292`) conflicts in 14 hunks: `index.hh` 6, `reader.hh` 5,
  `tuple_buffer.hh` 3. Those hunks need real resolutions, not mechanical ones:
  1. Main's `key_of` now returns a view into the key term. Format 13's key is `core_part::key`,
     and `pending_docs_` must point into `o.doc.key`, never into a temporary `optional<string>`.
  2. Main's multi-thread flush keeps `split_text` and calls `add_split` (main `index.hh:1812`,
     `:2008`). That makes the split branch of `inverted_plugin::records(docid, part)` live (FM3).
  3. C11's `charge_columns` and `76b6b5a`'s key arena both rewrite `charge_columns`, and main's
     key-placement code (`key_groups_`) is code `76b6b5a` deletes.

  So the C16 item 1 check (nothing from main's follow-ups lost) cannot pass on this branch yet.
- **FM2 (none): the 16 hunks of `ff5392e` keep both sides.** I recomputed the conflicts with
  `git merge-tree 0954761 69a3f8c` (`index.hh` 5, `reader.hh` 9, `merge.hh` 1, `strata_engine.cc`
  1; `merge_oracle.hh` and its test modify/delete, both correctly left deleted under LC4) and
  diffed them against the resolution:
  1. Main's `hash_slots` version map, its active buffer and `pending_docs_` all stay, with keys
     taken from `doc.key` (`index.hh:1806-1809`, `:1837`).
  2. One hash per key lookup across segments (CR7, `index.hh:1571-1572`).
  3. `merge_env` carries `configured_structures()` (`index.hh:1054`).
  4. Both writer paths set `key_prefix` and `structures` (`index.hh:1969-1970`, `:2036-2037`).
  5. `validate` keeps main's sorted-terms and token checks together with F8's refusal of
     key-prefixed terms (`reader.hh:304-319`).
  6. The F21 digest records for the active buffer: the digest is a sum of record hashes, so the
     per-document D/N/S interleaving is order-free. The key invariant moved into
     `document_record` (`index.hh:674`).
  7. The `strata_engine` `fill_document` change and the test edits (keys moved from terms to
     `doc.key`, version 0 for keyless documents) weaken no assertion. Fixtures and `digest.txt`
     are byte-identical to `0954761`.

  Mutants m1 (no active-buffer N record), m2 (no active-buffer P records) and m4 (no token
  postings in `records(docid, part)`) each fail `index_test.cc:382`.
- **FM3 (low): an unexercised branch.** Mutant m3 disables the `split_text` branch of
  `inverted_plugin::records(docid, part)` (`reader.hh:1895-1898`) and passes all of `index_test`.
  The branch is unreachable on this branch, because `index.hh:1810-1814` converts split text to
  tokens before it is buffered. After FM1's merge it becomes reachable (main buffers split text
  as is), so m3 must then fail a test.
- **FM4 (medium, memory; for the developer, with a design question for the architect): the keys
  path holds each key three times, and `finish()`'s working set is not charged.**
  `keys_plugin::merge` (`merge.hh:164-174`) copies every key from the merge's `key_map`
  (`merge.hh:271-281`) into `keys_builder`'s `std::string` pairs (`writer.hh:717`, 40 B a key).
  `finish()` (`writer.hh:722-730`) then copies them into `entries` and `keys::encode`
  (`keys.hh:57-92`) adds `hashes`, the fuse filter build and the termdict builder. None of that
  is charged. `keys_mem_probe` at 1M keys measures:
  1. `key_map`: 58.3 MiB of heap, 68.7 MiB charged.
  2. `keys_builder` after its adds: +40.0 MiB.
  3. The peak during `finish`: +132.2 MiB over the `key_map`, of which 47.6 MiB is charged, so
     about 80 MiB of the peak is never charged.

  At 100k keys the same peak is +13.9 MiB. The flush runs the same `finish()`
  (`writer.hh:946`). Format 12 streamed key terms through the term dictionary. This gap matches
  in size the 1M peak-RSS rise in `docs/perf/format13-landing.md` (715.0 to 801.4 MiB, +86 MiB),
  but I did not attribute it with a measurement of the real run. It does contradict that note's
  statement that the key paths are "charged to the budget, so this is peak RSS, not a budget
  overrun".

  Fix (developer):
  1. Let `keys_builder` hold views into the `key_map` (in a merge) or the tuple buffer's key
     arena (at a flush).
  2. Sort in place rather than copying into `entries`.
  3. Charge `hashes` and the filter build.

  Question for the architect: each source's keys already come sorted (`each_key` walks the
  FST), so a k-way merge would need neither the `key_map` nor a sort.

## Verified

- ASan+UBSan: 13 tests pass with no reports. They are `index_test`, `tuple_buffer_test`,
  `format13_test`, `merge_test`, `segment_test`, `fixtures_test`, `files_test`,
  `lifecycle_test`, `background_merge_test`, `churn_test`, `reorder_test`, `media_test` and
  `concepts_test`. macOS ASan has no LeakSanitizer, so leaks were not checked.
- `parity` on the oracle's 10,000 documents: 419 of 419 queries equal in counts and top 10, worst
  relative error 3.87e-16. The same holds with 4 segments and with `PARITY_REORDER=2`.
- `strata_engine verify` on 10,000 documents (`STRATA_BUFFER_MB=4`): 1,257 queries with 0
  mismatches each time:
  1. Merged into 1 segment.
  2. With `STRATA_DELETE_EVERY=7` (1,429 deleted).
  3. With `STRATA_AUTO_MERGE=0` (9 segments).

## Verdict

**CONDITIONS: not landable at `d5bab6b`.**

- **Blocking:** FM1. Merge main (`be64292`, which contains `1cf89c7`) into the branch, resolve its
  14 hunks keeping format 13's core key, and have that merge reviewed. A rechecker can rerun
  `cr18_shift_probe` (it must pass), check that `max_run_bytes` is gone from `index_options`, and
  run main's C11, CR16 and CR17 tests under ASan+UBSan.
- **Conditions:**
  1. FM3: after that merge, mutant m3 fails a test.
  2. FM4: before the freeze, either fix the keys path's copies and uncharged working set, or get
     the architect's ruling on it. Correct `docs/perf/format13-landing.md`'s "charged to the
     budget" sentence either way.

The resolution of `ff5392e` itself (FM2) needs no change.

Not run: benchmarks or RSS runs of the real 1M index (FM4 is a heap probe only), LeakSanitizer
(not supported by macOS ASan), TSan, the fuzzers, the trial merge's build (I resolved none of its
hunks), Linux and FreeBSD locally (CI covers them at `09bf626`), and the gate VM.
