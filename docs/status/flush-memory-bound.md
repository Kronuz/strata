# flush-memory-bound

Branch `fix/flush-memory-bound` (from origin/main). Small fix tier.

## State

Fixed in the test; proven.

## Cause

`writer_holds_no_copy()` (test/flush_memory_test.cc) samples the shared `byte_budget` at the
first `write_at` of a threads = 3 flush. The file sink gathers up to 1 MiB (index.hh
`file_sink::gather`), so a run's first write is its `sink.finish()`, after its writer is built.
The other two runs are still alive then, and each draws a transient ~1.06 MiB for its key
filter's build scratch (`keys::filter_build_bytes(n)` = 64 n + 1 MiB, keys.hh:68; 200 keys a
run), released later. Charges per run: 262144 at start, ~1.08 MB at the filter build, ~276 KB
after. The flush's peak draw is 3.49 MB over `before`; the bound was 2.46 MB. When the first
writer finishes while two others are inside the filter build, the sample passes the bound with
no record copied. The writer holds none: the scratch does not depend on the record size.

Evidence: built ASan+UBSan -O1 (Homebrew LLVM), mapped variant, `taskpolicy -b`, 32 at once:
the unmodified test failed `flush_memory_test.cc:393` 10 of 300 runs (0 of 200 at 12 at once,
0 of 100 at 4). Traces (temporary fprintf, reverted) showed the 1,061,376-byte `flush_charge`
grow per run and a first-write sample of 1.09 MB when quiet.

## Fix

The test, not the code. Records go from 16 KiB to 64 KiB so a quarter of them (9.8 MB) clears
every run's scratch twice over, and a CHECK states that relation from `filter_build_bytes` and
`flush_charge::quantum`. The bound stays a quarter of the records.

## Proof

1. Before (unmodified test, ASan+UBSan -O1, mapped, `taskpolicy -b`): 10 of 300 runs fail at
   the line-393 CHECK with 32 at once; 0 of 200 at 12 at once. Failures kept in
   `.scratch/old-failures/`.
2. After (f0ba032): 0 of 600 at 32 at once; both variants pass with leak detection.
3. Mutant: `w.borrow_stored(lease)` removed in `flush_documents` (the writer copies the
   records): the new test fails both thread variants at the same CHECK.

## Done

f0ba032 the test fix and this ledger.

## Next

Nothing; CI runs on push (not watched).

## Blocked on

Nothing.
