> Status: DRAFT (2026-10-29) `defer_io` and `work_pool_executor` as built on `phase7c/l5-defer-io`; the design is Xapiand `docs/design/phase7c.md` D4, D6 (R5, C15, C16 of `docs/reviews/phase7c-design.md`). Not reviewed.

# defer_io: an apply that does no I/O

A host that runs `apply` on an event loop (Xapiand's reactors) cannot let it flush a segment, drain
files or wait for a delete. `index_options::defer_io` makes `apply` report that I/O as owed and
return, and the host runs it later as a chore on a worker. With it off, `apply` is the code it was.

## How it works

```
 apply(t) ----------> accepted | rejected          no device call
    |
    +- buffer past its share / lane full ..... flush_owed  --> host: refresh()            --> apply(t) again
    +- storage permit short, files queued .... drain_owed  --> host: reclaim_for_space()  --> apply(t) again
    +- chores a check-in left owed ........... stay owed: chores_owed()  --> host: check_in()
```

- **Flush.** `apply` flushes first when the buffer is past its share, the transaction does not fit,
  or the buffer's documents plus the transaction's pass the cap. Under `defer_io` it answers
  `verdict::flush_owed` instead, after the replay check (a position already applied is `skipped`)
  and before anything is applied: the position, the buffer and the version map are untouched, so the same
  transaction at the same position is applied after `refresh()`.
- **Drain.** Storage admission calls `drain()` when the permit is short. Under `defer_io` it answers
  `verdict::drain_owed` (nothing reserved or applied) when a drain has something to do: files in the
  reclaim queue, deletes posted to the executor, or removals awaiting a directory sync.
  `reclaim_for_space()` is the chore: reclaim every queued file, wait for the posted deletes, sync
  the directory.
- **Owed chores.** A check-in that meets the lease held leaves its chores (publish finished merges,
  free views readers released and queue their files, `retain()` with its manifest reads) to the
  holder. Under `defer_io` `apply` neither runs them at its end nor at the lease handoff; they stay
  owed (`chores_owed()`), and the host calls `check_in()` as a chore. A host that never calls
  `check_out` (Xapiand) never has any; the idle task's `reclaim()` frees released views.
- **Owed answers end (C15).** `flush_owed` is answered only while the buffer holds something a
  flush would write, so an oversize transaction on an empty buffer is `invalid`, and after at most
  one `flush_owed` on a non-empty one. `drain_owed` is answered once per round: after
  `reclaim_for_space()` ran for an answer, the next `apply` that reaches admission answers `no_space`
  if the room is still short, whatever the chore freed; a full disk with nothing queued answers
  `no_space` at once.
- **Not covered.** A segment not yet mapped (`eager_segments` off) maps at its first read inside
  `apply`; a replace-by-key read of a mapped segment can page-fault (T6, gated at Phase 6).
  `install()` is itself a chore (it reads and writes whole files) and still drains inline.

## work_pool_executor

`strata::work_pool_executor` (`work_pool_executor.hh`) is the executor concept over threadpool's
`WorkPool`: each group the engine opens is a `WorkGroup` of width `per_group`; every item is
merge-class, so strata's merges and reclaim deletes run behind queries and flushes, age past a
flush flood, and never take the pool's reserve workers (the pool needs a worker beyond its reserve).
Items are caller-owned `WorkItem`s allocated here; a cancel hands the queued ones back and they are
freed here. Waits off the pool's workers reach `WorkPool::Options::block_guard`.

## Invariants

- With `defer_io`, `apply` makes no `IO` or `File` call: `test/defer_io_test.cc` asserts
  `faultfs::filesystem::calls()` is unchanged across the apply on each owed path, and that the same
  steps without `defer_io` do make calls.
- An owed answer applies nothing, and the retry at the same position is accepted, not skipped.
- An owed answer is not repeated when the chore cannot help (C15): the oversize, lane and
  failed-removal tests.
- With `defer_io` off the verdicts, the mutating calls and the file bytes are those of the tree
  before the option (`default_path_unchanged`, captured there).

## Code

`index.hh`: `apply`, `writer_scope`, `reclaim_for_space`, `drain_pending`. `faultfs.hh`: `calls()`.
Tests: `defer_io_test.cc`, `work_pool_executor_test.cc`, `background_merge_test.cc workpool`.
Ledger: `docs/status/phase7c-l5.md`.
