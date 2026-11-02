# strata review: X3-1, the chunked per-index reader registry

## Architect check (X3-1)

- Date: 2026-11-02.
- Reviewed: branch `phase7c/x3-1-chunked-registry` at strata:929f5c7 (strata:c784d33, strata:12a9d89); `include/strata/core/hazard.hh`, `include/strata/core/index.hh:465-475, 1274-1355`, `docs/design/shared-reader.md` R4.1 to R4.8 (`:624-668`), `docs/status/x3-1-chunked-registry.md`, `bench/shared_reader/registry.cc:24-85`; Xapiand master `src/engine/strata/shard.cc:185-240, 460-472` (the registry's one user).
- Checked against: the ruling in Xapiand `docs/reviews/phase7c-x2b-code.md`, "Architect ruling on the reader registry" (R1, R2, X3-1, X3-2); shared-reader D4, D5, D9; the rule that every limit names its mechanism.
- Checked by: reading the claim, link, scan and destructor paths against the seq_cst total order; probes in `.scratch/arch-x31/`, Homebrew clang 23, macOS x86_64 (i9-9980HK):
  - `acquire_tls.cc`, `acquire_fixed.cc`, `onetls/strata/core/hazard.hh` (the branch header with a one-TLS hint): acquire+release, 1 thread, best of 5 x 0.3 s, three interleaved rounds under `quiet strict` (`acquire_abc.txt`, `acquire_tls.txt`); TLS references counted in `acq_branch.s`, `acq_onetls.s`.
  - `slot_walk.cc` under ASan: Xapiand's `claimed()` walk on the chunked registry (`sanitizers.txt`).
  - `dtor_race.cc` under TSan: a guard released on one thread while the registry is destroyed on another, 2,000 rounds (`sanitizers.txt`).
  - Not run: the branch's own test suite, litmus and TSan stress (taken from the ledger); arm64, Linux, FreeBSD; 8-thread timing; the developer's bench binary; any Xapiand build.

### Verdict

**LAND WITH CONDITIONS: the protocol is sound and matches the ruling; one small API gap (A3) blocks, because Xapiand's only use of the registry turns into an allocation of the whole capacity on the chunked list.**

What holds: chunks of 4, empty until the first acquire (`hazard.hh:136`, `index.hh:1347-1349`); a new chunk built whole with record 0 preclaimed and linked by one seq_cst compare-exchange (`hazard.hh:335-341, 284-287`); no unlink or free while the registry lives, chunks kept when it dies with a record claimed (`:140-155`); the cap at `reader_records` 4096 (`index.hh:471-475`).

What blocks: A3.

### Findings

**A1. The link and claim protocol has no unsafe interleaving (ruling item 1).** These are the cases I checked, each against the single total order S of seq_cst operations:
1. A reader that appends a chunk: its record write is the relaxed store at `hazard.hh:336`, published by the seq_cst compare-exchange at `:286`, which precedes its validation load in S. If that load read the old pointer, it precedes the owner's publish, which precedes the owner's link load. The link load therefore reads the new chunk, and because it synchronizes with the compare-exchange it sees record 0 naming the old pointer, or a later retry by the same reader.
2. A reader that claims a record in a chunk someone else linked: it reached the chunk by a seq_cst link load that read the compare-exchange, so the link precedes its claim in S, and the owner's walk after its publish reaches the chunk.
3. A scan racing a link: if the walk read a null link, the link comes after the owner's publish in S, the appender's validation reads the new pointer, and `settle` (`:241-248`) rewrites the record before returning. R4.5 states this correctly.
4. A stale hint: `rec` is trusted only when `id` matches (`:279`). Registry ids are never reused (`:263-266`), and records never move while their registry lives, so a cached record from another registry is never dereferenced. A cached record whose index is past the cap cannot happen, because hints are set only from claims below `n` (`:323-331`) or from a chunk appended under the cap (`:334-339`).
5. Two appenders with the same `total`: one compare-exchange wins, and the loser's `make_unique` frees the unlinked chunk and retries (`:337`). The number of chunks stays at most ceil(cap/4), and a cap that is not a multiple of 4 leaves the tail records unclaimable (`:323`), which is correct.

The proof stands on the ledger's litmus (K = 1, so every race crosses a link) and its TSan-only kill of `link-relaxed`. That kill is the right evidence, because x86 cannot show the relaxed link natively. Not rerun here.

**A2. The extra ~1 ns on acquire is not inherent to the shape. The cheapest fix is one thread-local instead of three (ruling item 2).** `hint()` (`hazard.hh:275-281`) has a dynamically initialized `thread_local mine` beside `thread_local h`. On Darwin that produces three TLV variables (`mine`, its guard, `h`) and 5 `@TLVP` references in `acq_branch.s`. The variant in `onetls/` keeps the seed inside `hint_t`, computes it on the first id mismatch, and leaves `h` constant-initialized: 1 `@TLVP` reference. Interleaved, 1 thread, in ns (`acquire_abc.txt`):

   | Round | Fixed (bench arm) | Branch | One TLS |
   |---|---|---|---|
   | 1 | 7.95 | 7.86 | 7.40 |
   | 2 | 7.80 | 7.90 | 7.46 |
   | 3 | 7.91 | 7.86 | 7.69 |

   In this loop the branch matches the fixed arm, and the one-TLS hint is 3% to 6% faster than both. The developer's ~1.1 ns gap therefore comes partly from the bench's loop shape, not only from the registry. A hinted acquire never walks chunks, so the chunk size cannot move it. A first chunk inline in the registry would cost 320 B (640 B on Apple arm64) for every open index with no reader. That breaks the ruling's "zero until the first reader" at millions of indexes, so it is rejected. Accept the shape, and take the one-TLS hint (C1). Not measured: 8 threads and arm64, where TLS access is cheaper (a thread-pointer register) and the gap may differ.

**A3. Xapiand's `claimed()` allocates the whole capacity on the chunked registry (ruling item 4; blocking).** `StrataReaders::claimed()` (Xapiand `shard.cc:188-193`) runs `registry.slot(i)` for every `i < capacity()`. On the chunked registry, `slot()` (`hazard.hh:249-260`) appends empty chunks until record `i` exists. `slot_walk.cc` with a cap of 4096: `allocated` goes from 0 to 4096 records, 327,680 bytes of chunks, on every `~Core` (`shard.cc:233`). That includes indexes no reader ever touched. With `unbounded_records` the walk never ends. `slot()` can also throw `bad_alloc` inside a destructor. The registry already computes "is a record claimed" in its own destructor (`hazard.hh:141-145`) but does not expose it, so the R2 keep-alive has no non-allocating query. Fix in strata: add `bool claimed() const noexcept` that walks the existing chunks with acquire loads (see A4), use it in the destructor, add a test that `allocated()` is unchanged after it, and mark `slot()` in its comment as a test-only function that allocates. Then Xapiand switches to it (C3).

**A4. The destructor's held check uses relaxed loads (`hazard.hh:143-144`) (low).** Suppose a guard's release store races the destructor, and the relaxed load reads that store's null. The destructor then runs `delete c` with no happens-before from the store, which is formally a data race on the chunk. `dtor_race.cc` under TSan reported nothing in 2,000 rounds: the window is narrow and most rounds took the kept path. Use `memory_order_acquire` there (the same walk as A3's `claimed()`). It costs nothing on a teardown path.

**A5. The 4096 default works as a memory and scan backstop, not as a reader limit (ruling item 3).** Nothing in Xapiand reaches 4096 readers of one index before X3-2. `run` is synchronous, so readers per index are bounded by threads x nesting (tens), and Xapiand's `AdmissionGate` refuses at `readers_per_index` before a record is taken (`shard.cc:186, 1524`). What 4096 bounds is a runaway: about 320 KiB of chunks per index on x86_64 (80 B a record with the link line; 640 KiB on Apple arm64), kept until the index closes (R4.4), and a scan of about 8 us (the ledger's 2.05 us at 1,024 claimed, times four). Both stay acceptable for one index and do not compound across indexes. R4.8 says the cap is an overload cap set from X3-2 but does not give this mechanism or the 4096 figure. That belongs in R4.8 and in D10's registry row (C2). Until X3-2, Xapiand keeps its gate at `readers_per_index` and may pass `reader_records` = `readers_per_index`. The ruling's "Xapiand drops `readers_per_index`" waits for X3-2's admission cap (C3).

**A6. R2 (a registry outliving its index while a guard is held) is covered in strata, and covered in Xapiand once A3 lands (ruling item 4).** In strata, the index-owned registry lives in `reader_state` (`index.hh:1277-1281`), shared by the publication and every `reader_handle`. When the last owner drops it with a record claimed, the destructor keeps every chunk (`hazard.hh:148-150`), so a late release writes a live line. `~basic_index` already leaks retired views a record names (`index.hh:624-631`), and the `kept_chunks` mutant shows the ASan kill. In Xapiand, which passes its own registry, `~Core` keeps `StrataReaders`, and with it the gate, when `claimed()` is true (`shard.cc:233-235`). That is correct, apart from A3's allocation. Either way the late release lands in kept memory, and Xapiand still needs its own keep-alive, because the admission gate must outlive the guard.

### Blocking items

- **L1 (A3, A4).** Add `basic_reader_registry::claimed() const noexcept`: it walks the existing chunks with acquire loads and never allocates. The destructor uses it, and a test shows `allocated()` unchanged after it on an empty registry and on one with chunks. Mark `slot()` as test-only and say that it allocates.

### Conditions

- **C1 (A2).** Fold the one-TLS hint (`.scratch/arch-x31/onetls/strata/core/hazard.hh:270-285`), then rerun `shared_reader_registry` at 1 and 8 threads and put the numbers in the ledger. This is not a gate. Drop it if the 8-thread run shows no gain.
- **C2 (A5).** R4.8 and D10's registry row name 4096's mechanism: per-index chunk memory and scan time under a runaway, beneath the host's admission. Give the bytes for x86_64 and Apple arm64.
- **C3 (A3, A5).** Before Xapiand bumps its strata pin, `StrataReaders::claimed()` calls the new `claimed()`. Xapiand keeps `readers_per_index` and its gate until X3-2's admission cap exists, then sets `reader_records` from that cap.

Not run: the branch's tests, litmus and TSan stress (the ledger's results are taken as given); 8-thread and arm64 timing; Linux and FreeBSD; any Xapiand build or test.
