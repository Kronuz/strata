# encode-once (perf/encode-once)

## State

Started 2026-10-18 from `perf/inverter` at `75b8715`. Plan written (`docs/perf/encode-once.md`).
The prototype is drafted, not yet compiled: strict host holds (`inv-confirm-5m`, then
`inv-confirm-5m-nomerge` until 10:10) block compiling; the gate VM is held until about 10:30.

- `tuple_buffer.hh`: `STRATA_ENCODE_ONCE=1` makes each drain write a piece's final blocks
  (`encode_piece`), `decode_once` reads them (digest, deletes, threads), and the flush hands
  each term's pieces to `flush_from` (`pieces_of`), re-encoding a piece only when it lacks wdf
  or count blocks its term needs.
- `writer.hh`: `write_piece_positions` and `write_piece_postings` append the pieces and build
  the skip data from their records (`write_skip`, shared with `write_postings`); section 12
  holds the run table. Phase timers under `-DSTRATA_INST`.
- `tools/encode_once_check.cc`: the test decoder for the run layout.

## Done

- Worktree `~/code/Kronuz/strata-wt/encode-once`, branch `perf/encode-once`.

## Next

1. Compile; correctness at 20,000 and 100,000 documents (`encode_once_check`), and the
   delete and thread paths byte-identical to today's.
2. The bound (phase timers) at 100,000 documents on macOS.
3. The interleaved A/B on macOS, then the gate VM after its hold.

## Blocked on

Nothing.
