# format-14-kv: the document layout's key-value read path (format 14)

- Task: design format 14's key-value layout for strata's core (the document layout), so the core
  can beat RocksDB as a key-value store: key-ordered value blocks, a shared decoded-block cache, a
  key-ordered scan, the buffer's sorted view, key-ordered merges. Design only to the first
  buildable step; no review ladder (PLAN, "Formats stay open until the first release"); the owner
  settles the shape from the summary, then a developer builds.
- Branch / worktree: `design/format-14-kv` from main `426cc59`, at
  `/Users/kronuz/code/Kronuz/strata-wt/format-14-kv`.
- Design: `docs/design/format-14-kv.md`.

## State

Designing: reading the inputs, then the micro-prototypes.

## Done

- (none yet)

## Next

1. Micro-prototypes under `bench/kv/proto/` (quiet-held, minutes each): block size and codec,
   the decoded-block cache, the buffer's sorted view, the current buffer's bytes per record.
2. The design, the first buildable step, the shape summary.

## Blocked on

Nothing.
