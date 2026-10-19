# strata review: format 13, step 7c (code)

- Date: 2026-10-18.
- Reviewed: `f2c19c6` on `phase4/format13`, `git log d508158..f2c19c6` (base: layers
  `d508158`): 7c-1 `b8d9ac9`, 7c-2 `e6b2144`, 7c-3 `86225aa`, 7c-4 `da6e9a6`, 7c-5 `bd69714`,
  7c-6 `f92e80b`, 7c-7 `2fe7e85`, T7 `ae42746`, and the sanitizer fixes `d00e587` and
  `82060cc`, with every diff in `include/`, `test/`, `fuzz/`, and `tools/`. Built and probed in
  a detached worktree at `f2c19c6` (`strata-wt/format13-review`), never the branch name.
- The developer's fold `23716c5` (pushed during this review) has the same tree as `f2c19c6`
  (`cf085de`), so every finding applies to it unchanged.
- Read first: `docs/design/format-13.md` (revision 1 with both re-checks' amendments),
  `docs/status/format13.md` (K1 to K14, step 7c and its deviations), `AGENTS.md`, `docs/PLAN.md`,
  and `docs/reviews/calibration.md` at main `088a64f`, whose checks were applied: CI on every
  platform for the reviewed commit; `HEAD` against `origin` (when the review began, `origin` was
  at `f2c19c6` while the developer's worktree already held the unpushed fold `23716c5`, pushed
  during the review); one implementation per mechanism (no thread, queue,
  cache, or encoder is added: keys reuse `termdict`, `fusefilter`, and `xxh64`); digest inputs
  from replicated state (the length record keys on the manifest's configured bits, not the mask).
- Clean room: no Xapian or Xapiand source read. `fst` and `termdict` (Kronuz, MIT) read for one
  throw site.

## Interim (paused)

Paused at 08:18 on the coordinator's quota stop. Everything below is final except the local
sanitizer suite: 28 of 36 tests pass under ASan, UBSan, and LeakSanitizer, none fails, and
eight were stopped unfinished (`lifecycle`, `lifecycle_reordered`, `index`, `background_merge`,
`faultfs`, `search`, `cost`, `engine_query`), all of them green in CI's `linux-asan-ubsan` at
`82060cc` and `23716c5`. Still unchecked: those eight locally, and the "Not run" line's items.
The verdict below was reached before the pause and stands; the eight could change it only by
failing.

## Summary

The on-disk layout matches the design: an independent decoder written from F2, F3, F5, and F6's
text accepts all nine pinned fixtures. No memory error turned up anywhere: not in the full suite
under ASan, UBSan, and LeakSanitizer, not in 1,600 resealed mutations of `keys`, `key_index`, and
`versions` driven through every keys read path, and not in any absent-structure profile.

The findings concern four things:

1. The order in which an open classifies structure bits (CR1).
2. What the fuzzer and `check()` never reach (CR2, CR3, CR4).
3. Unvalidated configured bits in the manifest (CR5), and a key that the buffer and the disk
   estimate no longer count (CR12).
4. Test strength under mutation (CR6), and two lookup and memory costs (CR7, CR8).

One finding is about landing rather than the code: format 13 keeps version 13 while its bytes
keep changing until the freeze (CR11). Nothing blocks; CR11 is due before 7c lands on main, and
CR1 and CR2 before the freeze.

## What was run

The host: macOS 15.6 on an Intel i9-9980HK (x86_64, 4 KiB pages), Homebrew LLVM 23.1.2. Every
build and run used `nice -n 19` and at most four jobs under other agents' light holds, and was
paused (SIGSTOP) during four strict holds (`si-m2b`, `si-m2b2`, `si-m2b3`, `sched-k1-tiny`),
resuming only once `quiet check` no longer exited 1. All probes are in `.scratch/` of the review
worktree (`.scratch/watch.sh` did the pausing).

| What | Result |
|---|---|
| Build: Debug, `-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer`, libc++ hardening at its debug mode (CI's flags, with libc++'s counterpart of `_GLIBCXX_ASSERTIONS`) | Builds. A `-fsyntax-only -Wall -Wextra` pass over `format13_test`, `index_test`, `fuzz_segment`, and `strata_engine` gives the same five `-Wmissing-field-initializers` in `index.hh` as the base `d508158`; none is new |
| `ctest`, the whole suite, under that build with `ASAN_OPTIONS=detect_leaks=1` | 28 of 36 pass, none fails: #1 to #16, #19 to #24, #30 to #33, and the two fuzz corpus replays (#35, #36, run directly). Stopped unfinished by the coordinator's pause: #17 `lifecycle`, #18 `lifecycle_reordered`, #25 `index`, #26 `background_merge`, #27 `faultfs`, #28 `search`, #29 `cost`, #34 `engine_query`. CI's `linux-asan-ubsan` runs all 36 green at `82060cc` and `23716c5` |
| CI: run 39126687560 at `82060cc`, whose code is `f2c19c6`'s (only the ledger differs) | All eight jobs green; and run 39130677071 at the fold's head `23716c5`, all eight green |
| `layout_check.py`: an independent decoder written from F2, F3, F5, and F6's text (its own CRC-32C) over every format-13 fixture | 9 of 9 match: prefix CRC `0x4ff2df7b`, a 36-byte header with zero flags, the 56-byte footer with a zero reserved word, every section at the previous one's 8-byte-aligned end, zero padding, F3's rank order, F4's presence both ways, `stats` at 8 bytes, `lengths`' size, the key range |
| `probe_bits.cc`: reserved structures laid out by F3's rule | CR1 |
| `probe_keys_fuzz.cc`: 1,600 resealed mutations of `keys`, `key_index`, `versions` through every keys read path, ASan and UBSan | No memory error, hang, or out-of-range ordinal; CR2, CR4 |
| `probe_check.cc`: inconsistent `keys` and `key_index` with good checksums | CR3 |
| `probe_absent.cc`: eight structure sets (all four; `document`; `log`; without `stored`; without `norms`; `keys` only; `stored` only; none) at 0, 1, and 200 documents, each through `check_segment`, key leaves and terms by the term table, a phrase and a conjunction over a key leaf, the digest records, `verify_all`, and merges into four configurations, under ASan, UBSan, and LeakSanitizer | All pass; every merge writes exactly its configuration's bits |
| `probe_manifest.cc`: configured bits against F4 and the prefix | CR5 |
| `probe_keybytes.cc`: what the buffer and the disk estimate charge for a document's key | CR12 |
| `fix/CR1/`: CR1's proposed open order in a scratch copy of `include/strata` | `probe_bits` says `unsupported_segment` for every row; the existing `format13`, `segment`, `fixtures`, `index`, and `media` tests pass |
| Mutation testing (`mutants.py`, `mutrun.sh`): 16 mutants of one edit each, each against the tests that should kill it, non-sanitized at `-O1` | CR6 |
| Sweeps: a view into a temporary (the two CI ASan bugs' pattern) in tests, tools, fuzz targets, and the library's `string_view` members; format 12's names; comment length | No new view into a temporary; CR9 |
| Hot paths, by inspection: every changed line on a per-term or per-posting path | One per-posting change: `length()` reads its width from a relaxed atomic in the heap block whose acquire load it already makes (`reader.hh:321-322`). One per-term change: the planner's prefix test (`searcher.hh:108`). No allocation per term or posting; the costs found are per lookup and per open (CR7, CR8) |

## Findings

### CR1. Unreadable structure bits are classified after the table, so a later release's structure reads as damage (F4; F23 rules 3 and 5; T1)

- `core_segment`'s constructor decodes the table (`reader.hh:128`; `format.hh:350, 359-360,
  369`) before `check_structures` classifies the footer's bits (`reader.hh:130`;
  `format.hh:378-384`). `decode_toc` checks every entry's kind and rank against this binary's
  table, where the kinds it cannot place are `unranked` (`format.hh:169-180`). A reserved
  structure laid out by F3's own rule ("a pinned kind after its owner's pinned sections, any
  other after its owner's streamed ones") therefore fails as `corrupt_segment: section order`
  before its bit is looked at.
- F23 rule 5: a binary without the structure's reader refuses "such a segment with
  `unsupported_segment`, never as corruption". F23 rule 3, for `compact`: the table's "entry
  form may change in segments that set the bit ... so a reader reads the bit before the table".
  F4 says the same of unlisted bits ("a later release's structure (F23), not damage").
- Probe `.scratch/probe_bits.cc`: a good keyed segment laid out again with one more section,
  every checksum resealed.

  | Segment | This binary says |
  |---|---|
  | bit 8 set, `pair_summaries` (kind 18) after `lengths` | `corrupt_segment: section order` |
  | bit 8 set, the same section after `key_index` | `unsupported_segment: structure bit 8` |
  | bit 9 set, `stored_dictionary` (19) after `stored` | `corrupt_segment: section order` |
  | bit 11 set, `quantized_impacts` (21) after `term_index` | `corrupt_segment: section order` |
  | bit 12 set, `key_hash` (22) after `keys` | `corrupt_segment: section order` |
  | bit 15 set, the table unchanged | `unsupported_segment: structure bit 15` |
  | bit 15 set, a table 8 bytes longer (another entry form) | `corrupt_segment: table of contents trailing bytes` |
  | bit 20 set, a table 8 bytes longer | `corrupt_segment: table of contents trailing bytes` |

  The same structure is unsupported or damage depending on where its section sits.
- In an index nothing changes today: `check_readable` refuses the manifest's mask first
  (`index.hh:2060-2063`). What breaks is the lone-segment contract, which this reader freezes
  for every later format-13 release.
- Test gap: T1's "each reserved bit set" (`format13_test.cc:369-391`) sets the bit in the footer
  and adds no section, so it passes without the layout a release would write.
- Fix: classify the footer's bits right after `decode_footer` (the requires rule, then `structures
  & ~readable` as `unsupported_segment`), then decode the table, then check presence; split
  `check_structures` into those two halves. T1 gains the rows above, each expecting
  `unsupported_segment`. Before the freeze (K1).
- The fix is checked. A scratch copy with that order (`.scratch/fix/CR1/`: `check_bits` right
  after `decode_footer`, `check_presence` after the table, `check_structures` kept as the two
  together) turns all eight rows of the probe into `unsupported_segment`, and passes the existing
  `format13`, `segment`, `fixtures`, `index`, and `media` tests (`mutrun.sh FIX_CR1`, at `-O1`).

### CR2. No fuzzing reaches the keys decoder (F8; T2, K2)

- `fuzz_segment`'s `exercise()` (`fuzz/fuzz_segment.cc:103-113`) calls only `version` and
  `may_hold` on the keys reader, and `may_hold` reads only the range and the filter. Nothing calls
  `find`, `scan`, the cursor, or `check_segment`, so the fuzzer never decodes the `keys` section's
  `termdict` blocks or any ordinal value (`keys.hh:142-207`). The checked-in corpus
  (`fuzz/corpus/segment`, unchanged in the range) was grown on format 12's layout.
- K2 is unticked, so this is a condition on an open criterion, not a false claim. My probe found
  no defect. `.scratch/probe_keys_fuzz.cc` flips 1 to 4 bytes inside `keys`, `key_index`, or
  `versions` and reseals every checksum (`refresh_checksums`, with the prefix). It then runs
  `may_hold`, `find` with and without live bits, `scan` from fixed and random bounds with early
  stops, `version` for every ordinal, the cursor, `check_segment`, and a merge, all under ASan and
  UBSan. Over 1,600 inputs (seeds 1 and 2): no sanitizer report, no hang, no leaked
  `termdict::corrupt`, and every ordinal handed to a callback below the document count. 171
  inputs ended in `invalid_argument` (CR4).
- Fix: `exercise()` calls `find` (a hit, a miss, with live bits), `scan` from a few bounds with an
  early stop, the cursor, and `check_segment`; a section selector for `keys` and `key_index`; a
  corpus grown on format 13's bases. With K2, before the freeze.

### CR3. `check()` accepts keys the reader then cannot find (F8)

- `check_segment`'s key walk (`check.hh:44-53`) checks order, count, and each ordinal's range
  through the cursor. It compares nothing with `key_index`'s range and filter, and it lets two
  keys name one ordinal, which `keys.hh:30` rules out ("a document has at most one").
- Probe `.scratch/probe_check.cc`: ten documents keyed `Q0` to `Q9`, sections rebuilt with every
  checksum good.
  - Two keys on ordinal 0 (`Q0`, `Q1`): `check()` passes, and both `find`s answer 0. A merge of
    that segment alone keeps 9 keys and loses `Q0`, because `merge.hh:269` overwrites
    `keys[s][o]`.
  - `key_index`'s minimum is `Q1` while `Q0` is held: `check()` passes, and `find("Q0")` misses.
  - A filter built over other keys: `check()` passes, and `find` misses all 10 held keys.
- A missed key is the costly case. `lookup_key` (`index.hh:1401-1412`) misses it, so an upsert
  adds a second live copy, which the digest's key invariant reports only later. Only a writer bug
  or a resealed input produces such a segment, and `check()` is the tool meant to catch a writer
  bug.
- Fix: in the walk, the first key equals `min()`, the last equals `max()`, `may_hold(key)` holds
  for every key, and no ordinal repeats; the merge's key collection refuses a repeated ordinal as
  `corrupt_segment`. It costs one filter probe per key.

### CR4. A malformed `key_index` FST escapes as `std::invalid_argument`

- `keys::reader`'s constructor (`keys.hh:124-128`) catches `termdict::corrupt` but not the
  `std::invalid_argument` that `fst::map` throws from its own validation (`fst.hh:151, 156, 441`
  at the pinned `24da59a`). The inverted reader converts both (`reader.hh:297-303`, "the FST's own
  check").
- `key_index` is pinned and CRC-checked whole at open, so media damage fails as corruption first,
  but a resealed input or a writer bug does not. `index::open` then sees `std::invalid_argument`,
  which `read_checked` neither retries nor turns into damage (`index.hh:2041-2057`). It is also
  the type `index::open` throws for an operator error ("the index's key prefix differs",
  `index.hh:1996`).
- Probe `probe_keys_fuzz`: 41 of 400 inputs at seed 1 and 130 of 1,200 at seed 2 end in
  `invalid_argument: fst: malformed bytes` (one `fst: not an FST`), all from `key_index`.
  `fuzz_segment` counts `invalid_argument` as a fence (`fuzz_segment.cc:203`), so the fuzzer
  cannot flag it.
- Fix: catch `std::invalid_argument` beside `termdict::corrupt` there, as the inverted reader
  does. Then consider dropping `invalid_argument` from `fuzz_segment`'s fences: the other throws
  I found are on the writer side (`docids.hh:123-125`, `keys.hh:63-64`), so the next read path
  that lets one out would become a finding instead of a pass.

### CR5. The manifest's configured bits are not validated (F20, F13-R28; F4, F8)

- `decode_manifest` checks only that the configured bits lie within the mask (`files.hh:230`),
  and `check_readable` checks only readability (`index.hh:2060-2067`). A manifest can therefore
  open with configured bits that break F4's requires rule, or with `keys` cleared under a nonempty
  prefix (F8: "the prefix is empty exactly in a keyless index"). Stage 1 then accepts writes that
  no flush can write, because `segment_writer` refuses them (`writer.hh:804, 816, 871-872, 888-890`).
  `install` adopts the source's bits the same way (`index.hh:623, 672`).
- Probe `.scratch/probe_manifest.cc`: a committed keyed index, its head manifest rewritten, then
  open, an upsert, and a commit.

  | Manifest | Result |
  |---|---|
  | as written | opens, applies, commits |
  | `keys` cleared, prefix `Q` | apply accepted; commit throws `a key in a segment without keys` |
  | `inverted` cleared, `norms` kept | apply accepted; commit throws `terms in a segment without inverted` |
  | `stored` cleared | opens, applies, commits (a valid configuration) |

  The index is wedged with accepted transactions it can never flush. Only a resealed manifest or
  another binary's bug writes such a manifest, but `configured` now arrives from a peer through
  `install`.
- Fix: `decode_manifest` refuses configured bits that fail `requires_of` (`corrupt_index`), and
  open and `install` refuse a prefix that disagrees with the `keys` bit for the layout. While
  there: the `slots` reference is accepted with any generation (`files.hh:225`), though no slot
  table exists. Refuse a nonzero one as `values_generation` is refused (`files.hh:247-248`), or
  state why not.

### CR6. Mutation testing: what the tests let through

Sixteen mutants of the code under review, one edit each (`.scratch/mutants.py`), each built at `-O1`
without sanitizers and run against the tests that should kill it (`.scratch/mutrun.sh`). The
unmutated tree passes all of them. A mutant is killed when one of its tests fails; each kill was
checked to come from the check the mutant breaks. Ten are killed and six survive.

| Mutant | What it breaks | Tests run | Result |
|---|---|---|---|
| M8 | the requires rule runs after the unreadable-bit check | format13 | killed (`format13_test.cc:377-381`) |
| M29 | the footer's reserved word is unchecked | format13 | killed (`:358`) |
| M30 | `check()` accepts a version without a key | format13 | killed (`:705`) |
| M31 | open ignores the manifest's version range | format13 | killed (`:484`) |
| M32 | the versions writer drops the minimum rule | format13 | killed (`:682`) |
| M35 | F2's rule 3 dropped: a format-12 file is damage | format13 | killed (`:259, 269`) |
| M34 | the planner sends key-prefixed terms to the dictionary | format13 | killed (`:782`) |
| M6 | the `D` record drops the key | fixtures | killed (`digest.txt`) |
| M10 | `lengths` skips its CRC on first use | format13, segment | killed (`segment_test.cc:270`, the corruption sweep) |
| M28 | `total_length` read from the wrong statistics field | format13, segment, search, exact, multisegment | killed, but only by a score-distribution property (`multisegment_test.cc:321`) |
| M1 | the key cursor stops checking that keys increase (`keys.hh:170`) | format13, fixtures, segment, index | survived |
| M3 | the key cursor stops checking its count against `key_index`'s (`keys.hh:164`) | format13, fixtures, segment, index | survived |
| M2 | one key whose range has min ≠ max is accepted (`keys.hh:119`) | format13, segment | survived |
| M33 | the merge path's key-prefix backstop dropped (`writer.hh:269`) | format13, merge, index | survived |
| M4 | a merge writes every plugin of the layout, configured or not (`merge.hh:361`) | format13, index, merge, lifecycle | survived |
| M5 | the length record skips length-0 documents in segments (`reader.hh:1690`) | format13, fixtures, index, lifecycle | survived |

What each survivor owes:

- M1 and M3 guard only against resealed damage (a writer cannot produce either), which no unit
  test builds. They need CR2's `exercise()` to walk the cursor, not a unit test.
- M2: one more refusal beside `key_index`'s others (`segment_test.cc:672-677`): a one-key
  segment whose minimum and maximum differ.
- M33: F13-R10's "refuse, never drop" is tested only on the flush path
  (`format13_test.cc:817-828`). A merge of a segment that holds a key-prefixed term, under a
  nonempty prefix, should throw `std::invalid_argument`.
- M28: a direct test that a written segment's statistics equal its documents' (total, minimum,
  and maximum length, most positions). Today only a score property notices a wrong total.
- M4: F4's "a merge's output takes the index's configuration at the merge, as a flush does"
  has no test. The keyless index's test checks its segments after flushes only
  (`format13_test.cc:707-733`), and `lifecycle`'s keyless schedules compare digests, which key
  on the configured bits and so cannot see a `keys` section a merge should not have written.
  A `force_merge` in that test, then `!holds<keys_plugin>()` on its output, would kill it.
- M5: F21's "a length record for every document ..., length 0 included" has no test. Neither
  the pinned digest's script (`fixtures_test.cc:156-204`) nor `lifecycle`'s documents
  (`lifecycle_test.cc:60`, one to four terms) hold a length-0 document, and every digest test
  compares the code with itself. A keyed index holding a length-0 document, digested before and
  after a flush, would kill it (the buffer path emits the record and the mutated segment path
  does not).

### CR7. `lookup_key` probes each candidate segment's filter twice (T5)

- `lookup_key` (`index.hh:1406-1407`) calls `keys.may_hold(key)` and then `keys.find(key, live)`.
  That calls `keys::reader::find` (`reader.hh:539`), which calls `may_hold` again
  (`keys.hh:143`). Each segment whose filter answers yes costs two range comparisons, two
  `xxh64` hashes of the key, and two filter probes. Format 12's `find` went to the dictionary
  without the second probe, so the cost is new in step 7c.
- Every segment's filter hashes with xxHash64 at seed 0 (I12), so the hash is the same in every
  segment. Yet a miss over 50 segments, F8's deciding case ("the common case of an upsert"),
  hashes the key once per segment whose range admits it.
- Not measured (no benchmarks in this review). T5 holds `lookup_key` at 1 and 50 segments to
  format 12's row.
- Fix: one probe per segment (drop `lookup_key`'s pre-check, or give `find` a form that skips its
  own), and one hash per lookup through a `may_hold(key, hash)` overload. Before T5 runs.

### CR8. Memory per open segment and per merge (T10; PLAN's many-index workload)

- An absent `inverted` reader allocates `doc_count` zero bytes of norms per open
  (`reader.hh:459`). No search index writes such a segment (its configuration always holds
  `inverted`), but a search-layout binary that opens a `document`-profile segment holds 1 MB of
  heap per million documents, all zeros. A shared zero buffer, or a norm of 0 without a table,
  holds none.
- A segment with `inverted` and without `norms` verifies and reads all of `lengths` at open and
  builds its norms there (`reader.hh:285-291`). That is O(`doc_count`) time and bytes per open,
  and the one profile whose open reads `lengths`, which T7 forbids for the profile it tests. No
  configuration writes such a segment today (see the architect's question below).
- A merge writes `docids` before the plugins' merges (`merge.hh:358`), so the pinned `docids`
  bytes are held and charged through `inverted`'s merge, which is the merge's peak. The flush
  writes them after `inverted` (`writer.hh:893-894`); writing them after the merge's plugin loop
  keeps the same bytes and rank.
- A keyless segment's `keys_builder` still collects and charges 8 bytes of version per document
  (`writer.hh:682`) that it never writes.
- Not measured. Fix the first and the third before T10; the others are small.

### CR9. Format 12's words left behind, and comment length

- Stale since step 7c, because a key is no longer a term:
  - `index.hh:46` ("the key term");
  - `tools/strata_engine.cc:618-619`, an orphaned comment about a "key term (wdf 0)" above
    `churn`;
  - `test/segment_test.cc:307` ("Format 12: ... a key term per document");
  - `test/media_test.cc:140` ("A merge reads the key terms");
  - `test/concepts_test.cc:25-26` ("a stub keys plugin ... until format 13's keys plugin exists").
- Added comment blocks over four lines: `index.hh:519-524` (6 lines); `keys.hh:23-30` (8 lines,
  the byte layout, which belongs in F8 with a one-line pointer); `section_io.hh:234-238` (5
  lines).
- `basic_index<Layout>` reads `segment::readable`, the search layout's bits, in `check_readable`
  (`index.hh:2061`), and hard-codes the configured bits in `create` (`index.hh:231`). Both are
  right while `basic_index` instantiates only the search layout (`digest()` needs `inverted`),
  and wrong for the first other layout. Derive both from `basic_segment<Layout>`.

### CR10. The sink does not refuse a duplicate or unplaceable pinned section

- `section_sink::write` holds any pinned kind, and `finish()` sorts and writes the held sections
  (`section_io.hh:260-268, 307-321`), with no check that a kind arrives once or has a rank. A
  plugin that wrote `term_index` twice, or `pk_filter` or `index_sort`, would produce a file the
  reader refuses, with no `logic_error` at the write. Streamed sections already get that check
  (`section_io.hh:328-334`). No writer does this today.
- Fix: the same check for pinned kinds in `write`.

### CR11. Format 13's version stays 13 while its bytes keep changing (F1, F2; for the architect and the lead)

- F1: "Until the freeze, the branch writes format 13 with candidate encodings, fixtures are
  re-pinned with each change, and no index outside tests holds format 13." Step 7c writes
  `version = min_read_version = 13` (`format.hh:68-69`). F10 to F13, F8's codec choice, and the
  `values`, `term_lists`, and `index_sort` plugins will each change bytes under that same number.
  Nothing in the code stops a binary from opening an index an earlier format-13 binary wrote.
- Scenario: `bench/query_trials.sh:26-30` recompiles the strata engine and keeps its index when
  `index-strata.done` exists. Run after F10 lands, the new binary opens 7c's index: `read_version`
  accepts 13, the checksums are good, and the postings decoder reads 7c's skip data as F10's
  advance entries. The best case is `corrupt_segment`; the worst is wrong matches. Fuzzing's
  "fence or answer" covers resealed damage, not a valid file of another layout under one number.
- Once 7c lands on main, main's binaries write this format for gate rows and benchmarks
  (PLAN, "What waits"), so F1's "no index outside tests" needs a mechanism, not a convention.
- Fix, either:
  1. A pre-freeze version: `version = min_read_version` set to a development number (bumped
     at each byte-changing format-13 commit, and set to 13 at the freeze). Every binary then
     refuses another pre-freeze layout as `unsupported_segment`, and the manifest's range as
     `unsupported_index`, through the rules `read_version` and `check_readable` already have;
     T1's prefix CRC constant changes with it.
  2. The lead's rule that no index built before a format-13 change is opened after it, with the
     bench scripts' markers keyed on the format's state.

  I recommend the first. Decide before 7c lands on main.

### CR12. The key left the terms, but not their accounting (F8; D17's budget, T5's admission)

- In format 12 the key was one of the document's terms, so `inverted_plugin::bytes` and
  `disk_estimate` counted it. Since 7c-3 it is `core_part::key`, which no plugin's `bytes` or
  `disk_estimate` sees (`reader.hh:742-770`). So `doc_bytes` (`index.hh:728`), which feeds the
  budget permit (`charge`, `:731-739`) and the `buffer_share` trigger (`needs_flush`, `:741-745`),
  and `estimate_bytes` (`index.hh:1380-1387`), which feeds storage admission, no longer count the
  document's key.
- A buffered keyed document holds its key four times: the version map's two copies, which
  `version_key_bytes_` counts (`index.hh:1499`); `buffered::key`; and `buffered::doc.key`
  (`index.hh:781-786, 1618`). Format 12 counted three of those four; format 13 counts two, and
  `buffered::key` now duplicates `doc.key` outright. An `add` carries its key in `doc.key` with
  an empty `op.key`, so the disk estimate counts that key not at all, while the flush writes it
  into `keys` and `key_index`.
- Probe `.scratch/probe_keybytes.cc`: a document with a 1,000-byte key and one term has
  `part_bytes` 61 and `disk_estimate` 17; the same 1,000 bytes as its stored record give 1,061
  and 1,079. In an index the version map's quarter-share trigger still bounds large keys (1,000
  upserts with 1,000-byte keys at `buffer_share` 256 KiB flush 31 segments, against 4 for short
  keys with 1,000-byte records), so this is an accounting error of two key copies per buffered
  document, not a runaway buffer. Keys past the small-string size (22 bytes in libc++, 15 in
  libstdc++), such as a `Q` and a UUID, pay it in full.
- The same move dropped the key's length check from stage 1. As a term, the key was refused
  above `max_term_bytes` (2^32 − 1, `reader.hh:236`). Now only the flush's `termdict` builder
  refuses it (`termdict.hh:95`), after the transaction was accepted. A key that large is
  impractical, but the limit's mechanism moved from admission to flush (the load-bearing-limits
  rule).
- Fix: count the key in `doc_bytes` and in the disk estimate (a `core_part` term in both), check
  its length in stage 1 with the terms' limit, and drop `buffered::key` in favor of `doc.key`.
  With the next fold.

## The criteria the ledger ticks

Each tick was checked against its evidence:

1. **K6** (`media_test` sweeps bytes 0 to 15): `media_test.cc:101-108` adds every prefix offset
   of every segment before the stride, and the suite passes it here (114 s under ASan). Holds.
2. **K8** (the digest changes in exactly one commit): `git log d508158..f2c19c6 --
   test/fixtures/digest.txt` names only `bd69714` (7c-5), and on the fold only its copy
   `2bd28e7`; mutant M6 shows the pinned digest catches a change to the `D` record. Holds.
3. **K9** (T7): `format13_test.cc:273-306` protects every whole page between the header page and
   the tail, asserts that at least one page is protected (`:297`), and opens with no fault and
   no verified region. It passes here at 4 KiB pages. Holds.
4. **K11** (F20's checks): `format13_test.cc:433-518` and `files_test.cc:191-238` cover a mask bit
   at open and at `install`, a range above this binary's, a segment outside its mask, and a
   version-1 manifest; mutant M31 is killed. The range-outside case is deviation 5 below. Holds.

## Deviations ruled

The ledger lists these for the review (7c-1, 7c-3, 7c-6). Each was checked against the code:

1. `index_sort` is not a readable bit until F18's reader (`core_segment::reads` is 0,
   `reader.hh:161`). Accepted: a segment that sets it is unsupported, which is F4's rule for a
   bit this binary lacks, and T1 tests it (`format13_test.cc:391`).
2. `merge_options::key_prefix` stays, as the backstop's prefix only. Accepted. The merge finds no
   key by it (keys come from the keys reader's scan from the empty key, `merge.hh:264-273`); it
   reaches only the inverted builder's refusal (`merge.hh:83`); and it is the one path from the
   index into a merge. The design's 7c-3 line still says "deleted" (architect, item 2).
3. F13-R25 item 3, the buffer path's key leaf, waits for D16. Accepted: the base has no delete by
   query and no predicate over buffered documents (the operation kinds, `index.hh:43`).
4. F13-R33's wildcard tests wait for automata's expansion. Accepted: term queries over a keyless
   index are tested (`format13_test.cc:831-849`).
5. A segment outside the manifest's version range cannot be built while the binary reads one
   version. Accepted: `load_segment`'s range check (`index.hh:2113`) is unreachable today,
   because `read_version` refuses any other version first and `load_segment` turns that into
   `corrupt_segment`, which is the same outcome. The check becomes live with format 14's reader.
6. The configured bits change only at creation (and by `install`) until F23 rule 4's operation
   exists. Accepted, with CR5.
7. Not listed by the ledger: F3 gives `values`, `term_lists`, and `index_sort` ranks 7, 8, and
   11, and `format.hh:172-175` leaves them `unranked`. That is harmless while their bits are
   unreadable, and CR1's fix makes an unreadable kind's rank irrelevant. The commit that adds
   each plugin sets its rank.

## For the architect

1. **A segment with `inverted` and without `norms`** (F4). The reader builds each document's norm
   from its exact length at open (`reader.hh:285-291`), so its scores equal a segment that built
   norms (`format13_test.cc:576-591`). F4 gives such a segment no meaning, and no configuration
   writes one. Either F4 states this rule and T7 and T10 accept its open cost (CR8), or the reader
   refuses such a segment until a profile needs it. I recommend stating it, since the code is
   small and tested.
2. **7c-3's text** says `merge_options::key_prefix` is deleted (step 7c-3; F13-R11). The code keeps
   it as the only path for the backstop's prefix, and `merge_context::key_prefix`
   (`merge.hh:68`) carries it to the builder, which is what F13-R25 item 1 asks. The design's line
   should say so.
3. **The open order is part of the frozen reader.** CR1's fix (the prefix, the footer, the bits,
   then the table, then presence) is a rule every later format-13 release relies on, so F2 and F4
   should state it rather than leave it to `core_segment`'s constructor.

## Verdict

**APPROVE WITH CONDITIONS.** Step 7c implements F2 to F9, F20, and F21 as the design states
them, with the deviations above and one exception (CR1's open order). Nothing found is a memory
error or a wrong answer from a file this binary writes. The fixes are small and do not change one byte of the fixtures, except
CR11's option 1, which changes the version.

Blocking items: none in the code. One decision precedes landing on main, the lead's and the
architect's: CR11.

Conditions, each with when it is due:

1. CR11, before 7c lands on main: a pre-freeze version (my recommendation), or the lead's rule
   with the bench markers keyed on the format's state.
2. CR1, before the freeze (K1): classify the bits before the table, with T1's rows for a
   reserved structure at its F3 place, `compact`, and an unlisted bit with a longer table.
3. CR2, with K2 and before the freeze: `fuzz_segment` drives `find`, `scan`, the cursor, and
   `check_segment`, with a keys selector and a corpus grown on format 13's bases.
4. CR3, CR4, CR5, and CR12, in the next fold or before the freeze: `check()` ties keys to
   `key_index`'s range and filter and refuses a repeated ordinal; `keys::reader` turns the FST's
   `invalid_argument` into `corrupt_segment`; the manifest's configured bits are validated at
   decode, open, and `install`; the key is counted in `doc_bytes` and the disk estimate, and
   `buffered::key` goes.
5. CR6, in the next fold: tests that kill M2, M4, M5, and M33, and a direct statistics test
   for M28; M1 and M3 through CR2's fuzz target.
6. CR7 before T5's row, and CR8's first and third items before T10.
7. CR9 and CR10 at the developer's convenience.
8. CI: the fold's head `23716c5` is green on all eight jobs (run 39130677071). None of the
   fold's rewritten step commits (`b8d9ac9` and `e6b2144` kept; `60b0819` to `596e675` new) has
   its own run, so L14's every-commit green for the fold is the lead's call. Landing also waits for the architect's scoped re-check
   (the ledger's Next 1; PLAN, "What waits").

Not run: the eight suite tests the pause stopped (see "Interim (paused)"); any benchmark or timing (T3 to T6, T10, T11; CR7 and CR8 are by inspection); TSan
locally (CI's `linux-tsan` is green at `82060cc`); the libFuzzer targets with `STRATA_FUZZ=ON`
(only the corpus replays inside `ctest`, plus the keys mutation probe); AGENTS.md's Release build
and the Linux, FreeBSD, and arm64 jobs locally (CI's eight jobs at `82060cc` cover them);
`strata_engine verify` and `parity` (K10's tool runs); and builds of the fold's rewritten step
commits.
