# strata review: field table (fields-and-keys F1), durability and decoder-safety check

- Date: 2026-10-28.
- Reviewed: `phase8/field-table` at strata:347a7da (equal to `origin`), `git log 0078bb6..347a7da` (strata:f3b7594,
  strata:4f3c1e7, strata:bcf201d, strata:347a7da): `include/strata/core/fields.hh`, the `fields` changes in
  `include/strata/core/{index,files,reader}.hh`, `test/fields_test.cc`, `fuzz/fuzz_fields.cc` and its corpus,
  `bench/field_table/field_table.cc`, `tools/mutants/fields.py`, `docs/status/field-table.md`,
  `docs/mechanisms/field-table.md`. CI runs 40650894714 (strata:4f3c1e7) and 40653412442 (strata:bcf201d).
- Checked against: `docs/design/fields-and-keys.md` D2, the format table (`:474-481`), F1's criteria
  (`:520-540`), T8 (`:582`), the risks (`:618-634`); `docs/reviews/fields-and-keys-design.md` FK-R1, FK-R11, C6
  and Re-check 1; `docs/design/phase-3.md:21` ("files are written once and never modified");
  `docs/design/track-c.md:800-808` (incremental checkpoint transfer); `docs/perf/idle-index.md` and the
  exploration log's idle-index row (5.9 KB macOS, 8.1 KB Linux per idle index); `docs/reviews/calibration.md`.
- Checked by: Homebrew LLVM 23.1.2, Debug `-O1`, ASan+UBSan (`-fno-sanitize-recover=undefined`), Intel Mac,
  build dir `.scratch/f1-arch/build-asan` (deleted after the review). `fields_test` passes (90 s), `files_test`
  passes, `fuzz_fields_replay` replays 206 corpus inputs. Probes in `.scratch/f1-arch/` (listed at the end):
  a randomized durability probe over 1,000 seeds, a publication allocation probe, five decoder mutants fuzzed
  with the branch's harness, and a reference-oracle harness fuzzed against the stock decoder and four mutants.
  - Not run: TSan (no probe needed threads), the T8 bench (its bytes reproduced analytically below, exact),
    a CPU-hour fuzz campaign, Linux and FreeBSD locally, any benchmark or timing.

## Verdict

**APPROVE WITH CONDITIONS: the append-in-place `fields-<gen>.kv` is sound under strata's crash model and is the
right encoding for T8; the decoder is memory-safe and its rules hold; nothing blocks the design or the code. Landing
waits only on CI green on every platform (L1).**

The ruling the developer asked for: **appending in place is approved.** No byte any manifest on disk names ever
changed across 8,447 injected faults, 10,835 crashes, 6,184 opens at an older manifest and 5,632 installs from a
source that kept appending (FTC-1). It is RocksDB's and LevelDB's MANIFEST discipline, with the same "start a new
file when the tail is not clean" rule. What it gives up, and what it now owes elsewhere, are FTC-2 and FTC-3.

What holds, and reproduces:

- Order: the block is written at the manifest's prefix (`index.hh:4397`), synced with the commit's other new files
  (`:4413`), and only then does the manifest name it (`:4434`, rename at `:4466`). `fields_clean_` drops before
  the write (`:4395`), so any failed write, sync or manifest leaves the next commit writing a new generation (`:4406`).
- Recovery: open reads exactly the named prefix (`read_fields`, `:4850`), checks the last block's CRC against the
  reference and every block's own CRC, and marks the tail unclean when the file is longer than the prefix (`:3521`,
  `:4737`); sizes come from the directory scan, so lazy opens see them too.
- Retention and reclaim: `names_of` adds the fields file, retention keeps it while any kept manifest names it, and
  reclaim's step truncation (`:3412`) only touches files no kept manifest names. Install never copies the file: it
  adopts the decoded table and writes its own generation (`fields_rewrite_`), so a growing source is read by prefix only.
- The storage budget charges only the appended block (`track` sets the size, `:3469`), and the commit allowance's
  `32 + 8 a declaration + name bytes` (`:3509`) covers both encodings (5 B a declaration and 22 B of file framing at most).
- Decoder: every length is bounded by the bytes left (`fields.hh:208, 213`); fuzzing a mutant without the name bound
  is caught by ASan in 10 runs (FTC-5). The rules (name twice, two key fields, empty block, CRC) are each killed by
  `fields_test` (`tools/mutants/fields.py:37-40`).
- T8, reproduced from the encoding: 100,000 names `field-0`..`field-99999` hold 1,088,890 name bytes; the whole
  table is 13 + 3 + 1,488,890 + 4 = 1,488,910 B and the appended file 13 + 100,000 x 5 + 1,488,890 = 1,988,903 B
  (1.34x); at 2,000, 26,909 and 36,903 B (1.37x). Both equal the ledger's figures to the byte.

What blocks: L1 (CI), a landing condition, not a design or code finding.

## Findings

**FTC-1. The core invariant holds: no named prefix changes. Holds; no action.** `probe_durability` runs 1,000
seeds of 60 random steps each: declaring commits under an injected fault one time in three (write, sync, directory
sync, create or replace; EIO or ENOSPC; before or after the effect), power loss at a random mutating operation
inside a declaring commit, a hand-torn tail, a checkpoint, an open at a random manifest on disk, and an install of
the current checkpoint after the source appended more. After every step it decodes every manifest on disk and
compares each named (file, prefix) with the bytes it held when first named; every reopened table must be a prefix of
the applied declarations, and `check()` must pass. Result: `0` changed prefixes, `0` non-prefix tables, `0` failed
reopens, 3,800 distinct named prefixes (`.scratch/f1-arch/durability.log`).

**FTC-2. A torn sector in the prefix's last sector is no longer survivable by falling back. Low; record the
assumption (C1), isolate checkpoints (C2).** Each append rewrites the sector holding the end of the committed
prefix whenever the prefix is not sector-aligned (almost always: blocks are 5 B plus names). faultfs models sectors as
atomic (`faultfs.hh:7-8`), and under that model the probe shows no loss. A device that tears a sector instead
(512e drives' read-modify-write of a 4 KiB physical sector at power loss) damages committed bytes, and then every
manifest naming that file fails to open (`read_fields` throws), retained predecessors and pinned checkpoints
included, because they all name the same file. For a write-once file, older manifests name older files. The loss is
the index's field names, so its segments' field ids lose their meaning. RocksDB's MANIFEST takes the same risk; it is
acceptable under the trusted-cluster, honest-fault model, but it is a new assumption and must be written down.

**FTC-3. Two rules elsewhere assume a named file never grows. Medium for Track C, low today; C1 and C2.**
(1) `docs/design/phase-3.md:21` states "Files are written once and never modified"; it is now false for one file and
the mechanism note is the only place that says so. (2) Track C's catch-up retry "transfers only the files the
receiver lacks" because "strata's segments are immutable" (`track-c.md:800-801`). A receiver holding `fields-3.kv`
from an earlier attempt lacks nothing by name, while the newer checkpoint names a longer prefix of it; the install
then fails `fields file shorter than the manifest says`, or with a size-only comparison could accept a stale copy
if lengths happen to match a different incarnation (below). Nothing in strata copies files by name today (install
re-encodes), so this binds the transfer when it is built.

Pre-existing, found by the same probe: a file name can be reused for different bytes after the file left the disk.
Opening at an older manifest takes its counters, raised only above names still on disk (`index.hh:4741-4744`), so a
generation reclaimed earlier is assigned again: 508 `fields-*` names reused across the 1,000 seeds, every one after
no manifest on disk named the old bytes (so FTC-1 holds). The same lines do this for `meta-*`, live-docs and
segment ids; F1 did not introduce it. It matters to the same transfer rule. Cheap fix, for the lead to queue
separately: on an open at an older manifest, raise each counter to the newest readable manifest's as well.

**FTC-4. Resident cost is about 3x the design's estimate and doubles with a reader. Not blocking for F1 (T8 only
records it); C3 before the many-indexes gate.** T8 records 167 B a field at 100,000 and 171 B at 2,000; that figure
is the process's live-heap delta, so it includes faultfs's copy of the file (19.9 B a field at 100,000) and is about
147 B for the table. FK-R11 estimated the names plus 32 B, and D2 the chain's 8 B: about 51 B for these 11-byte
names. The `std::map` node holds a second copy of each name. A view then copies the whole table whenever a
declaration moved the epoch (`index.hh:2636`): `probe_publish` measures 104 B a field requested at every publication
after one declaration (104,456 B at 1,001 fields, 1,040,456 B at 10,001, 5,200,456 B at 50,001; 0 B when nothing
changed). An index with a reader therefore holds the table twice and publishes in O(F). Against the design's own
corpora and the idle cost: bank's 49 fields add about 7 KB per index, 12 KB with a reader's view, each more
than the whole idle index (5.9 KB); sparse-kw's 2,000 fields about 500 KB with a view, 85x the idle index. The many-indexes workload multiplies
this per open index. A compact form fits in name + about 17 B: the declarations in one arena (the file's own block
bytes), a `uint32` id array sorted by name for lookup, the chain, and views sharing an append-only chunked table
with a size snapshot, so a publication copies nothing.

**FTC-5. The fuzz target has teeth for memory safety and none for the decoder's rules. Low; C4.** Five mutants
of `decode_fields`, fuzzed from a scratch copy of the committed corpus for 90 s each with `fuzz/fuzz_fields.cc`:

| Mutant | Change | Branch harness | Reference-oracle harness |
|---|---|---|---|
| m1-name-bound | `len + 3 >` becomes `len >` (reads 3 B past the end) | found, run 10, ASan heap-buffer-overflow | not needed |
| m2-name-twice | accepts a name twice | survived 2,686,235 runs | found, run 3 |
| m3-crc-off | skips each block's CRC | survived 2,214,131 runs | found, run 13 |
| m4-count-zero | accepts an empty block | survived 2,268,529 runs | found, run 13 |
| m5-two-keys | accepts two key fields | survived 2,371,543 runs | found, run 13 |

The harness's only oracle is "decode, encode, decode again, compare", which a decoder that accepts too much passes
by symmetry. `fields_test` kills m2 to m5 (`tools/mutants/fields.py:37-40`), so the rules are covered, but the
11.2M fuzz runs prove memory safety only. `.scratch/f1-arch/fuzz_fields_oracle.cc` adds an independent decoder
written from the format table and requires the two to agree on accept or refuse and on every declaration: 1,466,040
runs clean against the stock decoder in 60 s, and it finds m2 to m5 within 13 runs.

**FTC-6. Small notes, no action needed for F1.** The file's growth is bounded by construction, not only on T8's
names: framing is 5 B a commit and a declaration is at least 5 B, so bytes written are at most 2x the final table
plus 13 B a file, within T8's 4x for any workload. A manifest with `fields.generation == 0` but a non-zero size or
CRC decodes (no canonical-form check, harmless). The CRC and the chain are host-endian `memcpy`, consistent with the
project's little-endian-only formats (`files.hh:153`).

## Blocking items / Required changes

- **L1:** CI green on every platform at the landing commit (F1 criterion 3). At strata:4f3c1e7 the Linux and FreeBSD
  jobs were stopped by a runner shutdown (`##[error]The runner has received a shutdown signal`, the log in
  `.scratch/f1-arch/ci-4f3c1e7-linux-clang.log`), as the ledger says. At strata:bcf201d five jobs ended with exit 143
  (SIGTERM; annotations of jobs 122707234905 and 122707234363) while `linux-x86_64-gcc` was still running, so their
  logs were not readable; strata:347a7da has no run. No job has yet passed on Linux or FreeBSD.

## Conditions

- **C1 (before F1 lands; the lead edits the designs):** `phase-3.md:21` names the exception: `fields-<gen>.kv` grows
  by appended blocks past the prefix a manifest names, and no byte any manifest may name is overwritten. The
  mechanism note and phase 3's durability assumptions add the one this relies on: a power loss leaves each physical
  sector with its old or new bytes (FTC-2).
- **C2 (before any file-level transfer of a checkpoint, Track C or 3R, lands):** a file a pinned checkpoint names is
  never appended to again: the first declaring commit after `checkpoint()` writes a new generation (one whole table
  write a checkpoint, which also gives checkpoints back FTC-2's isolation). And any transfer or "files the receiver
  lacks" rule keys files by (name, size, CRC), never by name alone (FTC-3).
- **C3 (before the many-indexes gate, or with F2 if the lead prefers):** the resident table at most name + 24 B a
  field, views sharing it so a publication copies nothing, and T8 rerun reporting the table alone (without faultfs's
  bytes), with and without a reader, at 49, 2,000 and 100,000 fields; `bench/idle_index` on a 49-field bank index
  before and after (FTC-4).
- **C4 (by the phase gate):** `fuzz_fields` gains the reference oracle (or an equivalent spec decoder), killing m2
  to m5 in the gate's mutant run, and the gate's CPU-hour of fuzzing runs on it (FTC-5).
- Separately for the lead's queue, not F1's: the counters' name reuse on an open at an older manifest (FTC-3, pre-existing).

## Probes (kept in `.scratch/f1-arch/`)

- `probe_durability.cc`: the randomized fault, crash, open-at, checkpoint, install and torn-tail probe; run
  `./probe_durability 1000` (`durability.log`).
- `probe_publish.cc`: bytes allocated by a publication after one declaration (`publish.log`).
- `mutants/m1..m5/strata/core/fields.hh`, `build_fuzz_mutant.sh`, `run_fuzz_mutants.sh 90`: the five decoder
  mutants against the branch harness (`mutants/*/fuzz.log`, the m1 crash input in `mutants/m1-name-bound/art/`).
- `fuzz_fields_oracle.cc`, `run_oracle.sh 60`: the reference-oracle harness against stock and m2 to m5
  (`mutants/*/fuzz_o.log`).
- `build_probe.sh`, `cxxflags.txt`, `link.txt`, `fuzzflags.txt`, `fuzzlink.txt`: build any probe with
  `fields_test`'s or `fuzz_fields`'s exact flags. They need `build-asan` rebuilt first (deleted after this review):
  `cmake -S . -B .scratch/f1-arch/build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/local/opt/llvm/bin/clang++
  -DCMAKE_C_COMPILER=/usr/local/opt/llvm/bin/clang -DSTRATA_FUZZ=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  "-DCMAKE_CXX_FLAGS=-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"`, then build `fields_test fuzz_fields`.
- `ci-4f3c1e7-linux-clang.log`: the stopped CI job's log.
