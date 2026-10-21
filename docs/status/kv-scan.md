# kv-scan: the core's key-ordered scan

- Task: `basic_index::scan` (format-14-kv D3, "Then: step 2"), finishing the explorer's verified
  win (`explore/scan` at `8fa87a2`, `docs/perf/scan-lazy-merge.md` on that branch): a lazy k-way
  merge over the buffer's sorted view and each segment's key cursor, then `bench/kv/kv_strata.cc` a
  thin caller. Stays out of `writer.hh` and the flush path (`phase4/flush-memory`).
- Branch / worktree: `phase4/kv-scan` from main `f777ef9`, at
  `/Users/kronuz/code/Kronuz/strata-wt/kv-scan`. Not merged by the developer; a new shared public
  API, so it takes one code review before it lands.

## Done criteria (the lead's)

- [ ] Newest wins for a key in several sources (buffer, then newest segment), deletes hide older
  copies, updates and inserts between scans are seen; each tested, with a mid-scan insert and a
  key deleted in the buffer but live in a segment.
- [ ] Empty index, from past the last key, limit 0 and 1, keys on block and restart boundaries.
- [ ] The explorer's digest at seed 42 (`81bb0f0b7fe8d333`) unchanged through the core API;
  ycsb_e CPU per op within noise of 21.7 us (one quiet run).
- [ ] Full ctest, ASan+UBSan, a revert check per behaviour, one mutant on the tie rule.

## State

Implemented; tests pass; verification in progress.

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

(see `git log`)

## Next

Review, then the lead lands it.

## Blocked on

Nothing.
