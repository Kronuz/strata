# Code review: perf/inverter at b5f375c

Range `a62e9f9..b5f375c` (50 commits; `a62e9f9` is on main, `b5f375c` is on origin). The
ledger has no done criteria set, so the review checks the ledger's claims (byte-identical
segments, the kept series E23 to E26, E31, E32) and the lead's six focus items. The
commits after the cut (`0a5cabe` E33, `c6bf4d9` E34, and the rest) are not reviewed.

## Interim (paused)

Paused on the coordinator's instruction (over quota) before the sanitizer runs and the
mutation runs. Probes are in `.scratch/probes/` of this worktree. The base build is
`git archive a62e9f9` built in `.scratch/build-base`. The cut's build is
`.scratch/build-rel`. Both use Apple clang 17, Release, `-march=native`, macOS x86_64.

### Findings so far

- **CR1 (blocking): the batch term sort overflows the stack on valid input.**
  `tuple_buffer.hh:960-971`. `sort_run` recurses once for every 8 bytes that tied terms
  share (the recursive call is at `:967`). It runs from `sort_groups` (`:956`) in
  `drain()` (`:665`), on the apply or refresh thread.
  - Failure: one document carries two terms of N bytes that differ only in their last
    byte. On the main thread (8 MiB stack), N = 1 MiB crashes with SIGSEGV (exit 139);
    512 KiB passes. On a `std::thread` (512 KiB stack), N = 64 KiB crashes with SIGBUS
    (exit 138); 32 KiB passes.
  - Base: `a62e9f9` handles N = 16 MiB on the main thread and 64 KiB on a thread.
  - Backtrace (lldb): `sort_run` at `depth=1044968` (`:961`), and frame #100000 is still
    `sort_run` (`:967`).
  - Why it blocks: `limits.md` sets term bytes at 2^32 − 1, and index_test claims 64 KiB
    and 1 MiB terms work. The crash is deterministic, so replaying that log entry crashes
    every replica.
  - Probe: `probe_deep.cc` (`bin/probe_deep_cut N [thread]`, against `bin/probe_deep_base`).
  - Fix: an explicit stack instead of recursion, or a full-term comparison after a few
    chunks. Add a regression test with two 1 MiB terms on a 512 KiB thread.
- **CR2 (blocking): a run entry's collection frequency is 32 bits.**
  - Where: `tuple_buffer.hh:733` (`cf` is `std::uint32_t`) and `:768` (`cf += wdf`);
    `entry_view::cf` at `:474`, decoded at `:485` and summed at `:815`.
  - The format stores cf in 64 bits (`format.hh:290`, `:431`), and the base sums it in 64
    bits (`writer.hh:706`, `:760`).
  - Failure: two documents in one batch each give term `x` an explicit wdf of 0x90000000.
    Each wdf is within `limits.md`'s 2^32 − 1.
  - Result: the flushed term info says cf = 536,870,912 instead of 4,831,838,208, and
    `seg-*.sx` and the manifests differ from the base's.
  - Paths disagree. With one batch per document the value is right. `relabel` (`:850`)
    recomputes cf in 64 bits, so a flush that drops documents writes the right value. A
    merge repairs it (`merge.hh:236`). The digest omits cf, so digests stay equal.
  - Probes: `probe_tuple` parts 3 and 4 (5 of 5 seeds differ). `probe_index ... H`: seed 2
    differs from the base at `seg-1.sx` in modes G1 and T1, and agrees in G2.
  - `npositions` (`:733`) has the same width, but only an input of 4G positions in one
    batch reaches it.
  - Fix: make the entry's cf 64 bits. The header is LEB128, so the bytes do not change.
- **CR3 (test strength, mutations still to run): two shipped tests are missing.**
  - No test puts a token at exactly position 2^24. `test/tuple_buffer_test.cc:50` only
    lands at 2^24 + 1 or above. The code is right there (`probe_tuple` part 2).
  - No test forces a full 64-bit collision on the ≤ 8-byte exact path
    (`tuple_buffer.hh:914-915`). `hash()` is unkeyed and invertible: the 8 bytes
    `39 08 1b fa 24 51 17 fa` hash like `a`. The length check keeps them apart, and the
    output is correct (`probe_tuple` part 5).
- **CR4 (for the architect): the public API change is larger than the ledger says.**
  - The ledger names `split_text` and `add_split`. Against main, the branch also adds:
    - `document::text`, `document::tokens`, and `text_token`;
    - `split_words`, `count_words`, and `word_counts`;
    - `segment_writer::add(text, tokens)` (`writer.hh:473`) and `chunked_spill`;
    - the whole `tuple_buffer` class.
  - `tuple_buffer::add` does not check its preconditions. Given a token that repeats a
    terms entry, it writes corrupt position deltas where `segment_writer::add` throws.
  - The new fields change `sizeof(document)`. `phase4/layers-refactor` pins it with
    `static_assert(sizeof(document) == sizeof(detail::document_12))` (its `reader.hh:719`),
    following layers L10 and R11.
  - My view: `split_text` is additive and small. It belongs in the inverted plugin's part
    (L10). `tuple_buffer` should move to a detail namespace or document that its inputs
    are unchecked.
- **CR5 (low, read only): a failed flush can leave a partial file.** `index.hh:2050`
  creates the segment file before writing it. A failure that is not a `system_error`
  (`bad_alloc`, `invalid_argument`) leaves a partial file that nothing tracks until
  reopen. `:2069` takes the footer CRC back from the file instead of comparing it with the
  bytes produced, as `written_segment` did.
- **CR6 (low, read only): some of the buffer's scratch is not charged (D17).**
  `group_scratch`, `groups_`, `keys_scratch_`, `order_`, `entry_at_`, `key_groups_`,
  `placed_`, and `extra_keys_` (`tuple_buffer.hh:986-994`, `:1004`) are never reported to
  `memory`. That is a few MiB per buffer at the default batch.
- **CR7 (low, read only): the hash assumes little-endian.** `hash()` masks the low bytes
  (`tuple_buffer.hh:118-119`), which is right only on little-endian hosts, while the same
  branch writes big-endian cases elsewhere. Add a `static_assert`, or a mask that works on
  both.
- **CR8 (low): two docs items.** `ARCHITECTURE.md:92` still describes stage 2 as a term
  hash map with posting slices. The header comment of `tuple_buffer.hh` runs 11 lines.

### Verified so far

- **Corpus byte identity against `a62e9f9`.** `strata_engine index` at 20,000 and 100,000
  documents (`STRATA_BUDGET_MB=2056`), and at 20,000 with `STRATA_DELETE_EVERY=7` (2,858
  deletes): segment, manifests, and meta are identical.
- **`probe_index` against the base: 180 of 180 runs identical.**
  - Inputs: 30 seeds, each with 1,500 transactions.
  - Six modes: grouped terms, tokens, and `split_text`, each at one and two flush threads.
  - Transactions: replace and delete by key and by docid, and rejected transactions.
  - Refreshes and commits fall at random boundaries, and a force merge runs at the end.
  - Every file after every commit, every digest, and every verdict matched.
- **`probe_tuple`: 0 differences.**
  - 20 seeds, each at batch sizes 1, 2, 3, 13, 64, 1000, and 2^20.
  - Hashes full and narrowed, with plans that drop and reorder documents.
  - Edge inputs: positions near 2^24 and near 2^32 − 1, NUL and high bytes, and many
    fields.
- **Release ctest: 34 of 34.** `tuple_buffer_test` also passes under local ASan+UBSan
  (Homebrew clang 23).
- **CI run 39125555615 at `b5f375c`: green on all eight jobs.** These include ASan+UBSan,
  TSan, and FreeBSD.
- **The removed experiments E27 to E30 left no code.** `297464f..deb2ed2` differs only by
  `ff36290`.

### Still unchecked

- **ASan+UBSan:** the full suite and the probes (built in `.scratch/build-asan`, not run).
- **TSan:** concurrent `each_term`, `alphabet`, and `splitters` (`probe_ranges.cc`), not
  built.
- **Mutations:** `mutate.py` holds 20 mutants; none has run.
- **Limits:** the 32-bit run offsets (`marks_`, `placed_`, `entry_ref::size`) cap a run at
  4 GiB, a cap that names no mechanism; not probed.
- **Remaining passes:** a full comment-style pass, and the post-cut commits.

Verdict so far: REJECT, on CR1 and CR2. The remaining checks can add findings, but they
cannot clear these two.

Not run: the ASan+UBSan suite beyond `tuple_buffer_test`, TSan, the mutations, gate-VM
checks, benchmarks (excluded).
