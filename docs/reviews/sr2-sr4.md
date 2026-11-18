# Security review: SR2 and SR4 fixes (follow-ups of f14-step1-decoder.md)

Reviewer: security. Threat model: the trusted cluster. Inputs are our own files, possibly corrupt or truncated (damage
that survives the checksums is modelled by resealing); no attacker on a node. Calibration read first
(`docs/reviews/calibration.md`); its checks applied here: HEAD compared with origin for both branches (both are pushed
and equal: `21ca549` and `0f68045`), CI on other platforms is not mine to run (see Not run), and every mechanism a fix
relies on was found in the code at the cited commit.

Builds: Homebrew LLVM clang 23.1.2, `-fsanitize=address,undefined -fno-sanitize-recover=all`, RelWithDebInfo, 4 jobs,
nice 19, under `QUIET_LEVEL=light quiet run`. Full ctest (23 tests) on each branch: both 100% passed
(`.scratch/review/build-asan.log` in each worktree).

## Verdicts

| Branch | Commit | Verdict |
|---|---|---|
| `explore/sr2-stored-size` | `21ca549` | APPROVE TO LAND |
| `explore/sr4-merge-damage` | `0f68045` | APPROVE WITH CONDITIONS (C1 before landing) |

## Branch 1: SR2, stored section size (`21ca549`)

The change (`include/strata/core/reader.hh:410`): the stored reader's constructor throws `corrupt_segment("stored
section")` when the structure is present, the segment has documents, and the section is under `4 + 16 + 1 = 21` bytes.

### SR2-1 (info, answers the question): 21 bytes never refuses a legal segment

The writer emits, for any segment with documents, at least one frame: `encode_stored` gets one `stored_ends_` entry per
document, empty or not (`writer.hh:249-286`), so a frame is a codec byte plus a payload of the count and one length per
document (at least 2 bytes), then 16 bytes of index and the 4-byte frame count. The true minimum is therefore
1 + 2 + 16 + 4 = **23** bytes, reached by one document with an empty value; 21 has two bytes of slack. A codec cannot
go under it: a block is stored compressed only if it saves more than an eighth (`writer.hh:293`), and a 2-byte payload
cannot.

Probe `probe_sr2_min` (`.scratch/review/probe_sr2_min.cc`, output `probe_sr2_min.out`): 370 legal builds per layout over
codecs raw, LZ4, zstd 1/3/19; block sizes 1, 4096, 65536; 1, 2, 3, 17, 100, 3000 and 70000 documents; values all empty,
1 byte, compressible and random; for `document_layout` (4 KiB LZ4 policy) and `layout<stored_plugin>` (the default
policy: 64 KiB zstd, the search layout's). Result: **0 refused, smallest section 23 bytes** (one document, empty value,
every codec), each build also passing `verify_all`, every record read, and `check_segment`. The real search layout lives
in another repository and was not built (Not run); its stored policy is the default one covered above.

### SR2-2 (info): what a short or lying section does after the fix

Probe `probe_sr2_min` (cut section) on a valid 49-byte section of a 3-document segment, resealed at every length 0..49:

| Cut length | Before (`0674d44`, `probe_sr2_prefix`) | After (`21ca549`) |
|---|---|---|
| 0, 1, 2, 3 | silent OK: every record reads as empty | refused at open: "stored section" |
| 4 to 20 | refused at first use / `check`: "stored index" | refused at open: "stored section" |
| 21 to 48 | refused at first use / `check`: "stored index" | unchanged: refused at first use / `check` |
| 49 (whole) | OK | OK |

So the fix closes exactly the silent class (0..3); 4..20 now fail earlier. A section of 21 or 22 bytes, or any section
long enough that claims more frames than it holds, is not refused at open (`stored_frames()` checks `frames * 16`
against the size lazily, `reader.hh:431-434`), but it cannot be read silently: 20,000 random sections of 21..80 bytes
with a lying frame count (often small, often huge) on both layouts gave **0 silent accepts**; every one was refused at
first use or in `check_segment`: stored index 17,009; stored frame bounds 2,113; stored frame size 749; stored frame 88;
stored codec 39; truncated integer 2; **0 other exceptions**. That matches the design in the original finding (`check()`
is the line that says so) and the fix's stated scope.

The test fails with the fix reverted: `probe_sr2_prefix` (the same probe built against `0674d44`'s `reader.hh`) accepts
cuts 0, 1 and 3 silently, which are the test's three cases.

### SR2-3 (low, non-blocking): the bound is looser than the format

Tightening to 23 (a frame is at least 3 bytes) would refuse two more damaged lengths at open. They are caught at first
use and by `check()` anyway, so this is only tidiness. Not a condition.

### SR2-4 (low, non-blocking): no test pins the bound from the legal side

`format14_test` has the three refusals but nothing that opens a one-document, empty-valued segment (the 23-byte
section), so a future tightening that refuses it would pass the suite. `probe_sr2_min` is the check; moving a
single-document empty-value case into `format14_test` closes it.

### Verdict, branch 1

APPROVE TO LAND. No blockers. Full suite under ASan+UBSan passed, including `format14` (268 s) and `fuzz_stored_replay`.

## Branch 2: SR4, quarantine an inconsistent merge source (`0f68045`)

The change: `publish_merges` sends `std::logic_error` (so also `invalid_argument`, `out_of_range`, `length_error`) to
`mark_damaged` (`index.hh:1465`); `mark_damaged` runs `check_segment` on each source and marks the ones that throw
`corrupt_segment` (`index.hh:1486-1500`); `heal()` deep-checks a segment that a merge marked, both in memory and after
re-reading its file (`index.hh:689, 701`).

Which exceptions the SR4 cases raise (probe `probe_merge_damage_orig`, from the original review, rerun on this branch):
two keys naming one ordinal gives `invalid_argument` "merge order must name every live document once"
(`merge.hh:369`); a dropped key with a surviving version gives `invalid_argument` "a version without a key"
(`writer.hh:167`). The third original site, `merge.hh:150` (`logic_error`, "the key order and the inputs' keys
disagree"), is the same class.

### SR4-1 (medium): catching every `logic_error` conflates a bug in the merge with a damaged source

`logic_error` is the base of what the merge code throws for its own broken contracts as well: `section_io.hh:332, 357,
394` (a sink used out of order), `writer.hh:158, 233, 235, 361, 410, 422` (builder misuse and configuration), the
caller-supplied order contract at `merge.hh:315, 369, 374, 391`, `merge.hh:366` (two live copies of a docid, an
index-level invariant), and the limits `length_error` (`merge.hh:381`, `writer.hh:357`). After this change each of them
runs `check_segment` over the sources, and **if any source fails** the bug is reported as that source being damaged and
the merge is skipped.

Probes (`probe_sr4_index.cc`; the `inject_*` cases are built against `mut-include`, a copy of the headers with a
`logic_error` thrown at the top of `merge_layout` when `SR4_INJECT` is set, standing in for a bug in our merge code):

| Case | Sources | Result |
|---|---|---|
| `inject_clean` (6 segments) | all pass `check_segment` | `index_fenced: strata: merge failure: injected programming bug in merge`; `needs_heal=0`; bug visible, not masked |
| `inject_dirty` (6 segments) | segment 0 key-inconsistent | segment 0 marked (`needs_heal=1`), but the jobs that do not contain it fail the same way and the index fences with the bug's message |
| `inject_dirty_two` (2 segments) | segment 0 key-inconsistent, one clean | **masked**: `force_merge` returns normally, `needs_heal=1`, writes continue; the bug shows only after `heal()` clears the in-memory damage and the next merge fences with "injected programming bug" |

(inline and two-thread executors agree.) So the hiding is real but bounded: it needs a real damaged source in the same
merge job, and it lasts until the next merge of the healed source. It does not turn a bug into permanent damage, and
it never loops. It is still the wrong signal: whether a segment is damaged should be decided by what threw while
decoding it, not by whether the exception derives from `logic_error`, and the next person adding a `logic_error` to the
merge path inherits the quarantine behaviour silently.

**Narrower signal (what to do).** Mark damage only on `corrupt_segment`, and make the sites that detect an
inconsistent source throw it, as the original finding proposed:

1. `key_merge_order` (`merge.hh`, the loop that fills `out.order`, around lines 250-270): it already records
   `keyed[source][ordinal]`; throw `corrupt_segment("keys")` when an ordinal is named twice, and when the keyed plus
   keyless count differs from the live count. Then `merge.hh:369` stays an `invalid_argument`, which is correct for a
   caller-supplied order (the reordering path) and fences.
2. The "version without a key" check for a source document (the merge reading `version(ord)` of a keyless ordinal,
   `merge.hh` near `:150` and the `keys_builder` call at `writer.hh:167`): validate in the merge before calling the
   builder and throw `corrupt_segment`.
3. `merge.hh:150`: throw `corrupt_segment`.
4. Delete the `logic_error` handler (`index.hh:1465-1467`); `mark_damaged` and `check_segment` stay as they are.

The existing `inconsistent_source` test then still passes, and a new negative test (SR4-5) pins the other side.

### SR4-2 (low): `check_segment` on every source runs on the writer thread for any `logic_error`

`mark_damaged` reads, decodes and checks every record of every source of the failed job on the writer's thread inside
`publish_merges`, including for errors the check cannot explain: the document-limit `length_error` (a legitimate limit,
not damage), two live copies of a docid, a caller-order bug. The result is a full scan of the sources and then a fence.
It is in an error path only and is not measured here (no measurements requested). It goes away with SR4-1's narrowing,
since only `corrupt_segment` would reach it.

### SR4-3 (info, answers the question): `mark_damaged` passing on the failing source does not retry forever

Probe `dup_docid`: segment 1 replaced by a well-formed segment (own keys) whose docids equal segment 0's, so each
source passes `check_segment` and the merge throws `logic_error` "two live copies of a docid". Result on both
executors: `mark_damaged` marks nothing and the handler falls to `fence`: `index_fenced: strata: merge failure: ...
two live copies of a docid`, `needs_heal=0`, every later call fenced. Not a retry loop: `publish_merges` removes the job
from `pending_` before the handler, and the only path that does not fence marks a source `damaged`, which
`run_merges` skips (`index.hh:967`, `1159`) until `heal()`.

Probe `dup_plus_keys` (the same duplicate, plus segment 0 key-inconsistent): segment 0 is marked, the rest merge,
`heal()` replaces segment 0 from its (clean) file and clears the flag, and the next `force_merge` fences on the
duplicate. Terminates in one extra round; the index-level duplicate is not hidden for good.

The cross-segment docid duplicate fencing the index is the pre-existing behaviour (no per-segment check can see it) and
is the correct outcome for an index-level invariant; noted only so a reader knows that class is not quarantined.

### SR4-4 (low, non-blocking): only `corrupt_segment` is caught around `check_segment`

`mark_damaged` (`index.hh:1491-1497`) and the first `try` in `heal()` (`index.hh:688-698`) catch `corrupt_segment` only.
If `check_segment` threw anything else (`invalid_argument`, `out_of_range`, `length_error`, `bad_alloc`), the exception
would leave `publish_merges` from inside a handler with the job already removed, the index not fenced, and the failing
merge rescheduled to fail again. I found no such throw: probe `probe_sections` (`.scratch/review/probe_sections.cc`,
from the f14 review) ran 60,000 resealed random-damage cases over keys, key_index, versions, docids and stats:
ok 2,688, corrupt 57,312, **other 0**, no rank violation; the 20,000 stored-section cases of `probe_sr2_min` gave other 0;
`fuzz_stored_replay` passed. Within the trusted-cluster model nothing more is needed; if SR4-1 is done as written the
question disappears for `logic_error` sources.

### SR4-5 (low, condition with C1): test gaps

`inconsistent_source` (`test/background_merge_test.cc`, both variants, inline and threaded) fails without the fix:
`test_prefix`, the test built against `0674d44`'s `index.hh`, dies with `index_fenced: strata: merge failure: strata:
merge order must name every live document once`. But:

1. No test pins that a non-damage `logic_error` still fences (SR4-1's other side). Add the `dup_docid` shape
   (probe `probe_sr4_index`, `same_docids_as`): two well-formed segments with one docid must fence, not quarantine.
2. No test covers `heal()` finding the on-disk file inconsistent too (`index.hh:701-703`): the test notes the damage is in
   memory only, so the `corrupt_segment` out of `check_segment(fresh->seg, ...)` path (reported in the returned ids) is
   unexercised.
3. `(void)victim;` and the `victim` local in `inconsistent_source` are dead code.

### Verdict, branch 2

APPROVE WITH CONDITIONS. No blockers (nothing here loops, leaks state or loses data; the worst outcome is a bug
reporting as a damaged segment until the next `heal()`).

- **C1 (before landing)**: replace the `logic_error` handler with `corrupt_segment` thrown at the sources' inconsistency
  sites (SR4-1), and add the negative test (SR4-5 item 1).
- C2 (non-blocking): SR4-5 items 2 and 3.

## Probes (all kept in the worktrees' `.scratch/review/`)

SR2 worktree (`strata-wt/explore-sr2/.scratch/review/`):

| File | What it does |
|---|---|
| `san.sh` | Homebrew LLVM ASan+UBSan configure, build (-j4) and ctest |
| `build_probe.sh` | Builds a probe with a test target's flags; optional first include dir for a mutant |
| `probe_sr2_min.cc`, `probe_sr2_min.out` | SR2-1/2: smallest legal section, refusals of legal segments, every cut length, 20,000 lying sections |
| `probe_sr2_prefix`, `mut-prefix/` | The same cut scan against `0674d44`'s `reader.hh` (the fix reverted) |

SR4 worktree (`strata-wt/explore-sr4/.scratch/review/`):

| File | What it does |
|---|---|
| `san.sh`, `build_probe.sh` | As above |
| `probe_sr4_index.cc` | `dup_docid`, `dup_plus_keys`, `inject_clean`, `inject_dirty`, `inject_dirty_two` through the test's `index_test_access`, inline and two-thread |
| `mut-include/` | Header copy with a `logic_error` injected at the top of `merge_layout` (`SR4_INJECT`); `probe_sr4_inject` is built against it |
| `mut-prefix/`, `test_prefix` | `background_merge_test` built against `0674d44`'s `index.hh` (the fix reverted): dies with `index_fenced` |
| `probe_sections.cc` | The f14 review's random-damage probe, rerun (60,000 cases, other 0) |
| `probe_merge_damage_orig.cc` | The f14 review's `probe_merge_damage`, rerun: names the throwing sites |

## Not run

- The search layout itself (another repository): only its stored policy (64 KiB zstd default) through `layout<stored_plugin>`.
- Linux and FreeBSD builds and GitHub CI for either commit; TSan; fuzzing beyond the checked-in replays.
- A mutant of SR4 with the narrowed signal (SR4-1 is a recommendation, not built).
- SIGBUS or media faults during `mark_damaged`'s scan of a mapped source; measurements (none requested), including the cost of SR4-2.
