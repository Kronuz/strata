# write-buffer

- Design: `docs/design/write-buffer.md` on `origin/design/write-modes`, revision 2 (`ff9632c`):
  D2-D4, D6, D8, D10, D13-D14, "First buildable step", step 1 items 2 and 3. Reviews:
  `docs/reviews/write-buffer-design.md` there (WB-1 to WB-19, C1-C7). Prototype:
  `origin/perf/lanebuf-rebased` (`bench/kv/proto/lanebuf*.hh`).
- Branch / worktree: `phase4/write-buffer`, rebased onto `origin/main` at `fed0c85` (shared-reader
  views landed as `23e78bb`), at `/Users/kronuz/code/Kronuz/strata-wt/write-buffer`.
  Concurrency tier: a code review before it lands.
- State: folding in the code review (`docs/reviews/write-buffer-step1-code.md`, APPROVE WITH
  CONDITIONS, CR1 to CR9); see "Review fix round". Open items for the designer are under "For the
  designer and the architect".
- Done criteria (the lead's; ticked only with evidence):
  - [x] K1 Items 2 and 3 as D2-D4, D6, D8 and D10 state them for one writer (what they build,
    below; D9's index, the binary counter of runs and select-32 tail, and D10's retirement are item
    4's, as noted for the designer). The one build commit is titled WIP and names no decision IDs in
    its message; the IDs are in the code comments and this ledger.
  - [x] K2 The full ctest passes unchanged apart from the shims (ASan+UBSan 44 of 44, TSan 44 of
    44, on the rebased head). Bytes: `.scratch/wb/bytes.cc` (committed nowhere; deterministic
    workload, seed 7, 12 rounds of 400 operations on a faultfs index: replace 5/12, delete by key
    2/12, conflicting add 1/12, keyless add 1/12, delete_docid/replace_docid on keyless 1/12,
    conditional replace 1/12, set_meta 1/12; key space 900; values up to 200 B; refresh every 3rd
    round, commit every 4th, `force_merge(2)` at round 6, final commit; at 1 and 3 flush
    threads; FNV-1a over every file name and content) built Release (clang 23) against this head
    and `origin/main` (`fed0c85`, the base): identical on both, `threads 1: files 9, digest
    48c6d41502f08fc0, bytes 4a39e97a9c882d22`; `threads 3: files 15, digest 48c6d41502f08fc0,
    bytes ca8b4359e9bad8d7`; a second run of this branch repeats them.
  - [x] K3 New unit tests per decision (`write_buffer_test`); each WB finding's test is the killer
    of a mutant of its fix (K4), so it fails with the fix reverted.
  - [x] K4 Mutants on every new ordering or ownership check, each killed by a named test.
    `.scratch/wb/mutant.py` (in place, rebuilt, restored; logs `.scratch/wb/out/`),
    `write_buffer_test`: 14 of 14 killed. ASan build (`.scratch/wb/mutants.log`): `restore_skipped`
    and `no_prescan` by `rejection_restores`; `no_pending` and `freeze_no_apply` by
    `pending_until_freeze`; `no_carry` by `merge_carries_pending` (the freeze fences);
    `no_key_clear` by `flush_install_clears`; `no_docid_clear` and `no_old_key_tombstone` by
    `keyless_docid_shadowing`; `no_tombstone` and `tombstone_ignored` by `tombstones_and_prev`;
    `checkin_as_writer` and `owed_not_run` by `checkout_and_check_in`; `writer_ignores_chores` by
    `writer_waits_for_chores`. `slot_relaxed` (WB-9, release store made relaxed) survives natively
    on x86_64 (T10) and is killed under TSan by `slot_after_record`: "WARNING: ThreadSanitizer:
    data race" in its writer thread, rc -6 (`.scratch/wb/out/m_slot_relaxed_build-tsan.log`); the
    file is restored.
  - [x] K5 ASan+UBSan (Debug -O1, Homebrew clang 23, macOS x86_64): ctest 44 of 44
    (`.scratch/wb/ctest-asan.log`, 277 s). TSan (same compiler, `-fsanitize=thread -O1`): ctest 44
    of 44, no ThreadSanitizer report in the log (`.scratch/wb/ctest-tsan.log`, 1028 s wall at -j4).
    Both on the head rebased onto `fed0c85`.
  - [x] K6 Memory against the base's (`origin/main` `fed0c85`), `kv_proto_bufmem`'s method
    (`bench/kv/proto/bufmem.cc` from `origin/perf/lanebuf-rebased`: RSS at 100,000 and 300,000
    buffered records, document layout, nothing flushed; the difference over 200,000), Release,
    macOS x86_64, three interleaved trials, resident bytes per buffered record: 100 B values, base
    347.5 / 389.5 / 368.4, this branch 223.4 / 220.5 / 228.6; 1000 B values, base 1281.3 / 1302.0 /
    1301.7, this branch 1140.7 / 1132.2 / 1140.2. Per open index (`.scratch/wb/mem/proto/idxmem.cc`:
    RSS growth over 1000 indexes in separate directories, one `PosixIO` each): empty, base 2818 /
    2769 / 2736 B, this branch 2802 / 2785 / 2699 B; with 10 buffered 100 B documents each, base
    6439 / 6378 / 6185 B, this branch 9368 / 9368 / 9388 B, about 3.0 KB more, which
    matches one 4 KiB first lane chunk against the base's smaller initial vectors (not isolated).
    Fuller table: `.scratch/wb/mem/mem.out`. Not a timing claim.
  - [ ] K7 CI green on the branch: the rebased code head `a4b83e7` completed green; the ledger-only
    head after it is running, not waited on.
- Done: `74f122c` items 2 and 3 (WIP commit); `c346f10` ASan ctest green, bench fix; `a4b83e7`
  mutants ledger (these three rebased onto `fed0c85`); the verification commit after them: TSan,
  the TSan mutant, the byte check, memory.
- Next: item 4 (views carry buffers) once the code review and the designer's notes close; an
  inverting layout's record path (key-only records, ordinals) is compiled here but tested only in
  prospect.
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
- 2026-10-26: mutants on the ASan build, 13 of 14 killed; `slot_relaxed` left for TSan. Stopped
  here at the lead's token checkpoint; the build dir `build-asan` stays for the next agent.
- 2026-10-26: rebased onto `origin/main` (`fed0c85`); one conflict in `index.hh`'s apply tail
  (kept the views branch's `changed()` on a moved position and the chore run after it). ASan and
  TSan ctest 44 of 44; slot_relaxed killed under TSan; byte check identical; memory measured.
  State set to in review.

## Review fix round (code review CR1 to CR9)

Each fix has a test in `write_buffer_test` that fails with the fix reverted (ASan build).

- CR1 `45bfbaa`: the rollback truncates the lane to its mark (`lane::tell`/`truncate`), so a
  rejected transaction's records and document copies are freed. `rejection_frees`: reverted, 4
  checks fail; destructor call removed, 3. p4: +0 MiB after 4000 rejections, both rows.
- CR2 `7b8ebe5`: chores never throw. A failed chore fences the index; the next writer call
  reports `index_fenced` through `live()`. `check_in()` is noexcept on every path (checkout's end,
  explicit, the writer's re-check). `checkin_fences` (p5): reverted, aborts (rc 134).
- CR4 `1999bdc`: Dekker pairing. `check_in` stores the flag seq_cst and retries the chore CAS
  (seq_cst); every public writer call holds a `writer_scope` whose end hands off (seq_cst store)
  and re-reads the flag, running `check_in` when set. `owed_chores_run` (40,000 trials against
  apply, refresh, wait_merges): 0 owed; reverted 4967 of 20,000; retry removed 362-595 of 40,000;
  re-read removed 4955. p3: 0 of 100,000 at spans 3000, 10,000, 30,000.
- CR3: `check_in`'s contract narrowed (off the owner's thread only while no thread reads outside
  a writer call); debug builds report a lease-less read that overlaps another thread's chores
  (`write_lease::check_read` in get, find_key, has_docid, scan, segments, buffered_count).
  `writer_waits_for_chores` checks 2 reports: reverted, fails. No lease on `get`.
  **Item 4's done criteria gain, beside C9:** reads off the lease holder's thread go through
  views, and `check_in` off the owner's thread is then unconditional (the debug check stays as
  its proof).

