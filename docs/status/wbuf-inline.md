> Status: CURRENT (2026-11-04) workstream perf/wbuf-inline, developer; in progress.

# wbuf-inline: the write buffer's records hold their key and stored bytes inline, for every layout

Task: finish explore/wbuf-bytes (strata:bec2c09, docs/perf/wbuf-bytes.md) as one shape: no switch,
no document_layout-only path.

## State

Building and proving. Branch perf/wbuf-inline off main 9e344d8.

## Done

(filled in as steps land)

## Next

1. Tests under Release, ASan+UBSan, TSan; mutants.
2. Bytes per record per layout (bufmem), A/B and A/A (bench/kv/ab.py), digests.
3. Linux through tools/vm/aws-linux.sh.
4. Notes: docs/perf/wbuf-inline.md, docs/mechanisms/write-buffer.md, exploration-log row.

## Blocked on

(none yet)

## Design vs code

(see docs/mechanisms/write-buffer.md when written)
