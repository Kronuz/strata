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

## Re-check

At `41539b5`, whose code is `adc82ba`'s (`41539b5` changes only the ledger). The delta from
`b5f375c` holds CR1's fix (`0da5a05`), CR2's fix (`adc82ba`), E33 (`0a5cabe`), and the
removals of E34 (`2db1c54`) and E26 (`96e1f92`). Each removal exactly reverses its
experiment's `tuple_buffer.hh` patch. The head is `git archive 41539b5` in
`.scratch/head-src`. It is built three ways in `.scratch/build-head-{rel,asan,tsan}`:
Release with Apple clang 17 and `-march=native`, then ASan+UBSan and TSan with Homebrew
clang. Outputs are in `.scratch/out-head/`, mutants in `.scratch/mut-head/`.

### Status

- **CR1: closed.**
  - The fix: `sort_run` keeps tied runs on `sort_todo_` (`tuple_buffer.hh:1007-1025`), and
    the radix path passes its ties to it (`:966-977`).
  - Probe: `probe_deep` at the head exits 0 for 1 MiB and 16 MiB terms on the main thread
    and for 64 KiB and 1 MiB terms on a 512 KiB thread. The 1 MiB cases also pass under
    ASan. `b5f375c`'s binary still dies at 1 MiB on the thread (exit 138).
  - The new test (`tuple_buffer_test.cc:268-285`) kills MR6, which puts back one recursion
    per 8 bytes, with SIGSEGV.
  - Margin: the thread case is 8,192 levels, so it catches only frames of 64 bytes or more.
    1 MiB on the thread would not depend on frame size.
- **CR2: closed.**
  - The fix: cf is 64 bits from `encode_entry` through `decode` (`:474`, `:486`, `:735`,
    `:815`).
  - `probe_tuple`'s huge wdfs: 0 of 5 seeds differ (5 of 5 differed at `b5f375c`).
  - `probe_index <seed> 800 <mode> <threads> H`: 9 of 9 runs equal the base. Seed 2 in G1 and T1, which
    differed at `b5f375c`, now matches.
  - The new test (`:286-294`) kills MR7 (cf summed in 32 bits) and MR8 (cf decoded in 32
    bits).
  - `npositions` stays 32 bits. It only sizes a reserve (`:810`), so it cannot corrupt
    output.
- **CR3: open (low).** No test was added. M1 (a token at exactly 2^24 takes the short path)
  survives the suite. So do M2 and M21 (the length check and the exact path), and M15
  (`index::fits` always true).
- **CR4: for the architect, unchanged, not blocking.** The branch adds public API that the
  ledger does not name:
  - `document::text`, `document::tokens`, `split_words`, `segment_writer::add(text, tokens)`,
    `chunked_spill`, and the `tuple_buffer` class;
  - `tuple_buffer::add` does not check its inputs;
  - the new fields break `phase4/layers-refactor`'s `sizeof(document)` pin.

  The delta adds nothing public.
- **CR5, CR7, CR8: open (low), unchanged.** `index.hh:2050`, `:2069`;
  `tuple_buffer.hh:118-119`; `ARCHITECTURE.md:92` and the header comment.
- **CR6: open (low), grown.** E33's `radix_scratch_` (16 bytes a term) and CR1's
  `sort_todo_` (`:1041-1042`) are not charged to `memory` either. `sec_` is charged.

### New findings

- **CR9 (blocking): a batch's run wraps at 4 GiB, unchecked and unnamed.**
  - Where: run offsets are 32 bits in `marks_` and `placed` (`tuple_buffer.hh:675`,
    `:677`). An entry's size and its section lengths are 32 bits too (`:716`, `:769-770`,
    `:784`).
  - Nothing bounds a run below that:
    - `fits` (`:246`) counts only ordinals and side entries;
    - `needs_flush` compares against `buffer_share`, unlimited by default (`index.hh:317`);
    - a batch drains only at 2^20 tuples (`:162`, `:194`).
  - Reaching it: with default options, 1,024 documents of 1,024 distinct 4 KiB terms make
    one run past 4 GiB. One term of `limits.md`'s 2^32 − 1 bytes makes one entry past
    4 GiB.
  - Effect: entries are copied short, and the marks, `placed`, and key pointers land at
    wrapped offsets. Postings come out wrong or reads go out of bounds, the same way every
    time for a given batch.
  - The base keeps terms in a deque of strings and postings in per-term buffers
    (`writer.hh:957-959` at `a62e9f9`), with no 32-bit offset over the buffer.
  - The cap compared with the others:
    - `limits.md` has no row for it.
    - At 2^32, checked, it would be at the largest. Tantivy rejects a per-thread budget
      over about 4 GiB (u32 arena addresses), Lucene flushes a thread at 1,945 MB, and
      Xapian's flush threshold counts documents.
    - It is neither checked nor named.
  - Fix, either one:
    - 64-bit run offsets and entry sizes (the LEB128 lengths keep their bytes);
    - or drain before 2^32 and reject a larger single entry.

    Either way, add a `limits.md` row.
  - Not probed: it needs about 12 GiB resident on this shared 32 GiB host.
- **CR10 (condition, test strength): half of E33's radix sort is untested.**
  - MR1 drops the length-class pass, and MR3 skips sorting the radix path's ties. Both pass
    `tuple_buffer_test` and `index_test`.
  - Both are real bugs. Built against each, `probe_tuple` aborts with "terms must
    increase", and `probe_index ... S` differs from the base on 10 of 10 seeds.
  - `long_ties` with 400 extra terms passes under MR3, because hash order already puts its
    `…a` term first.
  - Fix: a test with 256 or more terms in one batch, compared with `segment_writer`. Use a
    small alphabet, shared 8-byte prefixes, lengths around 8, and several input orders.

### The delta's code

- E33's LSD sort matches `chunk_key::operator<`:
  - each pass is stable;
  - `left` is the least significant digit, and the chunk's bytes go low to high;
  - a pass is skipped only when every key shares its digit, and digit counts do not
    depend on order.
- The E26 removal keeps `prefix()`'s 8-byte load inside an entry: from a term's start, an
  entry has at least 8 bytes (three counts, the flags, four lengths).
- `decode_entry` reads counts from a second cursor on the wdfs when an entry carries none.

### Verified at the head

- **Byte identity with `a62e9f9`:**
  - `strata_engine index` at 20,000 and 100,000 documents, at 20,000 with 2,858 deletes,
    and at 20,000 with keys kept: all files equal.
  - `probe_index`: 180 of 180 runs equal.
  - `probe_tuple`: 0 differences.
- **Release ctest:** 34 of 34.
- **ASan+UBSan:** ctest 34 of 34 (Homebrew clang). Also clean, and equal to the base: `probe_tuple`,
  `probe_deep` at 1 MiB on the main thread and on a 512 KiB thread, and 19 `probe_index`
  runs.
- **TSan:** `probe_ranges` (concurrent `each_term`, `alphabet`, `splitters`) is clean and
  equal. The local ctest had passed 23 of 34 with no reports when time ran out. `tuple_buffer_test` passes on its own with no reports.
- **Mutants:** 20 of 26 killed. The survivors are M1, M2, M15, and M21 (CR3), and MR1 and
  MR3 (CR10).
- **CI:** run 39160732494 at `adc82ba`: seven of eight jobs green, including linux-tsan and FreeBSD. linux-asan-ubsan was still running when this was written.

Verdict: REJECT, on CR9. CR1 and CR2 are closed.
- Blocking: CR9.
- Conditions with its fix: CR10's test, and CI green on all eight jobs at the fixed head.
- After: CR3 and CR5 to CR8 can follow the landing. CR4 is the architect's.

Not run: a probe past 4 GiB (memory), the local TSan ctest's other ten tests (CI's
linux-tsan covers them), CI's linux-asan-ubsan to its end, gate-VM checks, the comment pass
over code outside the delta, and benchmarks (excluded).
