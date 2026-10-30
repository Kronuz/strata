> Status: REJECTED (2026-10-18) the branch's last commit removes encode-once ("plan differently").

# Encode once: each batch's drain writes its terms' final blocks

Targets the one-thread indexing row (`perf/inverter`, design target 4.5× Tantivy-0.26-st)
at 100,000 documents of the gate corpus, keyed, `STRATA_BUDGET_MB=2056`: the CPU the
inverter spends encoding every posting list twice. Base: `perf/inverter` at `75b8715`.

**Verdict: plan differently.** The bound saves 7.5% of the CPU, under this item's 10% bar.
The correct prototype is 1.04-1.07× faster with the same binary's path on against off. Its
flushed segment is 10.3% larger (postings +24%), because 41.5% of the postings land in
short blocks. The code is removed from this branch; the prototype is `72d1928`.

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

## Mechanism (the prototype, `STRATA_ENCODE_ONCE=1`)

- The drain: a group of fewer than 128 postings and positions keeps today's LEB128 entry,
  its ordinals from the run's start − 1, plus its last ordinal and its most positions. A
  larger group is gathered into columns and written as the segment holds it: FOR deltas or
  a bitset and PFOR wdfs per 128 postings, PFOR counts and positions per 128, one record per
  block (last, bytes, bitset, first position, its block's offset, count offset, competitive
  pairs), and the remainder in the LEB128 sections.
- The flush (all documents, none left out): per term, each run's piece as it is, unless the
  piece lacks wdf or count blocks its term needs (its own were all zero, or all equal to
  its wdfs). Such a piece is decoded and encoded again. Positions are the pieces' blocks and
  tails back to back. Below 128 postings the pieces' tails form one tail, each first delta
  rebased; that is today's bytes. Otherwise every piece's blocks are kept, with its
  remainder as a short block (code 2, its count first), except the last piece's remainder,
  which is the list's tail. The skip data is rebuilt from the records (`write_skip`, shared
  with `write_postings`), and a short block's pairs come from its LEB128 bytes. Section 12
  holds the run table.
- Documents left out, the key-range steps, and the digest's scan decode the new entries
  (`decode_once`) and write format 12 as today.

## Results

### The bound (phase timers, macOS, 100,000 documents, three interleaved runs each)

An instrumented build (`-DSTRATA_INST`) of this branch, the path off and on. The path-off
side is today's code (the A/B below shows it equal to the base on macOS); its column spans
both sets of three runs, but for one run whose writes stalled (sections 0.455 s, flush
1.276 s, CPU 1.91 s).

| Phase | Today | Encode once (first) | Encode once |
|---|---:|---:|---:|
| Drain: grouping (`group_run`) | 0.240-0.248 | 0.243-0.247 | 0.242-0.245 |
| Drain: encoding the entries | 0.301-0.321 | 0.516-0.525 | 0.389-0.392 |
| Drain: sorting the terms, copying the run | 0.115-0.123 | 0.121-0.124 | 0.118-0.119 |
| Flush: merging the runs, decoding (today) or taking the pieces | 0.276-0.284 | 0.158-0.161 | 0.137-0.140 |
| Flush: the terms (encoding or appending, skip data, dictionary) | 0.417-0.433 | 0.269-0.276 | 0.340-0.350 |
| Flush: the per-document sections and the writes | 0.036-0.062 | 0.060-0.076 | 0.032-0.036 |
| CPU, user + system | 1.85-1.91 | 1.81-1.85 | 1.71-1.74 |
| Run bytes (18 batches, 1,138,835 entries) | 68.2 MB | 70.8 MB | 66.8 MB |

The first version gathered every group into columns and gave every remainder its
competitive pairs at the drain, which saved 4%. The second keeps LEB128 for small groups
and computes a remainder's pairs at the flush, only for terms with skip data. It saves 7.5%
of the CPU (1.86 to 1.72 s): the drain spends 0.085 s more, the flush 0.22 s less. That is
the bound, under 10%.

The estimate missed because the 1.18 s it counted was not all encoding and decoding:

1. The merge is half of the flush's 0.28 s "merging and decoding". With the decode gone,
   0.14 s is left.
2. The flush's PFOR encode was about 0.1 s of its terms phase: appending still costs
   0.34 s against 0.42 s. Building the skip data and the dictionary stays, and the run
   layout has three times as many blocks to describe, with 0.07 s of pairs computed for
   short blocks.
3. Encoding blocks at the drain costs 0.085 s more than LEB128 (columns, PFOR, records,
   pairs per block), as E37 found.

### A/B (seven interleaved rounds, 100,000 documents, medians)

| | macOS | Gate VM (tmpfs, warm) |
|---|---:|---:|
| Base `75b8715`: wall, CPU | 2.100 s, 1.850 s | 1.800 s, 1.800 s |
| This branch, path off: wall, CPU | 2.100 s, 1.870 s | 1.770 s, 1.770 s |
| This branch, encode once: wall, CPU | 1.980 s, 1.740 s | 1.690 s, 1.680 s |
| Base / encode once: time | 1.056 [1.045, 1.077] | 1.071 [1.046, 1.091] |
| Base / encode once: CPU | 1.063 [1.052, 1.074] | 1.072 [1.047, 1.091] |
| Path off / on, same binary: time | 1.066 [1.060, 1.067] | 1.041 [1.029, 1.061] |
| Path off / on, same binary: CPU | 1.075 [1.057, 1.081] | 1.048 [1.029, 1.061] |
| Base / path off: time | 0.995 [0.990, 1.010] | 1.017 [1.000, 1.029] |
| Peak RSS: base, off, once | 261, 263, 242 MB | 213, 213, 202 MB |
| Index bytes: base, once | 43,493,711, 47,965,880 (+10.3%) | the same |
| Throughput: base, once | 47,600, 50,500 docs/s | 55,600, 59,200 docs/s |
| Tantivy-0.26-st: wall | 5.95 s | 6.30 s |
| × Tantivy: base, once | 2.83×, 3.01× | 3.50×, 3.73× |

Intervals are 95% bootstrap intervals of the per-round ratio. The change's own effect is
the same-binary row. On the VM the branch's path-off build is 1.7% faster than the base
(code layout around the templated encoder), so base / once overstates it there. The
trials ran on the full 2,056 MB budget, one segment each.

Savings at scale: about 0.12 CPU-seconds per 100,000 documents, 1.2 CPU-seconds per
million. At a billion documents a day that is about 0.33 CPU-hours a day, under two cents
at cloud vCPU prices, so the gain is worth something only as part of the ratio against
Tantivy, and it costs 10% of disk in every flushed segment.

### Correctness

- `encode_once_check` (the test decoder for the run layout, `72d1928`'s
  `tools/encode_once_check.hh`): today's segment and the encode-once segment of the same
  documents. It decodes every list of both with one decoder and checks today's against the
  real reader (`postings_iterator`, `positions_reader`). It checks the run layout's skip
  data against its own lists: level-1 offsets and lasts, every block's last, competitive
  pairs, first position, position-block and count-block offsets, and the tail's fields. It
  requires every ordinal, wdf, count, and position to be equal, the dictionaries equal term
  by term (df, cf, flags, single postings, whole-list pairs), and the header, lengths,
  stored, docids, stats, norms, versions, and pk_filter sections byte-identical.
- At 20,000 documents (4 runs) and at 100,000 (18 runs), on macOS and on the gate VM: all
  of it holds, with the same bytes on both platforms. At 100,000: 438,367 terms and
  8,900,623 postings.
- `encode_once_test`, three seeds of 2,000 random documents: tuple_buffer_test's documents
  (hash collisions, 255-byte and longer terms, positions past 24 bits, keys, positionless
  terms, wdfs apart from counts), plus two terms that force re-encoded pieces (5,076 of
  them). Batches run from 1 tuple to a million, with two hash masks. Every flush passes the
  checker (205,856 short blocks). With documents left out the segment is byte-identical to
  today's, and the key-range steps and the digest's scan give today's results. A mutation
  that drops a third of the short blocks' wdfs fails 18 checks.

### What the run layout costs at a flush (100,000 documents, 18 runs)

| | Today | Encode once |
|---|---:|---:|
| Postings blocks | 52,652 (10,053 bitsets) | 155,828 (9,776 bitsets, 129,790 short) |
| Postings in short blocks | 0 | 3,696,026 (41.5%) |
| Postings in tails | 1,860,068 | 1,570,634 |
| Position blocks of 128 | 121,752 | 85,752 |
| Positions in LEB128 tails | 2,718,899 | 7,326,899 |
| Postings section | 15,971,722 | 19,799,506 (+24.0%) |
| Positions section | 22,446,072 | 23,085,536 (+2.9%) |
| Terms (the term infos' lengths) | 4,230,390 | 4,230,777 |
| Run table | none | 76 |
| File | 43,493,556 | 47,965,724 (+10.3%) |

A batch is about 5,500 documents, so a term with up to about 128 × 18 postings in the
segment arrives as pieces under 128. Each piece becomes a short block with its own skip
entry and pairs, decoded by LEB128 at query time. Those short blocks are the 41.5% of
postings, and positions likewise move from PFOR blocks into tails.

## For format 13's bit 14 (the copy-merge design)

1. The base rule, max(previous last, the start of the run holding its last − 1), holds by
   construction for pieces. It fails for a block that spans a run boundary, whose base
   would pass its first ordinal. A per-term choice to re-encode inside a bit-14 segment
   must still cut its blocks at run starts, or the rule needs a per-term or per-block
   marker.
2. A short block needs its posting count. The prototype stores it as a LEB128 prefix
   inside the block, since F10's per-block flags carry only the code.
3. A position tail mid-stream moves the 128-position grid: a block's first position sits at
   (its first position − its run's first position) mod 128 in the block that level 0
   points at. Level 0 gives the offset, but a reader that jumps through level 1 does not
   know its run's first position. The prototype's decoder reads sequentially, so random
   access needs a field or a rule.
4. A flush's runs are small (18 at 100,000 documents, about 5,500 documents each), so short
   blocks are not bounded there: they hold 41.5% of postings. A copy merge's sources are
   whole segments and its short blocks number at most one per term per source, which is
   the case the design describes and the one C12 measures. Step 3 of this item (a merge by
   appending blocks) was not run.

## Recommendation

Plan differently. Encoding once at the drain buys 4-7% of one-thread indexing CPU at
100,000 documents (3.01× to 3.73× Tantivy, against 4.5×). It costs 10.3% more disk per
flushed segment and many more short LEB128 blocks for queries, so it does not justify bit
14 for flushes. What is left between the kept series and 4.5× is in phases this does not
touch: grouping (0.24 s), the runs' merge (0.14 s), the dictionary and skip data (about
0.3 s), and the front end (about 0.5 s). Bit 14 should be judged where the format design
puts it, on copy merges (C12: merge time against a re-encoding merge, bytes at sources of
1,000 to 100,000 documents), where pieces are large. Rules 1 and 3 above should be
settled there first.

Reproduce: `72d1928` holds the prototype, its checker, and its test. The harness is in the
worktree's untracked `.scratch/`: `build.sh` (base, branch, instrumented builds), `one.sh`
(one run with the phase timers), `tools/ab_index.py` (perf/inverter's, with CPU and a
`strata1` kind for `STRATA_ENCODE_ONCE=1`), `tools/ratio.py`, and `eo_vm.sh` (the VM leg).
