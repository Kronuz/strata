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

## Re-check

Scope `b527e0e..f129435` (15 commits), run in this worktree checked out at `f129435`
(detached; the review file is tracked there and is byte-identical to the one above). Hold:
light (`nice -n 19`, 4 jobs) at the build, free at the later runs. Probes, mutants and logs in
`.scratch/fm/` (`build_probe.sh`, `build_eng.sh`, `keys_mem_probe2.cc`, `ident/`, `cr18mut/`,
`mutants/*/run.log`).

**FM1: CLOSED.** `e3222eb` has parents `b527e0e` and `be64292`; `1cf89c7` and each of
`37ee946`, `9ce600a`, `7bca610`, `2518aae`, `4b15eeb`, `5db24ad`, `e9d6f65` are ancestors of the tip.
1. Run cap: `index_options` no longer has `max_run_bytes`. `grep` finds only the private
   `max_run_bytes_` (`index.hh:2499`, copied to the buffer at `:1876`), the test hook
   (`test/index_test.cc:17`, `index_test_access`) and the buffer's own member
   (`tuple_buffer.hh:199`, asserted `<= run_cap` at `:747`).
2. CR18: `tuple_buffer::doubled` caps the shift with `countl_zero` (`tuple_buffer.hh:108-110`,
   used at `:119` and `:708`). `cr18_shift_probe` rebuilt against the tip under ASan+UBSan:
   clean, prints `last page 69`. With the cap removed in a copy of the header
   (`.scratch/fm/cr18mut/`) the same probe aborts with "shift exponent 64 is too large" at
   `tuple_buffer.hh:109`. The tests that carry it are `tuple_buffer_test.cc:459` (CR18),
   `:401` (CR16) and `:430` (CR17), with `index_test.cc:2188`.
3. Format 13 against main: `git diff be64292 HEAD -- tuple_buffer.hh` is only the key arena
   (`keys_`/`key_ends_` in place of `key_ref`, `extra_keys_`, `batch_keys_`, `key_groups_`,
   `placed_`, `term_at`), as the ledger states; C11's floor and `charge_columns`/`charge_scratch`
   are main's.
4. Fixtures and `digest.txt`: `git diff --stat b527e0e f129435 -- test/fixtures` is empty.
5. ASan+UBSan, all 13 tests rebuilt at the tip: `index`, `tuple_buffer`, `format13`, `merge`,
   `segment`, `fixtures`, `files`, `lifecycle`, `background_merge`, `churn`, `reorder`, `media`,
   `concepts` all print ok, no reports. `parity` on the 10,000 documents: 419 of 419 queries
   equal in counts and top 10, worst relative error 3.87e-16, with 1 segment, 4 segments, and 4
   segments with `PARITY_REORDER=2`; the same as the first review.

**FM2: CLOSED (no action was asked).** The merge of `ff5392e` is unchanged by `e3222eb`'s
conflicts; the ledger records "FM2 needed no change". The suite above passes over it.

**FM3: CLOSED.** `test/index_test.cc:356-363` adds `split_doc` and `:389` mixes it into the
`replace_key` operations of the inversion-on-arrival comparison (`flush_threads` 1 against 2).
1. Mutant m3 (`inverted_plugin::records(docid, part)` with `if (false)` for the `split_text`
   branch, now at `reader.hh:1911`) rebuilt against the tip: `index_test` fails at
   `index_test.cc:399` (`a.digest() == b.digest()`).
2. m1, m2 and m4 rebuilt as well: each fails at `index_test.cc:399`, so none lost its kill.
3. Without the new test: `7298fd3`'s `index_test.cc` against m3's headers passes that section
   and fails only later, at its `:452` check (`c.digest() == b.digest()`). So after the merge
   m3 was already caught by an existing check; the new test adds a direct kill at the digest
   comparison. Either way the condition ("m3 fails a test") holds.

**FM4: CLOSED.** The keys path holds each key once and charges what it holds.
1. Views: `keys_builder::add(f, borrowed)` (`writer.hh:741-751`) stores a 16-byte `keys::entry`
   (`keys.hh:58`); the merge passes `true` (`merge.hh:171`, the key map outlives `finish`), the
   flush paths pass `true` (`writer.hh` `writer_access`, from the index's buffer or the tuple
   buffer's key column, both alive through `w.flush`; `index.hh:2026-2028`), and the public
   `add`/`add_tokens` copy once into `owned_` (`stable_bytes`, `writer.hh:144`, `:793`).
2. In place: `keys::encode(std::span<entry>, flush_charge*)` sorts the span (`keys.hh:72-74`)
   and charges `hashes` plus `filter_build_bytes` (64 B a key + 1 MiB, `keys.hh:68`, `:77`),
   released on exit; `finish` charges the encoded sections (`writer.hh:753-762`).
3. Heap probe: `keys_mem_probe.cc` ported to the new API (`keys_mem_probe2.cc`, a global
   `operator new` hook, charge sampled at each allocation):

   | n | builder after adds, heap | peak through `finish`, heap | peak charge |
   |---:|---:|---:|---:|
   | 100,000 | +3.0 MiB | +9.6 MiB | 10.9 MiB |
   | 1,000,000 | +24.0 MiB | +85.8 MiB | 93.7 MiB |

   Before: +40.0 and +132.2 MiB at 1M. The numbers match the developer's table
   (85.8 / 93.6), and the charge now covers the heap at both sizes.
4. Byte identity (`STRATA_IDENTITY_SEED=13`, `strata_engine index` on the 10,000-document
   corpus, `ea98061` against the tip; `e3222eb` itself predates the seed, so it is not a
   side): default, 2 threads, deletes every 7th, auto merge off, 2 MiB 2 threads with
   deletes, and 1 MiB buffers (2 segments) with and without merges and with 2 threads and
   deletes: every file of every pair equal (8 configurations). `merge` mode does not read the
   seed; merging the 2-segment index with each build leaves 24 differing bytes of a 4.4 MiB
   segment: the two segment-id copies and the checksums that cover them. Keys, versions and
   key index sections are equal.
5. `docs/perf/format13-landing.md:90-94` carries the correction and `:96` onward the FM4
   section; its 1M peak-RSS paragraph says no change is measurable (0.992 [0.893, 1.056]),
   which is what a 46 MiB heap saving against a 200 MiB spread allows. I did not rerun the RSS
   A/B.

New observations (none blocks):
1. `keys_builder::add` now refuses a key past `format::max_term_bytes` (`writer.hh:745`); the
   termdict refused it before, at `b.add`. No test names the refusal (no test in `test/`
   mentions `max_term_bytes`); low.
2. The architect's question in FM4 (a k-way merge of each source's sorted keys, no key map and
   no sort) is still open and is the architect's; it is not a re-check item.

Verdict: FM1, FM2, FM3 and FM4 CLOSED. Nothing in this scope blocks landing. The code
reviewer's read of `e3222eb`'s 14 hunks (FM1's "have that merge reviewed") is not part of
this re-check; the test and probe evidence above is what I confirmed.

Not run: the code reviewer's line-by-line review of the merge's 14 hunks; benchmarks, and RSS
runs of the real 1M index (the heap probe stands in); the developer's 20,000- and
100,000-document identity runs and A/Bs (mine are 10,000 documents); `strata_engine verify`
on the new indexes; LeakSanitizer (not supported by macOS ASan); TSan; the fuzzers; Linux and
FreeBSD locally and CI at `f983012` (not waited on); the gate VM.
