# Encode once: each batch's drain writes its terms' final blocks

Targets the one-thread indexing row (`perf/inverter`, design target 4.5× Tantivy-0.26-st)
at 100,000 documents of the gate corpus, keyed, `STRATA_BUDGET_MB=2056`: the CPU the
inverter spends encoding every posting list twice. Base: `perf/inverter` at `75b8715`.

## Hypothesis

Under format 12 a 128-posting block spans batches, so each list is encoded twice: into
LEB128 run entries at each batch's drain (0.30-0.33 s at 100,000 documents) and, after the
flush decodes the runs (0.30 s), into the segment's PFOR blocks and skip data (most of
`flush_from`'s 0.56 s); `inverter.md`, "What bounds the kept series against 4.5×". If each
drain writes its terms' final blocks once (full 128-posting blocks, the piece's remainder
in the tail codec, the same for positions), the flush merges the runs by term and appends
each term's pieces, rewriting only the skip data, offsets, and the dictionary. The lead's
estimate: about 1.18 s of encoding and decoding becomes about 0.65 s, a quarter of the CPU.

The layout is format 13's bit 14 (`postings_runs`, "Copy merges" in
`docs/design/format-13.md` on `phase4/format13`): a per-segment table of run starts gives
each block's base, max(previous last, the start of the run holding its last − 1); short
blocks anywhere, a third codec code; position tails mid-stream.

## Procedure

1. Bound: the drain encodes blocks (PFOR, the remainder LEB128) and the flush appends the
   pieces and builds the skip data from the drain's block records, with no decode and no
   second encode. If it saves under 10% of the CPU at 100,000 documents, stop.
2. Prototype: the same, writer-only but exact, with a test decoder for the run layout that
   reads every list back; every posting, wdf, and position equal to today's segment for
   the same 100,000 documents; every other section byte-identical.
3. Interleaved A/B against `75b8715`, at least seven rounds at 100,000 documents, on macOS
   under a light hold and once on the gate VM (tmpfs index root, `AB_WARM=1`): CPU by
   phase, wall time, peak RSS, segment bytes.

Keep rule: CPU and wall time better by at least 10% on both platforms, every list equal,
the other sections byte-identical, segment bytes stated. Otherwise recommend planning
differently, with the numbers.
