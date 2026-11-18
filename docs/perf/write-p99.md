> Status: SUPERSEDED (2026-10-20) landed as strata:5737867 (exploration log).

# Write p99 at 2x memory (twox1 fillrandom, M = 5 MiB, 100-byte values)

Branch explore/write-p99. Host: this dev Mac, quiet-run light holds, one thread, 100k ops, 4 interleaved rounds.

## Where the slow ops go (before any change)

Per-op trace (>5 us) plus timing inside `refresh()`: of a 0.53 s run, ~480 ms is in ops over 5 us;
61 ops over 1 ms (343 ms; the first ~10 buffer-share flushes, 5-9 ms each, inline on the writer) and
283 ops of 100 us - 1 ms (124 ms). The 100 us - 1 ms ops are `refresh()` calls: ~590 flushes in
100k ops, one per ~170 documents, ~330 us each, ~100 segments alive at once.

Cause: `needs_flush()` returns `budget->pressure()` whenever `used >= capacity` (1.25 MiB = memory/4).
In-flight merges (2 pool threads) hold 1.5-6 MiB of the budget past capacity, so pressure is true on
nearly every apply and the writer flushes a near-empty buffer each time. A tiny flush cannot relieve
pressure held by merges; it only makes more segments, hence more merge work holding budget (feedback).
The flush itself is inline on the writer; the pool is used for merges only.

## Experiment 1 (win): pressure flushes only a meaningful buffer

`needs_flush()`: under pressure, flush only if buffered + map bytes >= buffer_share / D (switch
`KV_PDIV=D`, 0 = old behaviour). Buffer overshoot is bounded by share/D.

fillrandom, A/A (D=0 twice) then D=4, per round (ops/s, p99 us, p99.9 us):

| D | ops/s | p99 | p99.9 |
|---|---|---|---|
| 0 | 138k-193k | 11.8-14.4, or 200-290 in ~1/3 of runs | 450-560 |
| 0 (A/A) | 112k-178k | same bimodal | 430-610 |
| 4 | 245k-316k | 3.1-5.4 | 29-85 |
| 2 | 200k-250k | 4.7-6.4 | 0.8-1.1 ms (bigger flushes) |
| 8 | 112k-290k | mixed (5-290) | mixed |

D=4: ~1.7x throughput, p99 3-4x (up to 50x vs the bimodal baseline runs) lower, p99.9 ~10x lower.
A/A spread of D=0 is ~25% on ops/s; the gap is far outside it.

overwrite (3 rounds, D=0 A/A vs 4): 141-155k -> 264-280k ops/s; p99 12.6-201 -> 4.5-5.2; p99.9 520-840 -> 42-85.
fillseq (3 rounds): 195-233k -> 351-381k ops/s; p99 6.2-11.5 -> 2.2-2.4; p99.9 430-530 -> 34-47.
Peak RSS unchanged (95-135 MiB fillrandom). Segments at the end 1-8 in both.

Identity: a full key-order scan of every live key and stored value, hashed (`KV_DIGEST=1`), is
identical across D = 0, 2, 4, 8 for fillrandom (6 runs), overwrite and fillseq (A and B each).
Not run: sync/wal durability, multi-thread, many-index rows, the other twox1 rows (ycsb), 1 KiB values.

## Remaining tail

p99.9 ~40 us and max are now the buffer-share flushes (5-9 ms inline) and, at D=2, bigger ones.
Removing those needs the flush off the writer thread (a second buffer and a handoff): design work, not tried.

## To finish

Make the threshold a named option rather than an env switch (default share/4 looks right), decide
whether the tracing (`KV_TRACE`, refresh timing) stays, re-run the full twox1 set and the fits rows
(confirm no regression), and add a test that a merge-held budget does not cause per-document flushes.
