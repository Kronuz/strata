# Format 13: store the key's ordinal?

The lead's pre-freeze item in format 13's ledger (`docs/status/format13.md` on
`phase4/format13-on-main`): in a renumbered segment a key lookup was described as key → docid
→ ordinal, the permuted docids section making docid → ordinal a binary search (about 22
dependent reads at 5M); the key dictionary is per segment, so it could store the ordinal.
Decide before the freeze with T5's key lookups (`keybench`) on a renumbered index.

## Procedure (2026-10-19, gate VM)

- Format 13 at `026ee10` (origin/phase4/format13-on-main), Release, clang 18, `-march=native`:
  `strata_engine` and `tools/keybench.cc`.
- One indexing run of the first 1,000,000 documents (`corpus-1m.json`, keys `Q` + the
  document's URL), merged twice from copies: reordered (`STRATA_REORDER=1`, four reordering
  threads, 175 s) and docid-ordered (27 s). Untimed builds at `nice -n 19`.
- `keybench DIR 5` over 100,000 keys drawn with seed 7, each as a hit and with `~` appended
  as a miss; one thread pinned, the two indexes interleaved, three repeats, under a light
  hold. `perf record` of one run on each for the split.

## Results

| ns per lookup | Hits (three repeats) | Misses |
|---|---|---|
| Renumbered (reordered merge) | 1,030.8, 1,070.7, 1,080.9 | 62.7–64.8 |
| Docid-ordered | 987.9, 946.0, 985.2 | 61.3–64.8 |

Hits cost 5–14% more on the renumbered index (about 85 ns at the means, 1,061 against 973).
Misses are the keys filter's (`may_hold`) and do not differ.

Where a hit's time goes (`perf`, self time): the key dictionary (`fst::map::floor_value` 22.7%,
`termdict` block search, open, header, and restarts about 31%), the filter 8.7%, and on the
renumbered index `docids::reader::docid` at 5.2%, which the docid-ordered index does not
call (its docid is arithmetic). No docid → ordinal search appears in either profile.

## Recommendation: store nothing more

Format 13 already stores the ordinal: its `keys` section's values are each key's ordinal
(`include/strata/keys.hh:24-25` at `026ee10`), and `lookup_key` goes key → ordinal → docid
(`index.hh:1562-1570`), the last step one read of the packed docids section
(`core().docid(ord)`); the binary search from docid to ordinal the item describes is not on
this path at `026ee10`. What renumbering still costs a hit is that one read,
about 85 ns (8%) at 1M, mostly a cache miss in the docids section. Storing the docid beside
the ordinal would remove it at about four bytes per key (about 20 MB at 5M), against the
whole index's 3.2 MB of headroom (D15), for 8% of a lookup that the dictionary dominates.
Recorded as not worth its bytes; no reserved place is needed.

Raw: `~/bench/results/qexp-keys/` (`keybench.txt`, `perf-*.data`, `phases.log`); SHA-256s:
`keys100k.txt` `f488fa16…7558`, `keybench.txt` `8ee4389e…b7d6`, `keybench` `d6ab4ca4…32f8`,
the two manifests `a4809927…e4ef` (renumbered) and `03633f4f…7f87` (docid-ordered).
