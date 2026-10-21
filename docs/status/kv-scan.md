# kv-scan: the core's key-ordered scan

- Task: `basic_index::scan` (format-14-kv D3, "Then: step 2"), finishing the explorer's verified
  win (`explore/scan` at `8fa87a2`, `docs/perf/scan-lazy-merge.md` on that branch): a lazy k-way
  merge over the buffer's sorted view and each segment's key cursor, then `bench/kv/kv_strata.cc` a
  thin caller. Stays out of `writer.hh` and the flush path (`phase4/flush-memory`).
- Branch / worktree: `phase4/kv-scan` from main `f777ef9`, at
  `/Users/kronuz/code/Kronuz/strata-wt/kv-scan`. Not merged by the developer; a new shared public
  API, so it takes one code review before it lands.

## Done criteria (the lead's)

- [x] Newest wins for a key in several sources (buffer, then newest segment), deletes hide older
  copies, updates and inserts between scans are seen; each tested, with a mid-scan insert and a
  key deleted in the buffer but live in a segment.
- [x] Empty index, from past the last key, limit 0 and 1, keys on block and restart boundaries.
- [x] The explorer's digest at seed 42 (`81bb0f0b7fe8d333`) unchanged through the core API;
  ycsb_e CPU per op within noise of 21.7 us (one quiet run).
- [x] Full ctest, ASan+UBSan, a revert check per behaviour, one mutant on the tie rule.

## State

Built and pushed; not merged. Awaits the one code review (new shared public API). Evidence:

- `scan_test` (plain and mapped) covers: the map oracle over random puts, deletes, flushes, merges
  and a reopen; empty index; limits 0, 1, past the count; `from` before, on, between, and past
  the keys; `[from, to)`; a void and a stopping `f`; newest wins; deletes and replaces hiding
  segment copies; the tie rule with the invariant broken through test access (buffer over segment,
  newer segment over older, the later of the buffer's two runs); the snapshot rule (seven writer
  calls from `f` throw, the index and its digest are unchanged, a nested scan and a get work);
  `from` on every key and every gap of a 3,000-key multi-block segment plus a second segment and
  the buffer; the buffer's view across folds, flushes, and a rejected transaction.
- Mutants, each failing `scan_test`: oldest wins ties (3 CHECKs); no guard in `live()` (a
  mid-scan apply corrupts the scan: `corrupt_segment`); buffered dead flag ignored; segment live
  bits ignored; view not reset by `clear_buffer`; `to` ignored; only one tied source advanced.
- `fuzz_stored` now walks the lazy cursor from several keys and traps unless it equals the checked
  walk, and also mutates the keys and key_index sections. Mutant (`range` seeking from the start):
  `fuzz_stored_replay` traps (exit 132); restored: 252 inputs replay.
- Full ctest 25 of 25 plain and under ASan+UBSan (Homebrew LLVM 23).
- Digest at seed 42, 100k records, 100k ops: `81bb0f0b7fe8d333` and value bytes 5,088,720 through
  the core API (the explorer's). The 20k-record digest is equal between the ASan and plain builds.
- CPU per op, ycsb_e fits row, run phase: the host was loaded (load average 10 to 17, others'
  light holds), so absolute numbers run 4 to 5 times the explorer's 21.7 us. Interleaved five runs
  each with the explorer's binary (`8fa87a2`) under `quiet run`: explorer 108.6 to 118.1 (median
  114.7), core scan 103.9 to 110.8 (median 104.4), same digest. One unheld run on a quieter
  moment: 19.7 us. The lead's quiet measurement on main decides the absolute figure.

## Decisions

- API as the design's D3: `scan(from, n, f)` and `scan(from, to, n, f)`, n 0 meaning no limit,
  `f(key, value, version)` returning false to stop (a void f runs to the end); returns the count.
  The pulled `cursor` stays for later (snapshot handles).
- Snapshot rule, made checkable: a scan sees the index as it began; any writer call (apply,
  refresh, merge, commit, ...) from `f` throws `std::logic_error` through `live()` and changes
  nothing. Gets and nested scans from `f` work.
- The buffer's sorted view is a main run and a delta of at most 4,096 records (design D3), built
  when a scan asks, dead records dropped at a fold, reset with the buffer. Its bytes join
  `map_bytes()` (charged at the next apply). One line each in `map_bytes()`, `clear_buffer()`,
  `live()`: the only touches outside the new code, near the flush-memory branch's area.

## Done

- `ffeccee`: the scan, the sorted view, the guard, `kv_strata` as a thin caller, `scan_test`.
- the next commit: the fuzz target's cursor walk and keys sections, core-split.md, this ledger.

## Next

The code review; then the lead lands it. Later: a heap for the merge when segment counts grow
(a linear head scan today, as the explorer's), the pulled `cursor`, the arena buffer's own view
(step 4). The search layout's buffer (`inverting()`) takes the same accessors but is untested
here: prospect should run its own scan test when it adopts this.

## Blocked on

Nothing.
