> Status: CURRENT (2026-11-09) how `read_log`, `log_within` and `log_appender` in
> `include/strata/core/manifest_log.hh` work (step 3 of `docs/design/manifest-log.md` revision 1:
> D4, D6, D7). The index does not use them yet; step 3b moves commit and recovery onto them. Records
> and replay: `manifest-log-codec.md`. Framing and the tail rule: journal `docs/mechanisms/framing.md`.

# The manifest log as a file

`manifest-<g>` is journal's framing at 512-byte blocks (`log_stream`: magic `STRLOG01`, the index
UUID as identity, g as segment, epoch 1, no block manifests) around one snapshot of generation g
and then one edit per commit.

## Reading: `read_log(bytes, g, readable, to)`

1. A file that starts with `STRMAN01`, the retired manifest, is another build's index:
   `unsupported_index`.
2. The UUID comes from block 0's header, and `framing::scan` reads the file with the stream it makes.
   A damaged header fails the scan there, so a guessed UUID never reads.
3. The caller's rules (D6.3): no whole record means no whole snapshot, which is damage whatever the
   tail verdict, since a log is renamed into place only after its snapshot was synced. The first
   record decodes as a snapshot whose generation is g and whose UUID is the blocks'.
4. Edits replay in order through `apply()`. After each, the log so far must be within twice D7's
   bounds (`log_within(state, end, edits, 2)`); past them it is damage, so a crafted log cannot make
   the replay cost more than a small multiple of what a fresh log of the same state would.
5. Without `to`, a damaged tail is `corrupt_index` naming the offset and the generation of the
   first record that is not whole; a torn tail is dropped and the result is `dirty`, which makes the
   next commit roll over. With `to`, replay stops there, `to` must have read whole
   (`generation_not_retained` otherwise, also for a `to` below g), and damage after it is reported
   in `damaged` instead of failing: that is what `open(..., to)` and `fallback()` need.

The state is a local of `read_log`. Any exception, an allocation failure inside `apply()`
included, unwinds it with the frame, so no caller ever holds a half-applied state from a read.

## The bounds: `log_within(state, bytes, edits, slack)`

A log stays within `max(F, 2 S)` bytes and `max(R, n, S / c)` edits after its snapshot, S being
`snapshot_bytes(state)` (exact, framed) and n the state's entries (pins, segments, metadata pairs,
declarations). F = 16 KiB, R = 128, c = 512. The writer rolls over before passing slack 1; the
reader refuses past slack 2, which leaves room for a log written under a larger preset.

## Appending: `log_appender`

- `write(record)` frames the record from the log's end E with `framing::Emitter` and writes the
  bytes in one `write_at`. A record larger than a piece (1 GiB by default) goes in pieces that each
  end on a 512-byte boundary, so a crash still leaves only sizes at E, at the new end, or at a sector
  boundary between: never a cut fragment.
- A write that throws or comes back short is undone before the call reports: the file is
  truncated back to E and `sync_data`'d, so the log stays clean and a retry appends at E with no
  rollover. `ENOSPC` reaches the caller as it was thrown.
- `sync()` is the barrier: `sync_data`, then E moves past the record. One record is in flight:
  `write` refuses while the previous one is unsynced.
- A truncate-back or a barrier that fails breaks the appender; every later call throws
  `std::logic_error`, and the index fences.

## Proof

`test/manifest_log_test.cc` stages "caller rules", "crash images" (every subset of the sectors an
append touches at every size a crash leaves, over 40 logs: 2,484 images, 0 misread), "bit flips"
(35,200 single-bit flips, 0 silent, 40 in padding), "bounds on read" (each prefix reads exactly when
the oracle says it is within the bounds, by edits and by bytes), "replay cost" (time ratios: ten
times the declarations in one edit about 15x, edits adding segments onto a hundred times the state
about 1.6x), "allocation failure" (every allocation of a read failed in turn), and "appender" (over
faultfs). `test/manifest_log_mutants.py` breaks each rule once; `fuzz/fuzz_manifest_log_file.cc`
fuzzes `read_log` from framed seeds (`manifest_log_test --log-seeds`).
