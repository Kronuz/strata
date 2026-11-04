# Security review: the manifest log's codec and replay (`include/strata/core/manifest_log.hh`)

> Reviewer: security reviewer, 2026-11-04. Subject: strata `phase/manifest-log-steps12`. The code
> is at `strata:a662fad`, and the head `strata:0f6de43` is equal to `origin`. The branch pins
> journal at `journal:5336512`. Design: `design/manifest-log`, `docs/design/manifest-log.md`
> revision 1 (D2, D3, D6, D7), and its review `docs/reviews/manifest-log-design.md`. Ledger:
> `docs/status/manifest-log-steps12.md`. The framing's own review is journal's
> `docs/reviews/framing-security.md`.

**Verdict: APPROVE WITH CONDITIONS.** The branch has one blocker, SR1. `apply()` accepts a
CRC-valid edit that leaves the state breaking the snapshot decoder's own invariant, and the
branch's fuzz target aborts on a 75-byte input built that way. The fix is one check. SR2 is a
condition on step 3's replay: `apply()` costs time in proportion to the state, not the edit, so
replay is quadratic in a crafted log and slow on an honest one with a large deleted set.

## Threat model

The design's threat model is a trusted cluster whose on-disk bytes can be damaged or crafted.
The attacker for every finding here is **a malicious input file**: a manifest log handed over by a
hostile copy or a restore, or written by someone with local file access. The framing's CRCs catch
damage, so a hostile record is CRC-valid by construction. The decoder and `apply()` then have to
hold every invariant themselves.

## What was attacked, and held

- **Counts and allocation.** Every count goes through `reader::count(min_bytes)` with the right
  minimum: 25 for a segment entry, 5 for a declaration or a delete, 2 for a metadata pair or a
  segment attribute, 1 otherwise. `get_ordinals` reserves at most the bytes left. The bitset path
  reserves at most the document count, which the bitset's size pins. Decoder memory is linear in
  the input. Its factor is SR3.
- **Integer wrap.** Ordinal deltas are checked against `~0 - prev`. The totals use the
  overflow-checked `add`. The removal and delete subtractions in `apply()` run against running
  sums that the snapshot's totals check and each edit's `totals` check keep exact, so they cannot
  underflow. `e.generation != s.generation + 1` cannot match at 2^64-1, because generation 0 is
  refused. Versions are range-checked to 32 bits.
- **Canonical form.** Integers must be in shortest form, a deleted set has one encoding, and
  unknown mask bits and empty sections are refused. The fuzz target's re-encode oracle holds on
  everything I ran.
- **Checks before change.** Every `corrupt_index` in `apply()` is thrown before the first write
  (`manifest_log.hh:668`). The mutation phase calls nothing that throws `corrupt_index`. SR4
  covers `bad_alloc`.
- **Fuzz.** `fuzz_manifest_log` ran for 3 minutes under ASan+UBSan (Homebrew LLVM) from the 33
  seeds, with a dictionary (kinds, mask bits, section bits, a 2^63 integer, version 13). It
  executed 112,830 inputs, peak RSS 425 MB, with no finding. It did not reach SR1's 75-byte input
  either (SR5).

## Findings

### SR1 (high, blocking): an edit may lower `next_segment` below a live segment id

`decode_snapshot` refuses a segment whose id is at or above `next_segment`
(`manifest_log.hh:468`). `apply()` checks only that each *added* id is below the edit's
`next_segment` (`:620`). It never checks the ids it keeps, and never checks that `next_segment`
does not go back. An edit that carries only the scalar `next_segment = 1` over a state holding
segment 5 is accepted.

- Probe: `.scratch/secprobe/p1_next_segment.cc` (ASan+UBSan). In order:
  1. `apply(next_segment=1): ACCEPTED`, with segment 5 live.
  2. `re-snapshot of the replayed state REFUSED: manifest log: segment id`.
  3. A following edit adds segment 0 under `next_segment` 1, and it is accepted too.
- The branch's own fuzz target aborts on the same two records:
  `.scratch/secprobe/sr1-fuzz-input` (75 bytes) makes `build-asan/fuzz_manifest_log` exit 77,
  `uncaught exception of type strata::corrupt_index: manifest log: segment id`. That comes from
  its `decode_snapshot(snapshot) == state` oracle after `apply()`.
- Attacker: a malicious input file. Gains:
  1. The open accepts a state whose next rollover writes a snapshot that no open can read, so the
     index stops opening after commits it has acknowledged.
  2. A writer that allocates ids from `next_segment` hands out an id a live segment holds (ids 1
     to 5 here), which collides in `segments` and, by name, with that segment's files.
- Fix: in `apply()`, refuse `next[next_segment] < s.next_segment`. The snapshot's invariant (every
  id below `next_segment`) and the existing added-id check then carry it through replay. Add a
  replay-check test using the probe's two records, and a mutant that drops the check (17 + 1).
  Add `sr1-fuzz-input` to `fuzz/corpus/manifest_log/`.
- The owner may also want other counters to be monotone: `last_docid`, `revision`, the applied
  position. A crafted snapshot can already set those, so they are not a replay-only gap. That is a
  design question, not part of this blocker.

### SR2 (medium, condition on step 3's replay): `apply()` costs O(state) per edit

Three parts of `apply()` cost time in proportion to the state or to the square of the edit, not
to the edit's bytes:

1. **Field declarations:** the duplicate-name check scans the edit's earlier declarations for
   each one (`:658`), so it is O(N²) in one edit.
2. **Deletes:** each delete merges the segment's whole deleted vector into a new one and calls
   `entry_bytes` twice, which walks it through `deleted_encoding` (`:689-697`). That is
   O(|deleted|) per edit, however few ordinals the edit adds.
3. **Added segments:** each edit that adds a segment binary-searches every live segment's identity
   (`:628-632`), so it is O(segments) per edit.

Nothing enforces D7's bounds on a log that is read, so a crafted log has no ceiling. Shape 2 also
hits honest logs: D7 budgets 0.07 µs per delete edit, and R = 128 edits is the replay floor.

Measured with probe `.scratch/secprobe/p2_quadratic.cc`, plain `-O2` on macOS x86_64 under a light
hold (`p2.out`, `p2c.out`):

| Shape | Input | `apply()` time |
|---|---|---|
| 1 edit, 20,000 / 40,000 / 80,000 declarations | 189 KB / 389 KB / 789 KB | 1.68 s / 7.64 s / 58.2 s |
| 1,000 / 2,000 one-ordinal delete edits, segment of 2M docs, 1M deleted | 275 KB / 300 KB | 7.0 s / 13.7 s |
| **128 one-ordinal delete edits, segment of 20M docs, 10M deleted** (an honest shape at the R floor) | 2.5 MB | **8.9 s** |
| 2,000 / 4,000 / 8,000 add-segment edits over 100,000 segments | 2.9 / 3.0 / 3.2 MB | 12.4 s / 22.8 s / 46.9 s |

- Attacker: a malicious input file. Gain: a CPU denial of service on open, for minutes from a file
  of 1 MB. The honest row also affects any writer that runs `apply()` per commit (step 3's tracked
  S): about 70 ms per delete commit on that segment.
- Fix, before step 3's replay lands:
  - Check declaration names against a hash set (or `field_table`'s map plus a set of the edit's
    names).
  - Keep the snapshot's identities in a set in `log_state`, or check only the added identities
    against an index.
  - Make a delete edit O(k log n): an insertable deleted structure, or the bitset form kept in
    memory (see SR3), with `sizes.segments` updated by the encoding's size delta instead of
    re-walking.
  - Have the open refuse a log past D7's bounds by a margin as damage, since no honest writer
    leaves one.
- Gate: T7's replay-cost row should include the 20M-doc, 10M-deleted shape.

### SR3 (low): a bitset decodes to 64× its bytes

`get_entry`'s encoding 1 expands each set bit to a 64-bit ordinal (`:236-238`). A 1,000,077-byte
snapshot whose bitset is all set decodes to 7,999,999 ordinals, a 64.0 MB vector (probe
`.scratch/secprobe/p3_amplify.cc`). This is bounded by the input, so it is not a blocker. A crafted
100 MB log needs about 6.4 GB. An honest segment with 20M docs and 10M deleted holds 80 MB of
ordinals for a 2.5 MB bitset, and at millions of open indexes per host that is the memory ratchet's
concern. The ledger chose ordinals because 64-bit words over a deltas body are not input-bounded.
Keeping the bitset form when the record used it (1×), and ordinals for deltas (at most 8×), bounds
both cases and fixes SR2's delete cost too. This is the owner's call.

### SR4 (low): `bad_alloc` in the mutation phase leaves the state half-applied

After the checks pass, `apply()` writes `s.generation` and the scalars first, then allocates
(merges, map inserts, `field_table::append`). A `bad_alloc` there leaves a mix of the state before
the edit and after it. The comment promises only that `corrupt_index` leaves the state unchanged,
which holds. Condition for step 3: the open and the writer discard the state on any exception
from `apply()`, not only `corrupt_index`.

### SR5 (low): the fuzz target did not reach a 75-byte invariant break

Two runs, the developer's 339,203 inputs and this review's 112,830, did not produce an edit that
only lowers a scalar. Add SR1's input as a seed. Also add seeds of one-scalar edits for each of the
14 scalars, so mutation starts next to these checks.

## Conditions

1. **SR1 (blocking for this branch's landing):** the monotone `next_segment` check, its replay
   test, its mutant, and the fuzz seed.
2. SR2: `apply()` linear in the edit, or the open bounding replay by D7, before step 3's replay
   lands, with T7 rerun on the large deleted-set shape.
3. SR4: step 3's callers drop the state on any exception from `apply()`.
4. Step 3, from D6.3 (the ledger assigns it to the caller): a scan that stops inside the first
   record (`records.empty()`) is damage, whatever the tail verdict, and the snapshot's generation
   must equal the log's g (the framing's segment).

SR3 and SR5 are non-blocking.

## Probes (kept in `.scratch/secprobe/`, ignored by git)

- `common.hh`, `p1_next_segment.cc` (SR1, which also writes `sr1-fuzz-input`),
  `p2_quadratic.cc` with `run_p2.sh` and `run_p2c.sh` (SR2; outputs `p2.out`, `p2c.out`), and
  `p3_amplify.cc` (SR3).
- `build.sh` builds these and journal's probes (ASan+UBSan `-O1` and plain `-O2`, Homebrew LLVM,
  `build-asan/_deps` headers).
- `fuzz.sh`, `mlog.dict`, `corpus/`, `fuzz-mlog.log`: the 3-minute fuzz runs of both targets.

**Not run:** TSan (the codec is pure); Linux, FreeBSD and arm64 (left to CI); a fuzz run longer
than 3 minutes; a structure-aware mutator; step 3's open path, which does not exist yet (scan to
replay to state); `diff()` (a test and seed helper, not a decoder).

## Re-check

> Re-checker, 2026-11-04, at `strata:b391783` (the fix `strata:44a5bee` plus the journal repin to
> `journal:df7db00`), worktree `manifest-log-steps12`, ASan+UBSan (Homebrew LLVM) build against
> the local journal worktree at `journal:df7db00`. Probes rerun from `.scratch/secprobe/`.

| Finding | Verdict | Evidence |
|---|---|---|
| SR1 | CLOSED | `manifest_log.hh:602` refuses `next[next_segment] < s.next_segment`; `:622` refuses an added id 0; `:223` refuses id 0 in a record. Probe `p1_next_segment` now stops at "apply refused: manifest log: next segment id lowered" (was ACCEPTED, then a snapshot that no open reads). `manifest_log_test` passes. The 75-byte `sr1-fuzz-input` is byte-identical to `fuzz/corpus/manifest_log/sr1-next-segment-lowered`; `fuzz_manifest_log` on it exits 0 (was 77). |
| SR5 | CLOSED | The seed writer (`manifest_log_test.cc`, one-scalar edits lowered to 0 and raised by one) takes the corpus from 33 to 62 inputs; `fuzz_manifest_log_replay` replays all 62. SR1's input is a seed. |
| SR2, SR3, SR4, D6.3 | RECORDED | `docs/status/manifest-log-steps12.md:146-166`, "Step 3's conditions from the security review": SR2-strata at `:148`, SR3 at `:156`, SR4 at `:160`, D6.3's caller rules at `:163`. Each names its fix and its gate. |

Revert checks (`manifest_log_test` and `fuzz_manifest_log_replay`, one change at a time, restored
after each):

1. The whole `44a5bee` change to `manifest_log.hh` reverted: `replay checks` and `decoder` fail
   ("an added id not zero", "the next segment id never lowered", "the next segment id never below
   a live id" not enforced), and the replay target aborts on the SR1 seed with
   `corrupt_index: manifest log: segment id`.
2. The lowering check out: both "next segment id" tests fail, and the replay target aborts on the
   SR1 seed.
3. The added-id-0 check out: "an added id not zero" fails.
4. The record's id-0 check out: `decoder` fails.

Fuzz: `fuzz_manifest_log`, 2 minutes under ASan+UBSan from the 62 seeds plus the reviewer's
corpus and dictionary: 180,123 runs, coverage 5,118, peak RSS 500 MB, no finding
(`.scratch/secprobe/fuzz-recheck.log`).

The journal repin is the right commit (`CMakeLists.txt:29`, `df7db00`). The ledger's open question
(whether `last_docid`, `revision` and the applied position must also be monotone) stays the
owner's design call, as the review said.

No new findings.

**Not run:** TSan; Linux, FreeBSD and arm64 (CI); `diff()`; step 3's open path; strata's CI at
`strata:b391783`.
