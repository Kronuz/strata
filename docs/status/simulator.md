# simulator
- Design: docs/design/simulator.md at (this commit); reviews: none yet
- Branch / worktree: design/simulator at /Users/kronuz/code/Kronuz/strata-wt/simulator
- State: designing (ready for preliminary review)
- Done: the D1 design draft (scale contract D1, engine D2-D7, tiers D8, checker D9, shrinking D10, home D11, reports D12; T1-T13; Q1-Q6; S1-S5)
- Doing: S1, the deterministic engine in sim/ (random.hh, clock.hh, engine.hh, tests), a self-contained CMake project in namespace kronuz::simulation
- Next: S3 the checker; S2 the network; S4 tier C0 (consensus Core groups on the engine); S5 tier B's abstract groups and the T10 memory measurement
- Blocked on: Q1 (code home: new Kronuz/simulation library, owner); Q2 (member view, owner with D2); Q3 (Peers send policy extraction, owner and Detent); Q4 (journal identity seam, owner and Detent); tier C journals wait for Q4
- Findings folded in: none yet
- Last verified: design only, no build (2026-10-17)
