# strata review: cold tier step 2, item 6, the S3-FIFO rule and its ghost

- Date: 2026-11-03.
- Reviewed: item 6 at strata:b50e01e (`include/strata/core/ghost_set.hh`, `catalog.hh` `policy_pass()`, `evict()`,
  `pick()`, `demote_hot()`, `test/s3fifo_model.hh`), the ledger's item 6 section (`docs/status/cold-tier-step2.md:502-640`),
  `cold-tier.md` D6 at revision 7 (strata:5723f3d, branch `design/cold-tier`), my S8 criteria and model
  (`strata-wt/cold-tier/.scratch/arch-r4/s8_variants.cc`, `.out`), item 5's `pass()` at strata:e20219b
  (`strata-wt/cold-tier-step2-item5`, `catalog.hh:375-453`).
- Checked against: the owner's rules (one mechanism per need, every part pays, limits load-bearing, the many-indexes
  workload), S8.1 and S8.5, T4.
- Checked by: probes in `.scratch/review-ghost/`:
  - `rules.cc`: both rules (per-FIFO as `s8_variants`, total as D6), each with and without the ghost, and the ghost's
    size as a knob (`GHOST`, a fraction of `soft_open`). With `CHECK=1` every run at the default size is compared request
    by request with `s8::model` (per-FIFO) and `s8::model_total` (total, the oracle the catalog's lock-step test equals):
    `ref-equal` on all 16 runs (`check.out`).
  - Traces, 1M keys, capacity 110,000, 3M warm-up, 8M measured: the S8 cells (`z099`, `z099s`, `z12`, `z12s`), Zipf
    0.8 (`z08`), one-hit wonders (`ohw50`: half the requests to keys never seen again, the rest Zipf 0.99), a loop
    (`loop30`: 30% of requests cycle over 200,000 keys, past the cache), periodic scans (`scans`: 200,000 new keys
    every 1M requests), and fewer keys than the cap (`few`: 60,000 keys).
  - `sweep.out` (total rule, ghost 0.10, 0.25, 0.50 of `soft_open`), `half.out` (ghost 0.5, both rules).
  - `ghost_bench.cc` (`ghost_bench.sh`, the real `ghost_set.hh` with `hash_slots` copied verbatim into `shim/`):
    take and add at 99,000 hashes, under a `quiet strict` hold (`timing.out`).
  - Not run: the catalog's S8 harness (no build dir in the worktree; the catalog equals `model_total` to 0.0001 per the
    ledger's lock-step test and S8 cells, so the rules were compared on the lists), sanitizers (the ruling changes no
    code yet), Linux and FreeBSD, T4.

## Verdict

**APPROVE WITH CONDITIONS: the total rule stays; the ghost stays and pays, resized to half of `soft_open`; S8.1's
references become the design's own rule and S8.5's "ghost ignored" mutant dies on a loop cell.**

Neither of the two options holds as posed. The per-FIFO model's 3.1 points are not the ghost's worth (G1), and under
the total rule the ghost is not dead weight on the traces it exists for (G3).

What holds, and reproduces:

- The catalog's rule is D6's and the paper's (Algorithm 1 evicts only while the cache is full, the small FIFO's head
  while it holds its tenth). `rules.cc`'s total rule equals `s8::model_total` request by request, which the ledger's
  lock-step test equals to the catalog (`check.out`).
- The ghost costs nothing on a pin: the policy mutex and the ghost are taken on a miss and by the pass, never on a hit (`catalog.hh:19-24`;
  `policy_enqueue`, `catalog.hh:691-699`, runs once per open).

What blocks: L1 to L4 below, all small.

## Findings

**G1. My S8.5 number measured unused capacity, not the ghost.** Without the ghost, the per-FIFO model's main FIFO
fills only from promotions and evicts only when full, so it never fills; the cache runs short of its cap while it
closes. Peak resident (`check.out`), per-FIFO without the ghost:

| Trace | Peak resident | Hit ratio, with ghost / without |
|---|---|---|
| z099 | 99,496 | 0.8160 / 0.7846 |
| z12 | 58,165 | 0.9583 / 0.9409 |
| ohw50 | 37,700 | 0.3880 / 0.3485 |
| loop30 | 53,852 | 0.5651 / 0.5126 |

Every total-rule run holds 110,000. The 3.1 and 1.7 points are the per-FIFO rule's dependence on the ghost to fill
the main FIFO. S8.5's expected loss was mine and was wrong; the developer's survivor (ledger Findings 1) is correct.

**G2. The per-FIFO rule never wins, and it closes below the cap.** With the ghost at its current size:

| Trace | Per-FIFO | Total | Total less per-FIFO |
|---|---|---|---|
| z099 | 0.8160 | 0.8160 | 0.0000 |
| z099s | 0.8159 | 0.8158 | -0.0001 |
| z12 | 0.9583 | 0.9593 | +0.0010 |
| z12s | 0.9577 | 0.9587 | +0.0010 |
| z08 | 0.5811 | 0.5811 | 0.0000 |
| ohw50 | 0.3880 | 0.3948 | +0.0068 |
| loop30 | 0.5651 | 0.5682 | +0.0031 |
| scans | 0.6738 | 0.6738 | 0.0000 |

At ghost 0.5 the gap widens: ohw50 0.3795 per-FIFO, 0.3908 total, per-FIFO's peak 76,475 (`half.out`). Per-FIFO caps
the small FIFO at a tenth whether or not the cap is reached, so on a one-hit-wonder load it closes indexes while
34,000 slots stand empty, and each close is a later open at 0.4 to 1.1 ms on macOS (D6, Limits). For a host with
millions of indexes the trigger that pays is the one D6 has: close only when a soft cap asks. Ruling: the total rule
stays.

**G3. Under the total rule the ghost pays on its target loads, but at the main FIFO's size it loses on flat ones.**
Hit ratio over all 8M, total rule, by ghost size (`check.out`, `sweep.out`, `half.out`):

| Trace | No ghost | 0.10 | 0.25 | 0.50 | 0.90 (now) |
|---|---|---|---|---|---|
| z099 | 0.8162 | 0.8189 | 0.8199 | 0.8189 | 0.8160 |
| z12 | 0.9581 | 0.9585 | 0.9589 | 0.9593 | 0.9593 |
| z08 | 0.5880 | 0.5926 | 0.5922 | 0.5882 | 0.5811 |
| ohw50 | 0.3840 | 0.3856 | 0.3878 | 0.3908 | 0.3948 |
| loop30 | 0.5550 | 0.5575 | 0.5607 | 0.5648 | 0.5682 |
| scans | 0.6713 | 0.6736 | 0.6746 | 0.6746 | 0.6738 |

At 0.9 the ghost gains 1.08 points on one-hit wonders and 1.32 on the loop, but loses 0.69 at Zipf 0.8 and 0.02 at
0.99: a part that does not pay everywhere. At 0.5 it never loses (+0.27, +0.12, +0.02, +0.68, +0.98, +0.33; the S8
scan cells z099s 0.8188 against 0.8156, z12s 0.9586 against 0.9575). 0.25 has a better worst case (+0.08) but keeps
less than half of the loop and one-hit-wonder gain, which is what the ghost is for, and its mutant margin is thin
(G4). The mechanism the size names: a key closed from the small FIFO skips probation if it returns within that many
small-FIFO closes. At 0.9 that admits, under flat popularity, keys whose return is too far to earn a main slot, and
each displaces a main entry worth more. Ruling: the ghost stays, at half of `soft_open`.

**G4. S8.5's mutant can die by the hit ratio once the references are the design's rule.** S8.1 compares the catalog
with `s8_variants` (per-FIFO), so its 0.5-point margin has to absorb a rule difference (ledger Findings 2: +0.44 at
theta 1.2, 0.06 short of the margin). Against `model_total` the catalog's gap is 0.0001 in every cell. With the ghost
at 0.5, "ghost ignored" (total rule, `half.out` against `check.out`) loses 0.27 at z099 (first 1M after warm-up 0.59),
0.98 on loop30 over all 8M (first 1M 0.84) and 0.68 on ohw50. The loop cell kills it with room at a 0.2-point margin;
the Zipf cells alone do not.

**G5. The ghost's cost, and what it buys.** `ghost_bench` (macOS x86_64, `-O2`, 20M ops, 3 rounds): take 4.6 to
5.0 ns (one per miss), add 152 to 157 ns (one per small-FIFO close, which includes a take and an eviction of the
oldest). At z099 that is about 18% of requests times 160 ns, near 29 ns per request; the ghost's +0.27 points avoid
0.0027 opens per request, at least 1.1 µs at 0.4 ms each, about 37 times its cost. The list model's ns per request
(`timing.out`) varies with run order (738 and 339 ns for the same trace and ghost) and is not the catalog's cost; it is
not used here. Memory: a node is 24 B (hash 8, three `uint32_t`, aligned), plus an 8-byte `hash_slots` slot at most
7/8 full; at 99,000 hashes that is 131,072 nodes and 131,072 slots, 4 MiB per catalog; at 55,000 (half of 110,000),
2 MiB, about 18 B per open index. `ghost_set.hh:3-4` says "16 B each": wrong.

**G6. The `evict()` seam is the right cut, with four corrections at the merge.** One chooser that takes a stop
condition from the caller is right: item 5's pass keeps the caps, idle closes, bound closes and retries, and the
S3-FIFO order replaces its sorted candidate loop. But:

1. `before_close(idx)` runs before `begin_close` can refuse (`catalog.hh:767-778`), so item 5's `less(over_desc, ...)`
   placed there books relief for a close that did not happen, and the pass stops short. Count relief in flight in the
   catalog instead, as `pending_maps_` already does for demotions (`catalog.hh:315`, `:980`): descriptors and vnodes
   of closes begun, released when the close ends; `over()` reads counts less pending, and `before_close` goes.
2. Two measures of "open": item 6's queued count (`queued_over_soft_open`, `catalog.hh:718-721`) and item 5's
   `live_ - closing_` (item 5 `catalog.hh:396`). They differ by idle, bound and direct closes still queued until they
   end. Keep `live_ - closing_`, kept by the state transitions themselves; delete `queued_over_soft_open`.
3. Two hot-demotion paths: item 5's pass loop (idle 30 s and every cap's share) and item 6's `demote_hot()` with its
   own 256-slot scan. Keep item 5's and give it `demote_hot`'s subtraction of demotions in flight, which item 5's
   `over_hot` lacks (item 5 `catalog.hh:388-393` reads `counts_` at pass start); delete `demote_hot` and `scan`.
4. One pass guard: item 5's coalesced chore or `policy_running_`, not both.

Item 5's idle and bound closes do not enter the ghost (`to_ghost` only from `evict`'s small FIFO): right, keep it.

**G7. Note: the ghost's 2^22 bound (`catalog.hh:274`) names no mechanism in the line.** If it is the catalog's slot
limit (ledger `:257`, 4M slots), say so; with the ghost at half of `soft_open` it is redundant and can go.

## Blocking items / Required changes

- **L1.** The ghost holds half of `soft_open`'s hashes (`catalog.hh:272-274`); `docs/mechanisms/catalog.md:64` and
  `ghost_set.hh:3-4` say so, with the bytes per hash corrected (G5). `s3fifo_model.hh`'s `model_total` takes the ghost's
  size; the lock-step test runs at it.
- **L2.** S8.1's references are `model_total` at the catalog's parameters (ghost 0.5): z099 0.8189 and 0.8160 (all 8M,
  first 1M), z099s 0.8188 and 0.8153, z12 0.9593 and 0.9556, z12s 0.9586 and 0.9539 (`sweep.out`, `half.out`), margin
  0.2 point. `s8_variants` is reported beside it, ungated.
- **L3.** S8.1 gains the loop cell (30% of requests cycling over 200,000 keys, `rules.cc` `loop30`), reference 0.5648 and
  0.5535; S8.5's "ghost ignored" dies there (expected 0.98 down over all 8M), run at the scale the gated cells run.
  The other three S8.5 mutants are rerun against the new references.
- **L4.** The ledger's Differences 1 and Findings 1 and 2 are restated against this ruling.

## Conditions

- **C1.** D6 at revision 8 (the designer's edit, not the developer's): nothing closes below a soft cap (the total
  rule, with G1 and G2's reason); the ghost holds half of `soft_open` and why (G3's mechanism); the model numbers become
  `model_total`'s (0.819 and 0.959; 0.815 and 0.954 over the first 1M after the scan); T4's reference model is
  `model_total` at the row's open count.
- **C2.** The merge with item 5 applies G6's four corrections; its review checks them.
- **C3.** T4 reports the ghost's hits and adds per second beside closes per second, so the size is checked on real
  opens, not only on lists.

- Not run: the catalog's S8 harness, any per-FIFO catalog build, sanitizers, Linux and FreeBSD, T4. Probes kept in
  `.scratch/review-ghost/` (`rules.cc`, `ghost_bench.cc`, `ghost_bench.sh`, `shim/`, `check.out`, `sweep.out`,
  `half.out`, `timing.out`).
