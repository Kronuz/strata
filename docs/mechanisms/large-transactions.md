# Large transactions on a logged index

A logged index admits any transaction an unlogged one admits, whatever its encoded size, and
recovers it whole or not at all. A command past `options::command_bytes` (the record cap, 64 MiB
by default and 1 MiB in `kv_strata`) is written as a run of chunk records. Design:
`docs/design/rocksdb-parity.md` A3. Code: `index.hh` (`log_state::join_chunk`, `join_chunks`,
`replay_log`) and `command.hh` (`chunk_payload`, `decode_chunk`). Test: `test/test_large_tx.cc`.

## The bytes

A chunk record is a turn-log `Batch` record whose strata payload has kind 3:

| Byte | Meaning |
|---|---|
| 0 | `3`, the chunk kind |
| 1 | `1`, the version |
| 2 | the place: `1` first, `2` middle, `3` last |
| 3.. | a slice of the transaction's command encoding, at least one byte, to the payload's end |

A run is one first chunk, any middle chunks and one last chunk, each in a record of its own, at
consecutive sequences of the group. The concatenated slices are the command, which decodes as any
other. Batch records (kind 1) are unchanged, so a log with no large transaction has the same bytes
as before.

## Writing a run

```
apply(t): stage 1 accepts under the lease -> encode -> join_chunks
join_chunks: for each slice of command_bytes - 3:
    join_chunk -> {s, 1}; wait until the loop seals it; wake the loop
last slice:  applied = {s_last, 1}; the record's V includes t
```

1. Stage 1 runs first (apply first, then log), so a run is only ever written for an accepted
   transaction and needs no abort record.
2. The lease covers every chunk: no checkpoint, flush or close of the index lands inside a run, so
   a checkpoint's `covers` never falls inside one.
3. A chunk closes the open batch record first and takes the next sequence. It joins under the same
   turn bound as a batch record (`turn_records`), so the group holds at most one unsealed record and
   a run takes one turn per chunk. Other groups on the loop keep their own records in each turn.
4. Each non-last chunk's record carries the V before the transaction, and the last one carries V
   with it. The watcher therefore makes the transaction visible to views only once its last chunk
   is durable, and `await_durable(applied())` waits for that same record.

Memory is the command's encoding (T) beside the applied form, plus one chunk being sealed.

## Replay

Replay keeps the open run's slices above the head's position:

- A first chunk opens a run. A last chunk closes it and applies the decoded transaction at
  `{s_last, 1}`, skipped at or below the head as any position is.
- A middle or last chunk with no open run is `log_lost`.
- A run that a batch record, another first chunk or the log's end interrupts was never
  acknowledged: the writer died inside it. Replay drops it and moves the applied position to its
  last chunk's, so the index's next checkpoint covers those records and no later life reads them.

## Limits

- A run costs one barrier per chunk; at 1 MiB chunks a 64 MiB transaction takes about 65 barriers
  before its acknowledgement.
- Its records stay live until the checkpoint after it, so the log holds the whole run, and records
  in segments past `retain_segments` move forward while it lasts.
