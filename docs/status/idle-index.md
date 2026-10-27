# idle-index

- Branch / worktree: `perf/idle-index` at `~/code/Kronuz/strata-wt/idle-index`, from origin/main `98ce743`.
- Subject: landscape proposal D1: the cost of one idle open index and the distance to millions per host.
  Record: `docs/perf/idle-index.md`. Harness: `bench/idle_index/idle_index.cc`.
- State: measured; report delivered; no code change to the engine.

## Done

- Harness `idle_index` (build/open, bare and shared, idle window, query, close) and `idle_index_count`
  (allocation sites).
- macOS: 0/1/10/100/1,000 documents at 1k, 10k, 100k open; 126,507 (vnode ceiling) and 252,492 (0 docs).
- Linux VM (port 18772): same matrix with cold and warm opens; 300,000 open; the stock
  `vm.max_map_count` limit (65,491); `strace -c` of an open. VM directory removed afterwards.
- Heap breakdown per idle index; limits table; top three cuts; comparison with the field.

## Next

1. Cut 1 (cold segments) on main, with an A/B; cuts 2 and 3 after.
2. Per-index cost with prospect's search layout (hybrid).
3. Decide the closed-handle tier or the shared partition lock (vnode/descriptor ceilings).
4. 1M on Linux with raised sysctls and 32 GiB.

## Blocked on

- Nothing. The closed-handle tier is a host-policy decision parked by the lead (small-indexes F3).
