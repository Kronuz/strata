# write-buffer

- Design: `docs/design/write-buffer.md` on `origin/design/write-modes`, revision 2 (`ff9632c`):
  D2-D4, D6, D8, D10, D13-D14, "First buildable step", step 1 items 2 and 3. Reviews:
  `docs/reviews/write-buffer-design.md` there (WB-1 to WB-19, C1-C7). Prototype:
  `origin/perf/lanebuf-rebased` (`bench/kv/proto/lanebuf*.hh`).
- Branch / worktree: `phase4/write-buffer` from `phase4/shared-reader-views` at `428d415` (in code
  review; rebase onto main when it lands), at `/Users/kronuz/code/Kronuz/strata-wt/write-buffer`.
  Concurrency tier: a code review before it lands.
- State: in progress. Items 2 and 3 written; full ctest passes under ASan+UBSan (44 of 44, the
  new `write_buffer_test` included). Open: TSan, mutants (K4), the byte check (K2), memory (K6),
  CI (K7).
- Done criteria (the lead's; ticked only with evidence):
  - [ ] K1 Items 2 and 3 as D2-D4, D6, D8 and D10 state them for one writer; decision IDs in
    commit messages.
  - [ ] K2 The core's existing suites pass unchanged (full ctest); every format/byte the flush
    writes is identical to the base's on the same input (how checked, below).
  - [ ] K3 New unit tests per decision; a test per WB finding the item closes fails with its fix
    reverted.
  - [ ] K4 Mutants on every new ordering or ownership check, each killed by a named test.
  - [ ] K5 ASan+UBSan and TSan clean (Homebrew LLVM). ASan+UBSan (Debug -O1, Homebrew clang 23,
    macOS x86_64): ctest 44 of 44 (`.scratch/wb/ctest-asan.log`). TSan: not yet run.
  - [ ] K6 Memory per buffered record and per open index against the base's.
  - [ ] K7 CI green on the branch.
- Done: (commits below as they land)
- Next: K2 byte check, mutants, TSan, memory, push.
- Blocked on: see "For the designer and the architect".

## What items 2 and 3 build

- `include/strata/core/write_buffer.hh`: `wbuf::ref_t` (15-bit chunk, 17-bit 8-byte offset, D3);
  `version_map` (atomic 8-byte slots, hash beside reference, 7/8, release store after the record
  is complete, no removal, `reserve` before a transaction so restores never meet a grown table,
  D4); `stable_array` (the position index, entries never move, D9); `lane<Record>` (4 KiB
  doubling to 1 MiB chunks, fixed-stride records, a chunk directory fixed at creation: 8 inline
  entries and 12 blocks, 32,768 entries, WB-19; a borrow gate over its chunks' spans).
- `index.hh`: `record` (docid, version, position, prev, slot, tombstone bit and inverter ordinal,
  then the document) and `buffer` (one lane, key map, docid map built on first need (D3),
  position index, the active buffer's pending deletes on segments, its inverter in inverting
  layouts, scan runs) replace `buffered`, `borrowable_vector<buffered>`, `hash_slots` key and
  docid maps, and in-place dead flags. Stage 1 appends records and tombstones (key and docid),
  links `prev`, and restores slots on rejection; segment copies are pending deletes applied at
  freeze (D6); frozen buffers keep no live bits (WB-16). `freeze()` seals the active buffer and
  applies pending deletes (D8); `flush()` is freeze, then each frozen buffer oldest first, each
  installed by `install_flushed`, which clears in the new segments every copy a newer buffer's
  key or docid map holds (WB-16). Merges carry pending deletes to their output (`carry_pending`,
  WB-2); `install()` discards buffers and their pending deletes; `heal()` keeps ids.
- Item 3: the lease word is real in every build (`owner.hh`); `chore_scope` takes it only when
  free and never waits; a writer finding chores waits on the word, a second writer is still
  reported (SR-D20). `check_out()` returns a non-exclusive handle; `check_in()` runs the chores
  (finished merges published, released views freed) under a free lease, or marks them owed for
  the holder's next `apply()`.

## For the designer and the architect

- **A keyed segment document deleted by docid.** Stage 1 cannot name its key (a segment's
  dictionary maps key to ordinal only), so its tombstone carries the docid alone. Stage 1 and the
  lease holder's `get`/`scan`/`segments()` honour the pending delete through a set keyed by
  (segment, ordinal); a reader on another thread (item 4) would see the copy by key until the
  freeze. D6 assumes the key is known. Options: a reverse key lookup per segment, or freeze on
  such a delete, or accept until the freeze for docid-addressed deletes.
- **One `prev` per record.** A `replace_docid` that changes the key gives the new record two
  predecessors (its key's and its docid's); `prev` follows the key's. Item 4's reads by docid at r
  need the other, if any reader reads by docid.
- **Positions.** A record's position is its transaction's count in its buffer, from 1, until
  item 4 defines V and the buffer's base position.
- **Map-table retirement.** A grown table is freed at once: no thread but the lease holder probes
  a buffer before item 4's views carry buffers. D10's generation-stamped retirement comes with
  item 4.
- **The position index** stays 4 B per entry; D9's 12-byte prefixed index, the binary counter of
  runs, and the select-32 tail are item 4's scan. The scan here is the core's two-run view over
  record references (format-14-kv D3), now across frozen and active buffers.

## Log

- 2026-10-26: started from `428d415`. Read revision 1, then revision 2 (`ff9632c`, the lead's note:
  WB-16 to WB-19, D9's accepted structure, keyless docid shadowing in frozen buffers to test under
  K3). Built to revision 2.
- 2026-10-26: ASan+UBSan ctest 44 of 44 after: `segments()` returns a `segment_list` with the
  pending deletes applied in private bit copies (the lease holder's own view, as get and scan
  see them), and `carry_pending` on merge installs. Test changes beyond the shims:
  `shared_read_test` counts bit clones at commits (D6 moves them from the delete to the freeze);
  `bench/shared_reader/acquire.cc` copies the list after a refresh.
