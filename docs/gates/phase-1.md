# Phase 1 gate: foundation libraries

Every library is a standalone public repository with tests, a benchmark, README,
ARCHITECTURE, and AGENTS files, and an entry in `Kronuz/cpp-libraries`. Numbers come
from the macOS development host and the Linux gate VM; both shared the host with
other work, so rows marked provisional are re-run on a quiet host.

## Results

| Library | Gate target | Result | Status |
|---|---|---|---|
| `bitpack` | Decode with delta ≥ 90% of SIMD-BP128/D1 and ≥ 3× scalar | 0.97× SIMD-BP128 decode, 1.00× with delta (Linux VM); 0.99×/1.02× (macOS); 2.2–6× scalar (2.2× at 16 and 32 bits, where the scalar path is word-aligned); bytes identical to SIMD-BP128 at all 32 widths | Pass |
| `bitpack` | Encoded size within 2% of Lucene on the same postings | Doc deltas + frequencies: 958.7 MB (varint tails), 934.9 MB (LEB128 tails). Lucene's `.doc` with skip and impact data: 990.6 MB; its skip data is an estimated 26–44 MB | Estimate; exact once Phase 2 writes skip data |
| Tails | Size and decode of the tail codec | 4.5% of entries. LEB128 1.85 B/value, 2.60 ns; varint length format 2.30 B/value, 3.61 ns; StreamVByte 1.99 B/value. LEB128 added to `varint` | Pass; tails use `varint::leb128` |
| `fst` | Lookup p50 within 2× an open-addressing hash map at ≤ 25% of its bytes (Wikipedia vocabulary) | 10% of the bytes (5.91 B/key). Full-vocabulary lookup 2.43×; `floor_value` over a 1/32 block index 1.63× | Size passes. Full-vocabulary lookup misses (memory-latency bound, 23 MB FST beyond the LLC); the block-index use the architecture specifies passes |
| `docset` | Within 1.2× of CRoaring, densities 1e-6 to 0.5 | Worst rows at density 0.01: OR 1.15×, ANDNOT 1.07–1.17× (Linux VM); ≤ 1.14× (macOS); most rows below 1 | Pass, provisional |
| `task` | No-op task ≤ 30 ns with zero `malloc` | 13.9–21.5 ns (Linux, GCC/Clang), 17.9 ns (macOS), 0 allocations | Pass |
| `task` | Asio yield-to-resume p99 ≤ 1 µs, idle loop | p99 175 ns (macOS) | Pass |
| `task` | Affinity, cancellation at yield, exceptions, 10^6-deep symmetric transfer in constant stack, 10^8 tasks with random cancellation under ASan, UBSan, TSan | All pass; 10^8 in a release build, 10^7 under ASan/UBSan and TSan (macOS), 10^6 under GCC and Clang sanitizers on Linux, AArch64 under qemu | Pass |
| `residency` | Interface with a synchronous backend | `file`, `pinned` (CRC-32C verified), `backend` concept, `sync_backend` | Pass |
| `strata` | Scorer iterator concept | `strata::scorer`: `doc_iterator` plus `score()`, `block_end(t)`, `max_score(upto)` | Pass |
| `hashes` | CRC-32C before segment footers use it | Hardware CRC-32C, 14–19 GB/s, about 40× the journal's byte loop | Pass |

## Findings during the phase

- **Symmetric transfer is not portable.** GCC 13 emits the tail call only at `-O2`
  without sanitizers; at `-O0`, `-O1`, or under ASan a million synchronous awaits
  overflow the stack. `task` uses a per-thread trampoline unless
  `TASK_SYMMETRIC_TRANSFER` is set (the default on Clang).
- **POPCNT is not implied by SSE4.2 target attributes on Clang.** `docset`'s kernels
  computed popcounts with bit tricks until `popcnt` joined their target lists.
- **`pcmpistrm` versus `pcmpestrm`.** The implicit-length form is a third of the
  micro-ops; zero lanes appear only at an array's start, which takes the explicit form.
- **Xapiand's length codec is MIT** by Olly Betts' permission (Xapiand `1691e5b2e`);
  `varint`'s README now says so.

## Phase 0 status

- Indexing and query baselines for Tantivy 0.26, Lucene 10.4.0, and Xapian 2.1.0 on
  the 5M-document Wikipedia corpus are recorded (`bench/phase0/`), provisional. Xapian
  is 6–19× slower than Lucene and Tantivy on every query command and indexes at 881
  docs/s against about 14,000.
- Open: the Xapiand RESTORE and query profiles, the offload hop cost, the light-query
  p99 under heavy load, and the GPL-side oracle vectors.
