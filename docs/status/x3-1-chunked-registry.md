# x3-1-chunked-registry

- Design: `docs/design/shared-reader.md` revision 4 (R4.1 to R4.8). Ruling: Xapiand
  `docs/reviews/phase7c-x2b-code.md`, "Architect ruling on the reader registry (X2b-B1, N1, N2)",
  item X3-1. Probe: Xapiand-wt/x2b `.scratch/arch-registry/registry_arms.cc`.
- Branch / worktree: `phase7c/x3-1-chunked-registry` from main `71de4a5`, at
  `~/code/Kronuz/strata-wt/x3-1`. Lock-free concurrency: one light architect check of revision 4.
- State: building.
- Done: revision 4 text; chunked registry in `hazard.hh`; `reader()` makes the index's own
  registry (`index_options::reader_records`); tests.
- Next: sanitizers, mutants, bench, preflight.
- Blocked on: nothing.
