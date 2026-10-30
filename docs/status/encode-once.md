> Status: REJECTED (2026-10-18) the branch's last commit removes encode-once ("plan differently").

# encode-once (perf/encode-once)

## State

Finished 2026-10-18; verdict "plan differently" (`docs/perf/encode-once.md`). The bound saves
7.5% of the CPU at 100,000 documents, under the 10% bar. The correct prototype is 1.066×
wall [1.060, 1.067] on macOS and 1.041× [1.029, 1.061] on the gate VM (same binary, path on
against off), with peak RSS 5-7% lower and the flushed segment 10.3% larger (postings
+24%, 41.5% of postings in short blocks). The code is removed from the branch head; the
prototype, its checker, and its test are at `72d1928`.

## Done

- `8e37a69` plan and ledger.
- `4fbde39` prototype drafted under the host holds: the drain writes final blocks
  (`STRATA_ENCODE_ONCE=1`), the flush appends pieces, the skip data comes from the records,
  and section 12 holds the run table.
- `55ad4a9` `tools/encode_once_check.hh` (the run layout's test decoder) and
  `test/encode_once_test.cc` (randomized, batches of 1 to 2^20 tuples, re-encoded pieces,
  decode paths).
- `72d1928` small groups keep LEB128; short blocks' pairs computed at the flush (bound 4% to
  7.5%).
- Correctness: every list equal at 20,000 and 100,000 documents on macOS and the VM; other
  sections byte-identical; a deliberate mutation fails 18 checks.
- A/B at 100,000 documents, seven interleaved rounds, on macOS (light hold) and the gate VM
  (tmpfs, warm, light hold).
- Final commit: results, and the prototype's code removed.

## Next

1. If bit 14 is to be judged, judge it on copy merges (format 13's C12: a merge appending
   blocks against a re-encoding merge, at sources of 1,000 to 100,000 documents). This
   item's step 3 was not run.
2. For the format designer: the base rule fails for a block spanning runs, a short block
   needs its count, and random access into mid-stream position tails needs the run's first
   position (`docs/perf/encode-once.md`, "For format 13's bit 14").

## Blocked on

Nothing.
