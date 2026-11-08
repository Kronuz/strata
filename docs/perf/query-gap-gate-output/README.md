# Query gap: the gate runs' `gate.py` tables, as the design and its review read them

> Status: HISTORY (2026-10-16, 2026-10-17) host copies of two `gate.py --md` outputs.

`docs/design/query-gap.md` (revision 1, branch `design/query-gap`) and `docs/reviews/query-gap-design.md`
rescore and cite two `gate.py --md` outputs that lived only in the lead's home on the laptop: `n1-gate5.md` is
`~/.scratch/n1/gate5.md` (2026-10-16 05:03 PDT, run n1 at strata:2e774db: strata against Lucene, Tantivy and
Xapian over `q3-queries.json`, five trials, pipe floor 67 µs), and `quick-n2.md` is `~/.scratch/quick-n2.md`
(2026-10-17 03:08 PDT, run n2: the quick tier at strata:db0a891 with PISA 0.8.2 (181 sampled queries and 40
terms), three trials, pipe floor 78 µs). Their committed summaries are prospect `docs/gates/phase-3-ratchets.md`
lines 98 to 172 (n1) and 286 to 322 (n2); the raw query files stay in `~/.scratch/n1/` (4 MB each) and on the
gate VM.
