# strata review: format 13, CR6's mutants re-run

- Date: 2026-10-18. Scope: the code review's CR6 (`format-13-code.md:207-254`) and its
  condition 5 (`:452-453`): whether the branch's tests now kill the review's sixteen mutants,
  with CR6's tests (`b88ee36`) and CR2's fuzz paths (`953d907`). The rest of the CR1 to CR12
  re-check is not this file's.
- Reviewed: `phase4/format13` at `bb1e90a` (`origin` at the same commit, nothing unpushed). Its
  code is `d26a26a`'s: `a681347` and `bb1e90a` change only `docs/status/format13.md`, and CI run
  39163730187 at `d26a26a` is green on all eight jobs. Built and run in the detached worktree
  `strata-wt/review-f13-mut`.
- Read first: `format-13-code.md` (CR6's table, "What each survivor owes", condition 5), the
  ledger's CR6 and CR2 lines (`docs/status/format13.md:55-56`), and `docs/reviews/calibration.md`,
  whose checks were applied: CI on every platform for the reviewed commit, `HEAD` against
  `origin`, and a claim checked by running it.
- Clean room: no Xapian or Xapiand source read. `termdict` (Kronuz, MIT) read for the key
  blocks' layout and its cursor, which the M1 probe walks.

## Summary

CR6's tests work. M2, M4, M5, M28, and M33 are now killed, each at the line of the test CR6
added for it, and the ten mutants the review's tests killed are still killed at the tip.

M1 and M3 survive every test, CR2's corpus replay included. That replay cannot kill them: it
passes a refusal and an acceptance alike, and no corpus input changes its verdict under either
mutant (MR1). Two unit rows kill them; a probe shows each refused on the unmutated tree and
accepted under its mutant only. The probe also shows that a merge, which never walks the
checked cursor, fails on a repeated key with `std::invalid_argument` rather than as damage (MR2).

## The harness, rebuilt

The review's `mutants.py` and `mutrun.sh` went with their worktree, so both were rebuilt from
CR6's table, in `.scratch/` of this worktree:

- `mutants.py`: each mutant one edit, an exact text that must occur once at `bb1e90a` or the
  run stops, so a mutant never lands in the wrong place. The results table gives every edit's
  place, so the next run need not reconstruct them.
- `mutrun.sh`: applies a mutant, builds its tests' binaries, runs them through `ctest -j4`,
  restores the file from git, and records the result with the failing `CHECK` lines. For a
  mutant whose tests include the fuzz replay, it also saves the replay's verdict for every corpus
  input (`STRATA_FUZZ_VERDICTS=1`). With `ALL=1` it runs the whole suite instead of the review's
  list.
- `probe.sh` and `probes/probe_cursor.cc`: compiled against copies of `bb1e90a`'s `include/`,
  one untouched and one per cursor mutant, so a probe never reads a file the run is mutating.
- The build: Homebrew LLVM 23.1.2, the Debug type at `-O1` without `NDEBUG`, so the
  dependencies' assertions stay on, and no sanitizers: the review's "at `-O1` without
  sanitizers". The unmutated tree passes all 36 tests there (330 s at `ctest -j4`), and at `-O3`
  (272 s).
- Thirteen descriptions name one check exactly; three needed a reading, recorded here:
  1. M8 swaps the requires loop and the unreadable-bit test in `classify_structures`, where
     F13-R35 moved them.
  2. M28 reads `total_length` from the maximum length's field (`u32` at offset 12). The
     review's M28 failed `multisegment_test` and mine passes it, so the review read another
     field; CR6's test catches any reading whose total differs from 15 there.
  3. M32 deletes F13-R25 item 7's fallback to base 0 and width 64.
- Four variants on the checks CR6's tests target, named after their mutant: M5b (the buffer's
  length record skips length 0), M28b (`max_positions` read from the maximum length's field),
  M1b (the cursor accepts an empty key), and M3b (the cursor accepts more keys than counted).
- The host: macOS 15.6 on an Intel i9-9980HK (x86_64). Everything ran at `nice -n 19`, at most
  four jobs under the light holds `nostalls-core-r1` to `-r3`, and stopped through two strict
  holds (23:07 to 23:12, and `scale-threads-mac2` from 23:31 to 23:37) by
  `strata-wt/review-counters/.scratch/watch.sh`.

## Results

Lines are at `bb1e90a`. "Review" is the review's outcome at `f2c19c6`.

| Mutant | Edit | Tests run | Review | Now | Killed at |
|---|---|---|---|---|---|
| M8 | `format.hh:383-388`: the requires loop after the unreadable-bit test | format13 | killed | killed | `format13_test.cc:442, 445, 446` |
| M29 | `format.hh:326` deleted (the footer's reserved word) | format13 | killed | killed | `format13_test.cc:423` |
| M30 | `check.hh:62`: a version without a key accepted | format13 | killed | killed | `format13_test.cc:816` |
| M31 | `index.hh:2064-2066` deleted (the manifest's version range) | format13 | killed | killed | `format13_test.cc:568, 570` |
| M32 | `writer.hh:704-707` deleted (the versions' base rule) | format13 | killed | killed | `format13_test.cc:793` |
| M35 | `format.hh:242` deleted (F2's rule 3) | format13 | killed | killed | `format13_test.cc:294, 304` |
| M34 | `searcher.hh:108`: every term to the dictionary | format13 | killed | killed | `format13_test.cc:900` |
| M6 | `index.hh:536` deleted (the `D` record's key) | fixtures | killed | killed | `fixtures_test.cc:37` (`digest.txt`) |
| M10 | `reader.hh:454`: `lengths` without its CRC | format13, segment | killed | killed | `segment_test.cc:270`; `format13_test.cc:369` |
| M28 | `reader.hh:277`: `total_length` from offset 12 | format13, segment, search, exact, multisegment | killed by `multisegment_test.cc:321` only | killed | `format13_test.cc:1077` (CR6) |
| M2 | `keys.hh:119`: one key with min ≠ max accepted | format13, segment | survived | killed | `segment_test.cc:716` (CR6) |
| M33 | `writer.hh:269` deleted (the merge path's backstop) | format13, merge, index | survived | killed | `format13_test.cc:1012` (CR6) |
| M4 | `merge.hh:360` deleted (a merge writes every plugin) | format13, index, merge, lifecycle | survived | killed | `format13_test.cc:849` (CR6) |
| M5 | `reader.hh:1718`: skip a length-0 document's record | format13, fixtures, index, lifecycle | survived | killed | `format13_test.cc:1022` (CR6) |
| M1 | `keys.hh:175`: the cursor's order test dropped | format13, fixtures, segment, index, fuzz replay | survived | **survived** | none (MR1) |
| M3 | `keys.hh:169` deleted (the cursor's count test) | the same | survived | **survived** | none (MR1) |
| M5b | `reader.hh:1747`: the buffer's record only at length ≠ 0 | as M5 | | killed | `format13_test.cc:1022` |
| M28b | `reader.hh:278`: `max_positions` from offset 12 | as M28 | | killed | `format13_test.cc:1077` |
| M1b | `keys.hh:175`: the cursor's emptiness test dropped | as M1 | | survived, equivalent | (MR3) |
| M3b | `keys.hh:175`: the cursor's over-count test dropped | as M1 | | survived, equivalent | (MR3) |

Every kill was read at the line it fails, and each is the check that the mutant breaks. M1 and
M3 also ran against the whole suite, and pass all 36 tests (`results/M1.all.txt`,
`M3.all.txt`).

## Findings

### MR1. M1 and M3 survive, and CR2's fuzz target cannot kill them

- Both pass all 36 tests of the suite, `fuzz_segment_replay` over the checked-in corpus
  included (above).
- The target cannot kill them by construction. `LLVMFuzzerTestOneInput` catches
  `corrupt_segment`, `unsupported_segment`, and `termdict::corrupt` and returns either way
  (`fuzz/fuzz_segment.cc:217-226`), and the replay fails only on a crash or an escaping exception
  (`fuzz/replay.cc`). M1 and M3 turn a refusal into an acceptance, which passes.
- The corpus does not reach them either. Over all 1,185 verdicts (of 1,186 inputs, one shorter
  than two bytes), the unmutated build gives 877 `corrupt_segment`, 287 accepted, and 21
  `unsupported_segment`, and M1's and M3's builds give the same verdict input by input
  (`results/base.verdicts.txt`, `M1.verdicts.txt`, `M3.verdicts.txt`). Even a pinned verdict file
  would not kill them.
- So the ledger's "M1 and M3 through CR2's fuzz exercise" (`docs/status/format13.md:55`) and
  CR2's commit message ("and the cursor's order and count checks, M1 and M3") do not hold. The
  review's condition 5 asked for exactly that (MR4).
- The tests that kill them, from `probes/probe_cursor.cc`, each run against the unmutated
  `include/`, M1's, and M3's:
  1. **M1.** F13-R37's ten documents keyed `Q0` to `Q9` (`format13_test.cc:938-944`), one byte
     of `keys` changed so that a key repeats the one before, resealed through `with_section`.
     Byte 6 xor 0x01 gives `Q0 Q0 Q2 … Q9`; xor 0x02 and 0x03 give `Q0 Q3 Q2 …` and
     `Q0 Q2 Q2 …`. Every key lies in the range and the filter, the ordinals are distinct and in
     range, the count is the index's, and the range is the first and last key, so only the order
     test refuses it: `check()` says `key order` unmutated and accepts it under M1. The probe
     finds the byte by trying every byte and xor and walking the result through `termdict`
     directly, so a test written the same way does not depend on `termdict`'s block layout.
  2. **M3.** Ten documents, nine keyed, `key_index`'s count raised from 9 to 10 (still at most
     the document count) through the `reindex` helper (`format13_test.cc:963`) with a count
     argument, resealed: `check()` says `key count` unmutated and accepts it under M3.
- Fix: both rows in `format13_test`'s F13-R37 block (`:935-991`). That closes CR6.

### MR2. The cursor's checks guard `check()` alone, and a merge fails on a repeated key as `std::invalid_argument`

- `keys.hh:159-160` calls the cursor "the merge's and the digest's walk". Both walk the keys with
  `scan("")` instead (`merge.hh:267`, `index.hh:545`), which tests neither order nor count
  (`keys.hh:193-200`). Only `check_segment` (`check.hh:52`) and the fuzz exercise
  (`fuzz_segment.cc:128`) use the cursor.
- Probe, on the unmutated tree: M1's segment (key `Q0` at ordinals 0 and 1), merged alone under
  prefix `Q`, throws `std::invalid_argument: strata: a key repeated within a segment` from the
  output's `keys::encode` (`keys.hh:64`): damage leaves the merge as the type of an operator's
  error, the pattern CR4 fixed at open. M3's segment merges into a valid nine-key segment, its
  count's damage dropped.
- The cost is the diagnosis, not the outcome: a background merge's `std::exception` fences the
  index as "merge failure", and a `corrupt_segment` whose sources pass their checksums, as this
  resealed one does, fences it as "a merge's output is corrupt" (`index.hh:1169-1176`).
- `merge.hh:269` still overwrites `keys[s][o]` when two keys name one ordinal. That is CR3's
  second half ("the merge's key collection refuses a repeated ordinal as `corrupt_segment`"),
  which F13-R37 did not take, though the ledger counts CR3 as fixed by it
  (`docs/status/format13.md:51`). Noted for the CR re-check.
- Fix: the merge and the digest walk the keys with the cursor, and the merge refuses a repeated
  ordinal, each as `corrupt_segment`. Then M1's and M3's checks guard every path that reads keys,
  and a merge of M1's segment expecting `corrupt_segment` is a second test for M1. Before the
  freeze.

### MR3. M1b and M3b survive, and are equivalent where the cursor is used

- M1b: an empty key is below every minimum, and the minimum cannot be empty (`keys.hh:119`), so
  `check()`'s range test refuses it (`check.hh:58`).
- M3b: more keys than counted still fail the count test at the end of the walk (`keys.hh:169`),
  and every user of the cursor walks to the end.
- No test is owed. With MR2's fix, both stay equivalent.

### MR4. For calibration (the lead's file)

The review's condition 5 named CR2's fuzz target as M1's and M3's killer without running them
against it. A target that accepts both a refusal and an answer cannot kill a mutant that turns
one into the other. The check that would have caught it: run every mutant against the test named
as its killer before naming it.

## Verdict

**APPROVE WITH CONDITIONS, for CR6: its tests kill four of the six survivors (M2, M4, M5, M33)
and M28 directly, and all ten mutants the review killed stay killed; M1 and M3 survive the whole
suite, and two rows kill them.**

Blocking items: none. M1 and M3 guard only against resealed damage, and only `check()`.

Conditions:

1. MR1's two rows in `format13_test`, with the next fold. CR6 is then closed.
2. MR2 before the freeze: the merge and the digest walk the cursor, and the merge refuses a
   repeated ordinal, with the merge test. It also carries CR3's open half.
3. The `keys.hh:159-160` comment with MR2, or now if MR2 waits.

Not run: the mutants under ASan, UBSan, or TSan (the harness runs without sanitizers, as the
review's did; CI's sanitizer jobs are green at `d26a26a`); libFuzzer itself (`STRATA_FUZZ`), so
only the checked-in corpus; Linux, FreeBSD, and arm64 (CI at `d26a26a`); mutants beyond the
review's sixteen and the four variants; the whole suite for the mutants its listed tests killed;
and the rest of the CR1 to CR12 re-check.
