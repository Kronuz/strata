# Code review: write buffer step 1, items 2 and 3 (`phase4/write-buffer` 9a9957a)

Reviewer: code reviewer, concurrency tier. Code at `a4b83e7` (the ledger-only commits after it change no
code). Base `fed0c85` (`origin/main` has moved to `326024f` by two log-only commits; no code
conflict). Design `design/write-modes` `8cdbe61` revision 4 (items 2 and 3 implement D2 to D4, D6,
D8, D10 for one writer). Worktree `/Users/kronuz/code/Kronuz/strata-wt/review-wb1`; every probe,
mutant and log is under its `.scratch/` (listed at the end).

## Verdict: APPROVE WITH CONDITIONS

The buffer itself (lane, 32-bit references, fixed chunk directory, version map, tombstones, `prev`,
pending deletes, freeze, flush install, merge carry) is sound at one writer: a seeded differential
workload that the contributor's byte harness does not cover gives logical traces identical to the
base's in 21 runs (see "What held"), and I found no record read after publication, no lifetime
fault and no sanitizer report in the code paths the tests reach. What blocks landing is the new
surface around it: a memory leak that the budget cannot see (CR1), a crash in `~checkout` (CR2),
a data race the API invites (CR3), a lost chore wake-up (CR4), and a behaviour change in the
foreground merges (CR5). Each has a small fix. Conditions: CR1 to CR5 fixed with a test each, then
a scoped re-check (my probes p3, p4, p5, p9 and p1 are the re-check).

## Blocking findings

### CR1. Rejected transactions leave their records and document copies in the lane, uncounted and unbounded

- Where: `index.hh:2775-2799` (`rollback` rewinds `order`, `docs` and `bytes` but not the lane),
  `write_buffer.hh:191-208` (`lane::append`, nothing is ever removed), `index.hh:1710-1735`
  (`buffered_bytes()`/`needs_flush()` count `b.bytes`, which the rewind restores),
  `index.hh:3093-3095` (`freeze()` returns on an empty `order`, so a buffer holding only rejected
  records can never be frozen or flushed).
- What: every op of a rejected transaction that already ran has appended a `record` whose
  `document` owns a heap copy of the key and value (`push_record`, `index.hh:2833-2840`). The rewind
  hides it; the memory stays until the buffer is destroyed. The base popped them.
- Probe `.scratch/rv/p4_reject_growth.cc`, loop of `{replace k_i = 64 KiB, add <existing key>}`
  (version_conflict every time), `buffer_share` 16 MiB, ASan, quarantine off:

  | tree | after 4000 rejected transactions |
  |---|---|
  | base | RSS +0 MiB, `buffered_count` 1 |
  | branch, buffer holds one accepted record | RSS +305 MiB, `buffered_count` 1 |
  | branch, buffer empty (the conflicting key lives in a segment) | RSS +305 MiB, `buffered_count` 0 |

  Nothing in the node budget, `needs_flush()` or `buffered_count()` moves. The second row is the
  worse one: with no accepted record the buffer cannot freeze at all (`freeze()` and
  `needs_flush()` both return early on zero records). Optimistic-concurrency retries with large
  values are an ordinary workload, not an attack. Mutant `.scratch/mut/m_keepbytes` (do not rewind
  `bytes`) does not close the second row (305 MiB), so accounting alone is not the fix.
- The design allows the leak for readers (D4: "its records stay in the lane ... T3 reports the
  bytes") and says nothing about a bound; "limits are load-bearing" applies. Item 4's readers are
  the only reason to keep them.
- Fix (items 2 and 3 have no reader off the lease holder): destroy the rejected records at the
  rollback by truncating the lane to its mark (the tail is exactly the transaction's records; add
  `lane::truncate(mark)` that runs the destructors and returns the chunk position), restoring the
  base's behaviour. If the designer wants the records kept for item 4's readers, then count their
  bytes in `buffered_bytes()` and let `needs_flush()`/`freeze()` drop a buffer with no accepted
  record. Test: the p4 loop with a small `buffer_share`, asserting `buffered_bytes()` and the process
  RSS (or a counting allocator, p6) stay bounded; it must fail on this head.

### CR2. `~checkout` throws from a destructor: `std::terminate` on a fenced merge

- Where: `index.hh:690-693` (`~checkout() { if (ix_) ix_->check_in(); }`, implicitly noexcept),
  `index.hh:706-713` and `1923-1931` (`check_in` -> `run_chores` -> `publish_merges` -> `fence()`
  throws `index_fenced`).
- Probe `.scratch/rv/p5_checkin_throws.cc`: four segments, `fs.add_fault({create, 1, EIO})`,
  `maybe_merge()`, then `{ auto c = ix.check_out(); }`. Result: `libc++abi: terminating due to
  uncaught exception of type strata::index_fenced: strata: durability failure: faultfs create:
  Input/output error`. A request's last act would kill the process on any failed background merge.
- Fix: the destructor catches (the index is fenced; the next call reports it through `live()`), and
  `check_in()` stays callable explicitly for a caller that wants the exception. Test: p5 as a unit
  test, asserting the index is fenced afterwards and the process survives.

### CR3. `check_in()` is documented "callable from any thread", but the owner's reads take no lease: TSan data race

- Where: comment at `index.hh:703-705`; reads without `lease_scope`: `get` (`1308-1337`), `scan`,
  `has_docid`, `find_key`, `segments()`, `digest()`, `buffered_count()`. Chores mutate the state
  those read: `install_merge` (`2388-2460`) erases and inserts `segs_` and calls `carry_pending`;
  `free_views`/`retain` touch the reclaim lists.
- Probe `.scratch/rv/p1_chore_vs_get.cc` (TSan build): the owner thread loops `apply`, `refresh`,
  `maybe_merge` and `get`; another thread loops `check_out()`. 44 reports, e.g. write by the
  check-in thread in `install_merge` against the owner's read in `get_segments` (`p1_tsan.out`).
- Not a lease bug in the chore itself (`chore_scope` takes the word with one CAS from free and
  never runs beside a holder), but D2's promise that a client holds the index "never exclusively"
  while others read is not true until item 4 moves reads onto views. Today the only reads are the
  lease-less owner ones.
- Fix for this branch (no host calls `check_out` yet): narrow the contract in the comment and the
  ledger ("check-in off the owner's thread is safe only while no thread reads the index outside a
  writer call; item 4 removes the condition") and add it to item 4's done criteria beside C9. Do not
  put a lease on `get`: a CAS plus a store per get is a cost on the path the ratchet measures.

### CR4. A check-in's owed chores can be lost (lost wake-up)

- Where: `index.hh:706-713` (`check_in` stores `chores_owed_` after its CAS failed) against
  `index.hh:681` (the holder's one check, at the end of `apply`, before `lease_scope` releases at
  the closing brace). A holder past the check, or a holder in `refresh`/`commit`/`merge` (none
  tests the flag), releases the lease with the flag set and nobody holds it.
- Probe `.scratch/rv/p3_lost_chore.cc` (one `apply` against one `check_out`, the second delayed by
  a swept spin): 187 of 20,000 trials end with `chores_owed()` true and the lease free (span 3000),
  53 (span 10,000), 11 (span 30,000); 0 when the delay is tiny, which is how the contributor's test
  stays green. D2's writerless group is never lost, because the caller links behind the leader.
- Effect: a finished merge stays unpublished and its sources unreclaimed until the next call on an
  index that may have no further callers.
- Fix (Dekker pairing): the flag store and the retry CAS in `check_in` are seq_cst, and the
  holder, after releasing, re-reads the flag (seq_cst) and runs `check_in` itself if set. Put the
  re-read in one place (a wrapper at the end of the public lease-taking methods, or in
  `lease_scope`'s destructor via a callback). Test: p3 asserts zero stuck trials over 100,000 (the
  failing rate is about 1 per 100 on this host).

### CR5. `force_merge`, `force_merge_deletes` and `merge` with deletes pending: dead documents survive the merge

- Where: `index.hh:732-760` (the foreground merges do not flush or settle the buffer),
  `index.hh:2023-2060` (`schedule` snapshots `s->deleted`, which lacks the pending deletes),
  `index.hh:2466-2484` (`carry_pending` moves them to the output).
- Probe `.scratch/rv/p9_force_merge.cc` (two segments of 50 documents, 90 of 100 deleted, then
  `force_merge(1)`):

  | tree | documents in the merged segment | dead | bytes on the filesystem |
  |---|---|---|---|
  | base | 10 | 0 | 2578 |
  | branch | 100 | 90 | 3426 |

  `force_merge`'s own comment says "until at most max_segments remain and none holds deletes". The
  differential workload shows the same as a file-bytes difference at the first `force_merge` with a
  non-empty buffer (`p2_diff`, seed 1, `used` 8118 against 7426). A bulk delete followed by a force
  merge now rewrites every dead document, reserves storage for them, and leaves the segment with
  90% dead; the policy's `force_merge_deletes_pct_allowed` also misreads the segment, since `dead`
  excludes the pending bits. The contributor's K2 byte check avoids it: its `force_merge(2)` runs
  right after a `refresh`.
- D6 carries pending deletes through installs (WB-2); it does not say the foreground merges may see
  them as live.
- Fix: `merge`, `force_merge` and `force_merge_deletes` (the calls whose contract is "no deletes
  left") start with the freeze's application of the pending deletes (the first half of `freeze()`,
  `index.hh:3097-3108`, or a `flush()`). Background `maybe_merge` after `refresh` already sees them
  applied. Test: p9 as a unit test (`dead == 0` and `doc_count == 10` after the merge). The designer
  should record the ruling in D6, since item 4's view positions treat segment bits as positioned
  state.

## Memory ruling (many-indexes workload)

Measured exactly with a counting `operator new` (`.scratch/rv/p6_mem.cc`, the heap bytes the
index allocates after `create`, then N buffered 100-byte keyed documents, one transaction each):

| buffered documents | base | branch | branch with a 512 B first chunk (mutant `m_chunk512`) |
|---|---|---|---|
| 1 | +968 B | +5464 B | +1880 B |
| 10 | +3056 B | +6400 B | +3840 B |
| 100 | +22992 B | +25368 B | +28952 B |

The ledger's +3 KB at 10 documents is confirmed (+3344 B), and the cause is exact: one 4096 B first
chunk (80 B stride, so 51 records of room, 10 used) plus the 536 B `buffer` object (the lane's
inline directory); at one document the branch is 5.6 times the base. At the owner's workload that
is about 3.3 GB per million open indexes with ten buffered documents (4.5 GB with one). The
memory is returned at the flush (the buffer is destroyed), so it is a cost of indexes with
buffered writes, not of idle ones (the empty-index row is at parity in the ledger).

Ruling: it does not block landing, because it is bounded and returns at the flush, but it is a
condition (CR6): the memory ratchet is a stated goal and the fix is one constant. Start the lane at
a small first chunk and double (the directory arithmetic does not depend on the size: 32,768 chunk
entries stay, only the "at least 128 MiB at the smallest chunk" floor in the comment at
`write_buffer.hh:26` changes). 512 B costs 3.5 KB more than the present 4 KiB at 100 documents
(12 chunks to reach steady state, mutant column above); 1 KiB (12 records) or `16 * stride` is the
better trade and the contributor should pick it by this table plus a 1000-document row.
`write_buffer_test` passes unchanged with the 512 B mutant (ASan, `wbt_m512.out`). The 536 B
`buffer` object is the next piece (the eight inline `chunk` entries are 192 B): shrink to four
inline entries if the table still shows it.

## Conditions (not blocking, to be met in the same fix round or recorded)

- **CR6.** The first-chunk size above.
- **CR7. WB-23 ruling.** Items 2 and 3 need not change for it; item 4 owns it, as C9 already says.
  Reasoning at file:line: no thread other than the lease holder probes a buffer today. The flush's
  pool threads read only records through `lane.pin()` (`index.hh:3259-3270`); `newest()`,
  `find_docid_in` and every `build_docids` run on the owner's thread before the streams start
  (`index.hh:3243-3250`). So `docids_built` (plain `mutable bool`, `index.hh:1830`) and the
  building `find_docid_in` (`2678-2689`) are not a race now. Item 4 must: (1) split `find_docid_in`
  into a const probe that loads the table pointer with acquire and never builds, and the lease
  holder's building call; (2) derive "built" from the table pointer (or an atomic), not the bool;
  (3) retire a grown table (`version_map::grow`, `write_buffer.hh:106-120`, frees at once) by
  generation. Two more things item 4 inherits that the ledger does not list: `stable_array`'s
  `std::vector<T*> blocks_` (`write_buffer.hh:161`, `push_back` at `140`) reallocates, so a reader
  indexing an entry races the lease holder's block push although "entries never move" (use a fixed
  array of atomic block pointers, as the lane's directory does); and `has_docid() const`
  (`index.hh:1339`) is a public const method that now builds a map, where the base's const
  lookups mutated nothing (concurrent const calls were safe there).
- **CR8. K2 evidence scope.** Flushed bytes are identical to the base only while a flush covers the
  whole buffer. Two cases differ, both by design but undocumented: CR5's merge, and any retry after
  a failed flush (ENOSPC), where each frozen buffer becomes its own segment (the differential run
  shows one more segment, equal logical content). Record both in the ledger so the byte check is
  not read as unconditional.
- **CR9. Chunk limit margin.** `full()` (`write_buffer.hh:189`) leaves one chunk of room, so a
  single transaction larger than that chunk when the buffer is at 32,767 chunks hits the
  `length_error` at `194` mid-transaction, and `apply` has no rollback on an exception (as on the
  base for `bad_alloc`). Unreachable below about 32 GiB of record headers; make `fits()` compare
  the transaction's record count with the remaining room, or note the bound.

## Notes (follow-ups, no re-check)

- `checkout` keeps a raw `basic_index*`; moving or destroying the index with a checkout live is
  undefined. Say so in the comment.
- Release builds do not exclude a second writer: `lease_scope` reports through `owner_break`, which
  asserts only in debug (`owner.hh:60-64`). That is SR-D20's host contract and matches the design's
  "one writer"; item 4 and the applier must not rely on the word for exclusion in release.
- `test/scan_test.cc`'s `revive_buffered_copy` now only checks that the record exists; the scan
  assertions after it still test "the later record wins", so the test is not vacuous, but the helper
  name is stale.
- The single build commit `74f122c` is titled "WIP" and cites no decision IDs; noted, not blocking.
- Positions: `record.position` is a transaction count in the buffer (`index.hh:2842`); nothing reads
  it yet and `prev` is read only by `write_buffer_test` (my mutant `m_noprev` survives the
  differential for that reason). Item 4's tests must kill it.
- The seed workload for the lead's attention: `slot_after_record` is the only test of WB-9's
  ordering and kills its mutant only under TSan (T10), as the ledger says; confirmed, TSan run of
  `write_buffer_test` passes.

## What held (checked and found sound)

- 32-bit references and the fixed directory: `p8_directory.cc` checks all 32,768 `locate` indexes
  for uniqueness and range (0 bad), reference round trips at the limits, and that `no_ref` cannot
  name a record (it would need a stride of 8 at the last unit).
- Version map: reserve-before-stage-1 covers the worst case of two key slots and two docid slots per
  operation (`index.hh:2870-2882`); the reverse restore is exact (`write_buffer.hh:80-83`, test
  and `p2_diff` rollbacks); a slot is stored only after the record is complete and no field is
  written after publication (grep of every `lane.at(...)` use; none assigns).
- Differential workload `.scratch/rv/p2_diff.cc`, branch against base, ASan+UBSan on both: seeded,
  1 to 5 operations per transaction (rollbacks inside transactions), keyed and keyless adds,
  replaces and conditional replaces, deletes by key, `delete_docid` and key-changing
  `replace_docid` on keyed and keyless documents (stale and invented docids too), metadata,
  refresh, commit, `force_merge`, ENOSPC flush failures with retry, reopen. Every verdict (docids,
  versions, current version), every `get`, `find_key`, `has_docid` and `scan`, and the digest after
  every transaction match the base in 6 seeds (1,500 transactions each, 1 and 3 flush threads), 5
  more with pressure, 5 with a pool for merges (logical trace), and 5 runs in which I freeze and
  flush the oldest buffer at random between transactions (up to 8 frozen buffers, pending deletes
  on 1,500 steps; `index_test_access` as in the unit test). The sensitivity check: five mutants of
  my own (no carry, no key clear at install, no docid clear at install, freeze without applying
  pending, no `prev`) are killed by the trace in 4 of 5 (`prev` is unobserved, above).
- Lease: `lease_scope` and `chore_scope` are correct against each other: the chore takes the word
  by one CAS from free, a nested scope on the same token passes, a writer waits on the chore bit and
  is woken by the store plus `notify_all`; the sanitizer runs show no deadlock or report in
  `write_buffer_test`.
- ASan+UBSan: `ctest` 44 of 44 on this head (`ctest-asan.log`, 569 s).

## Re-check list for the fix round

p4 (both rows bounded), p5 (survives, index fenced), p3 (0 stuck over 100,000), p9 (10 documents,
0 dead), p1 under TSan (no report once the contract is narrowed: the probe then runs reads off the
owner's thread, so change it to read through a view or to stop reading, and keep it as the proof
that the comment is true), p6 (first chunk table), plus the 14 contributor mutants.

## Probes, harnesses and mutants kept (`/Users/kronuz/code/Kronuz/strata-wt/review-wb1/.scratch/`)

- `rv/cc.sh` (compile a probe against a tree's headers; asan, tsan), `rv/cfg.sh` (the two builds),
  `rv/norm.sh`, `rv/norm_fz.py` (trace normalisers).
- `rv/p1_chore_vs_get.cc` (+ `p1_tsan.out`), `rv/p2_diff.cc` (differential; builds
  `p2_branch`, `p2_base`, `*_bg`, `p2_branch_fz`; `-DBRANCH -DCOV -DBG`), `rv/p3_lost_chore.cc`,
  `rv/p4_reject_growth.cc`, `rv/p5_checkin_throws.cc`, `rv/p6_mem.cc`, `rv/p7_sizes.cc`,
  `rv/p8_directory.cc`, `rv/p9_force_merge.cc`.
- `mut/mk.py` and the mutants `m_nocarry`, `m_noinstallkey`, `m_nodocidinstall`, `m_noprev`,
  `m_freezenopend`, `m_keepbytes`, `m_chunk512`.
- `rv/ctest-asan.log`, `rv/ctest-tsan.log`, `rv/wbt_m512.out`; `design.md` and `design-review.md`
  are the design files at `8cdbe61`; `base/` is `git archive fed0c85`.
- Builds: `build-asan` (Debug, `-fsanitize=address,undefined`, Homebrew LLVM clang 23, macOS
  x86_64) and `build-tsan` (`-fsanitize=thread -O1`). Both configure against the sibling
  `shared-reader-views` dependency checkouts at the CMake pins (the lead deleted and restored that
  directory at about 06:22; probes compiled after it use `.scratch/deps`, extracted from the local
  clones at the same pinned SHAs).

## Not run

CI on other platforms (Linux, FreeBSD, arm64), the contributor's `.scratch/wb/bytes.cc` itself (I
extended the idea with `p2_diff`), prospect's search layout (inverting path compiled here, untested
in this repo), the 14 contributor mutants (killed per the ledger; I ran five of my own), any
benchmark or timing, a multi-writer or step-2 pre-resolution check (C8 has no code at this step:
there is no pre-resolution), the item 4 reader protocol, a 32 GiB buffer reaching the chunk
limit (arithmetic probe only), TSan `ctest` beyond the tests listed in `ctest-tsan.log` at the
time of writing (42 of 44 had passed with no report when I stopped; the last two are long tests still
running; see the file) and the TSan run of `write_buffer_test` (passed).

## Re-check (2026-10-26)

Re-checker, scoped to CR1 to CR6 at `f75de3a` (fixes `45bfbaa`, `7b8ebe5`, `1999bdc`, `a7d686b`,
`9af3e71`, `ba91973`). Worktree `/Users/kronuz/code/Kronuz/strata-wt/recheck-wb1`; probes are the
reviewer's, compiled against this head with `rv/cc.sh` (copies, mutants and logs in its
`.scratch/`: `rv/`, `mut/`, `mem/`). `write_buffer_test` passes on ASan+UBSan and on TSan (no
report; `owed chores run: 0 of 40000`); `index_test`, `background_merge_test` and `lifecycle_test`
pass on ASan.

| Finding | Verdict | Evidence |
|---|---|---|
| CR1 | CLOSED | `index.hh:2846` (`rollback` truncates the lane to `mark_.lane`, taken at `index.hh:2930`); `write_buffer.hh:192-203` (`tell`/`truncate`, `destroy` at `249`). p4 on this head, 4000 rejected transactions, buffer share 16 MiB: RSS +0 MiB in both rows (holds a record, `buffered_count` 1; empty buffer, 0); the pre-fix binary gives +305 MiB. Reverted: removing the `truncate` call fails `rejection_frees` (4 checks); removing the destructor call in `truncate` fails 3. |
| CR2 | CLOSED | `index.hh:711-725` (`check_in() noexcept`, so `~checkout` cannot throw), `index.hh:1966-1979` (`run_chores() noexcept`: try/catch sets `fenced_`). p5: "survived check-in" (was abort). `checkin_fences` asserts the next `apply` throws `index_fenced`. Reverted (try/catch removed): the test aborts, rc 134 (`index_fenced` through the noexcept frame). |
| CR3 | CLOSED | Contract narrowed at `index.hh:703-709`; `write_lease::check_read` (`owner.hh:89-95`) is called in `get`, `find_key`, `has_docid`, `scan_keys`, `segments`, `buffered_count`; ledger records the item 4 criterion. p1 unchanged on this head aborts at once in `owner_break` (the debug check fires; 0 TSan reports). p1 with the owner's reads removed (`rv/p1b_no_reads.cc`), TSan, 3 runs: 0 reports (the reviewer's pre-fix run had 44). Reverted (check removed from `get` and `find_key`): `writer_waits_for_chores` fails (expects 2 reports). It is a debug-only, sampling check (it sees a chore only while one is running), as the review accepted; no lease on `get`. |
| CR4 | CLOSED | `owner.hh:133-141` (`lease_scope::release`, handoff seq_cst), `owner.hh:152-164` (`chore_scope` CAS and handoff seq_cst), `index.hh:710-725` (flag store seq_cst, retry CAS, outer loop), `index.hh:1949-1962` (`writer_scope`: release, then seq_cst re-read of the flag and `check_in`), all 20 public lease-taking calls use it (only `open`, `create_deferred` and the destructor keep a plain `lease_scope`: no other thread can hold a checkout on an index that is not yet or no longer shared). p3, 100,000 trials at spans 3000 / 10,000 / 30,000, ASan: 0 / 0 / 0 (pre-fix binary on this host: 196 of 20,000 at span 3000). TSan, 30,000 trials at each span: 0 stuck, no report. Ordering argument below. Reverted: re-read removed, 11,977 of 40,000 trials stuck; retry CAS removed, 298; handoff store release instead of seq_cst, 103. |
| CR5 | CLOSED | `index.hh:746` (`merge`), `757` (`force_merge`), `765` (`force_merge_deletes`) call `apply_pending()` (`index.hh:3149-3164`, the freeze's first half; `freeze` calls it too). p9: after `force_merge(1)` 1 segment, 10 documents, 0 dead, 2578 bytes on the filesystem, equal to the base (pre-fix: 100 / 90 / 3426). `forced_merges_drop_pending` covers all three calls. Reverted: all three removed, 3 failures. |
| CR6 | CLOSED | `write_buffer.hh:30` (`min_chunk = 1024`); `lane_refs` asserts the first chunk costs at most 1 KiB. Reverted to 4096: `lane_refs` fails (1 failure). The memory reconciliation below finds no regression that the constant should change. |

### CR4 ordering argument (not only the trials)

Writer W (a `writer_scope`) and check-in B race. W: `S1: holder_.store(0, seq_cst)`, then
`S2: owed.load(seq_cst)`. B: `T1: owed.store(true, seq_cst)`, then `T2: holder_.CAS(0 -> chore,
seq_cst)`; if T2 succeeds, B runs the chores itself; if it fails, B returns. All four operations
are in the single total order. If T2 fails it read a nonzero holder value, so it precedes in that
order the store that clears that value, which is some holder's seq_cst handoff S1 (a lease_scope
release or a chore_scope destructor); T1 precedes T2, and S1 precedes that holder's own
`owed.load`, so the load sees true (or the value of a later `exchange(false)` in `run_chores`,
whose caller then runs the chores after it). A chore holder re-reads the flag after its
`chore_scope` ends (the scope closes before the `while`), so a flag set during its chores is
picked up by the same argument. On x86 the retry CAS is a full barrier whatever order is
requested, so two weakening mutants (the flag store as release; the CAS as acquire/relaxed)
survive the trials, 40,000 each, and are equivalent on this host; the seq_cst on both is what the
argument needs on arm64 (not run). The weaker handoff store (release) is killed (103 of 40,000),
so the test does exercise the store-load order that x86 allows to reorder.

### Memory reconciliation (CR6)

Same workload for every meter, `.scratch/mem/proto/memtab.cc`: document layout, kvbench 16 B
keys, 100 B values, one record per transaction, `PosixIO`, nothing flushes, Release, macOS
x86_64, base `fed0c85` against head `f75de3a`, three interleaved trials, medians (trial spread
under 2% except the base's resident slope, below). Per open index (the total after `create` and N
documents, averaged over 1000 indexes for N up to 100, 200 for 1000, one for 100,000):

| documents | meter | base B | head B | head - base |
|---|---|---|---|---|
| 0 | requested / in use / resident | 1856 / 2129 / 2769 | 1760 / 2129 / 2765 | -96 / 0 / -4 |
| 1 | requested (counting `operator new`) | 2712 | 4040 | +1328 (+49%) |
| 1 | malloc bytes in use | 2993 | 4433 | +1440 (+48%) |
| 1 | resident | 3674 | 5128 | +1454 (+40%) |
| 10 | requested / in use / resident | 4800 / 5457 / 6128 | 4976 / 5437 / 6111 | +176 / -20 / -17 |
| 100 | requested | 24,736 | 30,088 | +5352 (+22%) |
| 100 | malloc in use | 25,022 | 31,255 | +6233 (+25%) |
| 100 | resident | 26,518 | 28,033 | +1515 (+6%) |
| 1000 | requested | 220,769 | 260,953 | +40,184 (+18%) |
| 1000 | malloc in use | 250,338 | 269,657 | +19,319 (+8%) |
| 1000 | resident | 233,595 | 226,365 | -7230 (-3%) |
| 100,000 | requested | 22,736,712 | 20,363,520 | -2,373,192 (-10%) |
| 100,000 | malloc in use | 30,110,096 | 21,985,472 | -8,124,624 (-27%) |
| 100,000 | resident | 31,805,440 | 22,073,344 | -9,732,096 (-31%) |

Marginal bytes per record from 100,000 to 300,000 buffered records (one index; the K6 method):

| meter | base | head | change |
|---|---|---|---|
| requested | 285.0 (all 3 trials) | 206.2 (all 3) | -28% |
| malloc in use | 300.7 (277 to 306) | 224.7 (222 to 230) | -25% |
| resident | 389.5 (368 to 390) | 223.4 (223 to 229) | -43% |

The 100 B resident slope reproduces the contributor's K6 (base 347 to 390, head 220 to 229; here
390 / 389 / 369 against 223 / 223 / 229). The two earlier numbers do not contradict each other:
they answer different questions.

1. The counting allocator (p6) sums the bytes the index asked `operator new` for. It sees the
   whole of each lane chunk the moment it is allocated, so at 100 and 1000 documents it counts the
   untouched tail of the current doubling chunk (up to about half of what the lane holds), and it
   does not see malloc's per-block overhead or the holes the base's vector doubling leaves. That is
   why it reads +22% and +18% at 100 and 1000 documents while resident reads +6% and -3%: the
   difference between "in use" and "resident" at 1000 documents (43 KB per index against a 64 KiB
   last chunk holding 80 KB of records in 127 KB of chunks) is that tail not being touched
   (inferred from the sizes; I did not instrument page residency).
2. The K6 method (RSS slope between 100,000 and 300,000 records) removes the fixed per-index cost
   and measures what each further record costs on the host. There the head is 43% smaller
   resident, 25% smaller in malloc use and 28% smaller even in requested bytes, because the lane
   packs the record headers inline (80 B stride) instead of growing vectors by doubling.
3. At 100,000 documents all three meters agree the head is smaller (-10%, -27%, -31%). The sign
   changes between 1000 and 100,000 documents because the fixed cost and the tail are amortised.

Which number is right for which question:

- Per buffered record at scale: the resident slope (K6): the head costs 223 B against 390 B. The
  counting allocator understates the base (-27%) and cannot be used for this.
- Per open index on the many-indexes workload: resident, bounded above by malloc bytes in use
  (resident understates it where the untouched tail of a chunk sits in a region the allocator
  keeps apart; on a long-lived process whose freed memory is reused, those pages become
  resident, so use "in use" as the upper bound). By buffered documents: 0 documents, parity; 1,
  +1.4 KB (+40% of that index, from the 1 KiB first chunk and the 536 B `buffer` object); 10,
  parity; 100, +1.5 KB resident (+6%) and +6.2 KB in use (+25%); 1000, -7 KB resident (-3%) and
  +19 KB in use (+8%); 100,000, -31%.

Verdict: not a regression per record, and none for idle indexes. It is a bounded fixed-cost
increase of about 1.4 KB for an index holding a single buffered document (1.4 GB per million such
indexes) and of 1.5 to 6 KB around 100 documents, returned at the flush; it is the cost the CR6
condition was written to bound and the 1 KiB choice does (the 4 KiB first chunk it replaces was
+5.4 KB at one document). The head's win comes after a few hundred records. Open choice for the
designer, not a finding: a 16 x stride (1280 B) first chunk is below the base at 100 and 1000
documents in the contributor's table (22,040 and 209,896 against 22,992 and 219,024 B) but costs
about 250 B more at 1 and 10 documents; the ledger gives its reason for 1 KiB (power-of-two sizes
the allocator serves without rounding).

### Other

- No new findings from the fix commits. `run_chores` now swallows an exception in `apply`'s own
  chore call (`index.hh:681`) where it used to propagate after the verdict; the fence reports at
  the next call, which is the CR2 contract.
- Platform: macOS x86_64 only. The memory table needs the Linux and arm64 runs the cross-platform
  rule asks for before it is quoted as the engine's number.

### Not run

The full ctest on ASan and TSan (the contributor's `ctest-asan-cr.log` and `ctest-tsan-cr.log`
report 44 of 44; I ran `write_buffer_test` on both and `index_test`, `background_merge_test`,
`lifecycle_test` on ASan); the contributor's 14 mutants (I ran my own 11 against CR1 to CR6 plus
the two memory-order mutants); the differential workload `p2_diff` (not in the re-check list); p6
itself (the memory table above replaces it with the same counting method, on 16 B keys); Linux,
FreeBSD and arm64 builds; any timing; the CR7 to CR9 follow-ups.
