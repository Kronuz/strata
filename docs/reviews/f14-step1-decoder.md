# strata security review: format 14 step 1, decoder safety

- Date: 2026-10-20.
- Reviewed: Kronuz/compressors `block-codecs` at `9d2a87c` (from main `ed590c8`; `origin/block-codecs` is the same
  commit); Kronuz/strata `phase4/f14-step1` at `42ebbb4` (main `6e13416`; HEAD `47bd00e` adds only the
  ledger, and equals `origin/phase4/f14-step1`). Files: `compressor_lz4.cc:395-421`, `compressor_zstd.cc:166-190`,
  `include/strata/core/{reader,format,writer,merge,index,keys}.hh`, `fuzz/fuzz_stored.cc`, `test/format14_test.cc`.
- Checked against: the task's threat model (a trusted cluster: inputs are our own files, possibly corrupt or truncated by
  disk faults or torn writes; a decoder rejects or answers, never reads or writes out of bounds, never loops without
  bound, never allocates from an unchecked length); `docs/design/format-14-kv.md` D1a, D1c, D1d; the K1 to K7 proof in
  `docs/status/f14-step1.md`; `docs/design/limits.md`; `docs/reviews/calibration.md`.
- Checked by: Homebrew LLVM clang 23.1.2 with `-fsanitize=address,undefined -fno-sanitize-recover=all` (CMake
  RelWithDebInfo, so `-O2 -DNDEBUG`), build dir `.scratch/review/build-asan`; `format14_test` and `core_kv_test`
  (both pass); `fuzz_stored_replay` over the 249-input corpus with LeakSanitizer on (clean); the probes below; ten
  mutants of `decode_stored_frame`; one CPU-hour of `fuzz_stored`.
  - Not run: see the last line.

## Verdict

**APPROVE WITH CONDITIONS: no decoder-safety blocker; one limit regression (SR1) and four low items to record.**

What holds, and reproduces:

- Both codec wrappers are exact-size and bounded. `decompress_lz4_block` (`compressor_lz4.cc:413`) and
  `decompress_zstd_into` (`compressor_zstd.cc:183`) returned false for every wrong size (n-1, n+1, n+4096, empty
  source), every truncation, 20,000 random multi-byte mutations per size per codec, with canary bytes on both sides of
  `dst` untouched, and per-thread contexts decoded correctly straight after failed decodes, from four threads
  (`probe_codecs`).
- `decode_stored_frame` (`reader.hh:440-496`) rejected or answered correctly on 8,083 damaged-segment runs (every checksum
  resealed so the damage reaches the decoder): unknown codec bytes, a size larger than the block or the cap, sizes that
  disagree with the header by 1 and 4 in both directions, zero-length frames, a last frame empty or one byte long, frames
  begun past the index, overlapping or reordered offsets, wrong first ordinals, wrong frame counts, hostile record
  headers (counts of 2^32, 2^63, 2^64-1, lengths that wrap 2^64 or 2^32, a missing length, trailing bytes) inside valid
  raw, LZ4 and zstd payloads, and every prefix of the file. Outcomes: ok (identity edits only), `corrupt_segment`;
  no other exception, no sanitizer report (`probe_stored`).
- `read_index_sort` (`format.hh:440-457`): 3,000,000 random and structured strings under five structure-bit sets, in
  exact-size heap buffers: ok, `corrupt_segment` or `unsupported_segment` only (`probe_index_sort`).
- Random damage to the keys, key_index, versions, docids and stats sections (resealed), 2,630,000 iterations: only
  `corrupt_segment` or ok; whenever `check()` passed a key-ordered segment, every key sat at its rank
  (`probe_sections`). `check()` (`reader.hh:359-381`) is sufficient for the rank claim: nothing in step 1 reads
  `key_ordered()` except `check()`, so an unchecked false claim costs nothing at runtime (merges order by their own
  k-way merge of the keys, `merge.hh:63-103, 138-166, 240-290`).
- The per-thread get buffer is reentrant and exception-safe: nesting three and four deep over two indexes and the same
  index, the buffered path nesting into the segment path, a throwing callback, a throwing nested decode on a damaged
  segment, a 300 KiB record inside a small one and the reverse, four threads hammering nested gets, and a thread that
  exits all kept every outer view intact under ASan/UBSan, and the next top-level get reused the thread's buffer (same
  address), so the busy flag is released on every path (`probe_reentrancy`, `index.hh:277-307`).
- The probes are sensitive: of ten mutants of `decode_stored_frame`, the raw-size check (m1), the record-length overflow
  check (m3), the header-sum check (m4), the `n <= doc_count` check (m7), the unknown-codec default (m2) and the holds
  check in `stored()` (m10) are each caught; m6 (no `max_stored_frame` cap) shows the cap is load-bearing: a 5-byte
  frame then allocates 4,294,967,303 bytes. m5, m8 and m9 are defense in depth that other checks mask.

What blocks: nothing.

What the conditions are: C1 (SR1) and C2 (SR2 to SR5 recorded; SR5 before a release).

## Attackers considered

1. Disk faults and torn writes (the stated model). Section and region checksums catch these before the decoders run;
   the strongest in-model approximation, and what the probes use, is damage that survives the checksums (resealed), so
   every length, codec byte, offset and count is attacked as if the checksum had collided.
2. Someone with local file access who crafts a segment (outside the model, tested anyway with the same generators):
   no out-of-bounds access or unbounded loop found; the largest allocation a crafted frame can force is bounded by
   constants (SR3).
3. A compromised dependency: `compressors` is pinned by commit, but it pulls LZ4 and zstd by tag (SR5).
4. A caller of `get(key, f)` whose callback calls back into the index (reentrancy, exceptions): safe (above).
5. A peer node holding a key, a client with valid credentials, the network: not in this change's reach (no wire
   protocol, no authentication, no secrets or file modes touched).

## Findings

### SR1 (medium): records of zero bytes never close a block; a segment of more than 67,174,459 empty-valued documents cannot be written

`encode_stored` closes a block on the target's record bytes (`writer.hh:264`: `stored_ends_[last + 1] - begin <= block_`).
A record of zero bytes adds nothing, so N empty-valued documents land in one frame. Consequences:

1. The frame's raw size is about N bytes (a count, then one length byte per record), and step 1 caps it at
   `max_stored_frame` = 67,174,464 (`writer.hh:269`, `format.hh:100`). Past that the flush or the merge throws
   `std::invalid_argument("strata: stored record too large")` (a merge's `invalid_argument` fences the whole index,
   `index.hh:1458`). Main capped the same frame at 2^32 bytes. `docs/design/limits.md:16` states the documents per
   segment limit as 2^32 - 2, "at the largest"; for an index of keys with empty values (sets, tombstone-like markers) it
   is now 67 million, 64 times below, which is a limit below the largest of Lucene/Tantivy/Xapian that nothing names.
   Probe: `probe_empty_records_nosan 67200000` prints `flush threw: strata: stored record too large` (60 s, nice 19).
2. Every get on such a segment decodes the whole frame: with 1,000,000 empty values, one frame of 1,000,003 raw bytes,
   and a decode buffer of 5,000,015 bytes (the payload plus a 4-byte offset per record, `reader.hh:477-479`). That
   buffer is above the 256 KiB the per-thread buffer keeps (`index.hh:286`), so each get allocates, zero-fills and
   frees it: K4's "allocates nothing" does not hold for this workload. Probe: `probe_empty_records 1000000`.

Fix: close a block also when it holds `block_` records (or any fixed count), so a frame's count is bounded by the
block target and the 64 MiB frame cap is reached only by a record that large. It changes bytes for segments with more
than 4,096 records per block only, but it is a writer-rule change: bump `pre_freeze` and re-pin the fixtures if any
fixture crosses it. Decoder side: the existing `n <= raw_size` bound stands.

### SR2 (low, pre-existing): a stored section shorter than 4 bytes in a segment with documents reads as "every record empty"

`stored_frames()` returns 0 for a section under 4 bytes (`reader.hh:431`) and `stored_frame_of` returns "none"
(`reader.hh:502`), so `stored()` answers an empty record for every ordinal. The writer never emits fewer than 4 bytes
(`encode_stored` always appends the frame count), so for any segment with documents a shorter section is damage; a
section of exactly 4 zero bytes is refused (`reader.hh:505`, "stored index"). `open`, `verify_all()`, `check_segment`
and every `stored()` call accept sizes 0, 1 and 3 in all 12 codec/block combinations: silent loss of every record
(`probe_stored`, "FAIL ... section-size=0/1/3": 36 runs). The damage needs a section length change that passes the
table-of-contents checksum, so this is not reachable by a torn write; still, `check()` is the line that should say so.
Fix: in both functions treat `s.size() < 4` as corrupt when `doc_count_ > 0`.

### SR3 (low): `raw_size` from the index drives a zero-filled allocation before the frame is looked at

`decode_stored_frame` resizes the buffer to `raw_size` (up to 64 MiB + 64 KiB + 64) at `reader.hh:457`, before the
codec byte or the payload size is consulted, and the record-offset table then grows it by 4 * (n + 1) bytes with
`n <= min(raw_size, doc_count)` (`reader.hh:476-479`): up to about 320 MiB for one frame, per thread, per nested get.
Probe: a 5-byte frame whose recorded size is 67,174,464 leaves a 67,174,471-byte buffer behind (`probe_stored`, peak
line); a 63 MiB frame with `n = raw - 8` in a segment with enough documents reaches 330 MB (mutant m7 shows it, since
the `n <= doc_count` check bounds it in the unmutated code). Needs checksum-passing damage, hence low. Fix, all cheap
and before the resize: raw frames require `payload.size() == raw_size` first; LZ4 `raw_size <= 255 * payload.size()`;
bound `n` by `rend - p` (each length takes a byte) rather than `raw_size`.

### SR4 (low): a source that opens but is inconsistent fences the index instead of being quarantined

A key-ordered merge over a source with two keys naming one ordinal, or a dropped key whose document keeps a version
(both refused by `check()`), throws `std::invalid_argument` ("merge order must name every live document once", "a
version without a key": `merge.hh:369, 374`, `writer.hh:167`) and `std::logic_error` (`merge.hh:150`). The merge handler
sends `corrupt_segment` to `mark_damaged` and every other exception to `fence()` (`index.hh:1453-1459`), so the whole
index stops. A bad codec byte or a missing stored index in a source is classified correctly (`corrupt_segment`).
Probe: `probe_merge_damage`. The class existed on main (the version-without-key case); step 1 adds the duplicate-ordinal
case in `order_source<document_layout>::order` (`merge.hh:258`). Needs checksum-passing damage. Fix: throw
`corrupt_segment` from those three sites (the source is what is inconsistent), or run `check()` on sources first.

### SR5 (low): compressors pulls LZ4 and zstd by tag

`compressors/CMakeLists.txt:53, 98` use `GIT_TAG v1.10.0` and `v1.5.7` where strata pins every dependency, including
`compressors` itself, by commit (`CMakeLists.txt:20`). A moved tag changes the decoders that read these files. The
commits this build fetched: LZ4 `ebb370ca83af193212df4dcbadcc5d87bc0de2f0`, zstd `f8745da6ff1ad1e7bab384bd1f9d742439278e99`
(`git describe` reports the tags). Fix: pin those commits in compressors, then repin compressors in strata.

### Observations (no action required)

1. `decompress_zstd_into` accepts a source that is a concatenation of frames, or has a skippable frame in front, when the
   total is exactly `dst_size`, and an empty source for `dst_size == 0`; bounded by `dst_size`, and the empty source
   fails the header parse that follows (`probe_codecs` NOTE lines). The header comment says "malformed or decodes to any
   other size"; it is slightly stronger than the behavior.
2. `lent_frame` keeps up to 256 KiB per thread for the thread's life (`index.hh:286`): 1,000 threads hold 256 MB.
3. `fuzz_stored` mutates only the stored and index_sort sections (`fuzz_stored.cc`); its coverage stopped at 2,054
   edges after about 4,000 executions (the fuzz section below). `probe_sections` covers the other sections.
4. `get(key, f)` lends a view that lives until the thread's next decode (`reader.hh:517`); a callback must copy what it
   keeps. The buffered path lends a view into the write buffer instead; with PROBE_MUTATE set, `probe_reentrancy` applied
   4,000 writes, a refresh and a force_merge from inside callbacks without an ASan report, but nothing states the
   contract.

## Blocking items / Required changes

None.

## Conditions

- C1: SR1 fixed before the first release (it changes bytes, so with the next pre-freeze bump at the latest), or
  `limits.md:16` changed to state the empty-record cap and the owner's ruling recorded.
- C2: SR2 to SR4 fixed in a follow-up commit and SR5 before a release; `rechecker` scope is `git log 42ebbb4..HEAD`.

## Probes (all in `.scratch/review/`, kept)

| File | What it does |
|---|---|
| `probe_codecs.cc`, `probe_codecs.sh` | The LZ4 and zstd wrappers: wrong sizes, truncations, mutations, canaries, concatenated and skippable frames, capacity, contexts across threads |
| `probe_stored.cc` | Damaged-segment generator: 8,083 runs over stored-section fields, hostile record headers, file prefixes, short sections, stale-buffer frames; reports silent wrong data and the peak decode buffer |
| `probe_index_sort.cc` | `read_index_sort` on 3,000,000 strings |
| `probe_sections.cc` | Random damage to keys, key_index, versions, docids, stats; rank invariant after `check()` |
| `probe_reentrancy.cc` | The per-thread buffer: nesting, throws, damaged nested decode, big records, threads; `PROBE_MUTATE=1` adds writes inside callbacks |
| `probe_empty_records.cc` | SR1: frame count and sizes for N empty-valued documents |
| `probe_merge_damage.cc` | SR4: which exception a merge over an inconsistent source raises |
| `mutate.py`, `mutants/m1..m10` | Ten mutants of `decode_stored_frame` and the probe results against each |
| `build_probe.sh`, `run-fuzz.sh` | Build a probe against the ASan build's flags (`INC=` to point at a mutant's headers, `NOSAN=1`); the fuzz command |

## Fuzz

`fuzz_stored` at `42ebbb4` (HEAD adds only the ledger), built with Homebrew LLVM clang 23.1.2, `-fsanitize=fuzzer,address,undefined
-fno-sanitize-recover=all`, seeded with the 249-input checked-in corpus copied to `.scratch/review/fuzz-corpus` (the
checked-in corpus untouched), run as `QUIET_LEVEL=light quiet run f14-fuzz 70 "f14 fuzz_stored 1 CPU-hour" -- fuzz_stored
-max_total_time=3600 -rss_limit_mb=4096`, one process, one CPU (`run-fuzz.sh`, log `fuzz-0.log`), 18:12:32 to 19:12:40 PDT.

**Result: 961,754 executions in 3,601 s (267 exec/s), 0 crashes, 0 sanitizer reports, 0 artifacts.** Coverage 2,054 edges
(2,054 at start; 2,055 at the end), features 5,514 to 5,525, corpus 238 to 247 units (458 files after the run). Coverage
had stopped growing after about 4,000 executions; the later hour mutated the same shape of segments (observation 3).
RSS grew from 346 MB to 1,115 MB over the hour without reaching the 4,096 MB limit; the final corpus replayed under
LeakSanitizer (`detect_leaks=1`) reports no leak, so the growth is the fuzzer's and ASan's own (large allocations, the
quarantine), not the decoder's. The developer's 20-minute run (ledger K5, 367,541 executions, log
`.scratch/fuzz-stored.log`) ended at 2,367 edges, more than this run's 2,055; a 4-minute run of mine from an empty
corpus (`fuzz-empty.log`, 66,776 executions) reached 1,993. I did not find why the counts differ (the edge count
depends on the build's flags and on `-max_len`); the checked-in corpus plus an hour found no new crash either way, and
the probes cover the same fields directly.

Not run: TSan (the per-thread buffer is thread-local and the index is single-writer; the four-thread hammer ran under
ASan only); a Linux/FreeBSD build of the probes (CI covers the branch on eight jobs per the ledger; I did not re-read
the run); `fuzz_files`; fuzz of the keys/key_index/docids sections by libFuzzer (random damage in `probe_sections`
instead); mapped-file truncation under a live mapping (SIGBUS, pre-existing, untouched by this change); the
`COMPRESSORS_*` options other than the pinned LZ4/zstd build strata uses; compressors' own `compressors_test`; the
mutants against `fuzz_stored` (they ran against `probe_stored`); anything over the network or on another host.
