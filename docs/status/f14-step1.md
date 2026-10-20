# f14-step1: format 14's step 1 (key-ordered document segments in 4 KiB codec-tagged blocks)

- Task: build step 1 of `docs/design/format-14-kv.md` ("First buildable step") under the lead's
  decisions in `docs/status/format-14-kv.md` (split layout; LZ4 by default; zstd 1 on merged
  output as an opt-in per-index disk preset; T7's disk clause against RocksDB). Not in this step:
  the block cache (D2), the key-ordered scan API (step 2), the buffer rework (step 4).
- Branch / worktree: `phase4/f14-step1` from main `4d36aba`, at
  `/Users/kronuz/code/Kronuz/strata-wt/f14-step1`. compressors: `block-codecs` from main
  `ed590c8`, at `/Users/kronuz/code/Kronuz/compressors-wt/block-codecs`.
- Lands on proof (PLAN, "Formats stay open until the first release"): the lead lands it, after a
  decoder-safety review of the new block decoder. Not merged by the developer.

## Done criteria (the lead's, set from the design's K1 to K7; contributors tick, never edit)

- [ ] K1 (T1): fixtures re-pinned at the new pre-freeze version; format-13 segments refused as
  `unsupported_segment`; `core_kv_test`'s digest identical at main and at the branch.
- [ ] K2 (T3): `check()` refuses a segment out of key order; a docid-order mutant fails the test.
- [ ] K3: a merged document-layout segment is key-ordered with every live record once, its digest
  equal to its inputs'; the merge takes the ascending path (no records scratch file) and builds
  no key map (a test on its charged memory).
- [ ] K4: gets allocate nothing per call besides the returned string, and nothing with
  `get(key, f)` (an allocation-counting test).
- [ ] K5 (T2): the block decoder's fuzz target and corpus, replayed in CI; ASan and UBSan clean;
  each fix's test fails with its fix reverted.
- [ ] K6: CI green on Linux, macOS, and FreeBSD (`cpp-libraries/tools/preflight.sh` first);
  compressors' change additive, its own CI green, Detent's suite replayed against it.
- [ ] K7: a branch A/B with the KV harness (`kv_strata` on the core API) against main at the quick
  tier (100k records), point gets and fills, run-level with an A/A first, under quiet holds;
  every row reported. Functional evidence only; the gate rows are the lead's, on main.
- [ ] Prospect at the branch's strata commit: its search layout (64 KiB blocks) passes its suite.

## State

Started.

## Done

## Next

1. compressors: one-shot block codecs into the caller's buffer, per-thread contexts,
   `COMPRESSORS_LZ4_PINNED`.
2. strata: format, writer, reader, flush, merge, get, check, fuzz target; tests per K1 to K5.
3. CI, prospect, the A/B.

## Blocked on

Nothing.
