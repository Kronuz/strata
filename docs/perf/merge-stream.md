# Merges that stream (perf/merge-stream)

Targets: peak RSS and the memory budget's peak on big1 (fillrandom and readrandom at 100 B,
1M records, M = 12 MiB; ycsb_a at 1 KiB, 200k records, M = 24 MiB), without moving fits1.
Origin: the explorer's `docs/perf/bigmem.md` (explore/bigmem, 7eb2c11): a merge builds its
output sections in memory, charged to the budget without waiting, so working memory grows with
the merged segment (budget peak 96-419 MiB against a 3-6 MiB share).

Goal: a merge's working memory is a few blocks plus the key builders' state, not the merged
segment; segment bytes identical to main's for the same inputs; no format change.

Harness: `.scratch/ms/abm.py` (interleaved trials of run.py's rows, A/A first, six trials),
kv_strata Release, macOS arm64, `quiet run merge-stream` (strict). Identity: `KV_GET_DIGEST`
on every run, and `test/merge_stream_test.cc` (digests of merged files pinned from main).

## Profile (main, trace of `flush_charge` held/peak per merge entry)

The stored plugin's merge dominates. On ycsb_a a merge that writes a 40 MB segment holds
141 MB after its stored entry and peaks at 219 MB; keys hold 1-2 MB per 10k docs. The cause is
`stored_builder`: every record is copied into `stored_` (doubling growth), the whole section is
encoded into one string (doubling again), then `append_built` writes it, and the charge is not
returned until the merge ends. On fillrandom (100 B) stored is again the bulk (80 of 98 MiB
at 22 MB of output); keys add ~30 MB per 300k docs (the filter's `64 n + 1 MiB` build set).

## Experiments

### 1. Stream the stored section (keep: see numbers)

Hypothesis: gathering one block of records at a time and writing each frame through the
section stream, keeping only the 16-byte index entries, removes the copy and the encoded
string, so the stored entry holds a block. Bytes cannot change: the block-closing rule
(count at the target, or bytes past it) is applied record by record instead of by lookahead.
Flushes use the same frame writer, so their encoded string is gone too.
Keep if peak RSS and budget peak fall, throughput does not drop, and the digests match.
