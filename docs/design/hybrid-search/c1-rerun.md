# C1 rerun: strata beside the field at 100 and 1,000 tenants of 1,000 vectors

Fit condition C1 of `docs/design/hybrid-search.md`, rerun with strata included (owner, D8). The
first run without strata, at 10 and 100 vectors a tenant, is `docs/perf/c1-field-tenants.md`.
Raw JSON per row is in `c1-rerun/`; the harness is `bench/hybrid/` (`c1.py`, `c1_strata.cc`,
`vm.sh`).

## Setup

- Linux VM (document-gate's image): x86_64, 8 vCPU, 16 GiB, Ubuntu 24.04, kernel 6.8, on an
  i9-9980HK host. Each row is one `exclusive` campaign job; each load a `light` job before it.
- Data: 1,000 tenants × 1,000 random unit vectors, 768-d float32, one seeded file; 1,000 queries,
  top-10 inner product, each to a random tenant. Uniform tenant sizes.
- Versions: Qdrant 1.20.0, PostgreSQL 17.12 with pgvector 0.8.7, Elasticsearch 9.5.5 (8 GiB heap),
  in Docker; LanceDB 0.40.0 and strata embedded.
- Shapes, each C1's working one:
  - Qdrant: one collection, payload tenancy, bulk upload.
  - pgvector: one shared table with `btree(tenant)`, exact.
  - Elasticsearch: one shared index, filtered kNN.
  - LanceDB: a table per tenant, flat.
  - strata: an index per tenant behind a `catalog`, each vector a document's stored value, an
    exact scan. Main has no vector code; this is the engine as it stands.
- Recall: each ANN system's search parameter is tuned at load to recall@10 ≥ 0.95 on 200 queries.
  Qdrant meets it at `hnsw_ef` 128 (1.0). Elasticsearch's default `bbq_hnsw` stops at 0.63 for any
  `num_candidates`; `rescore_vector` oversample 20 at `num_candidates` 1,000 reaches 0.975. The
  others are exact (1.0). Every pass records recall@10; every row passes.
- A row: the service started on the loaded data, 30 s idle, memory and idle CPU, the page cache
  dropped, 1,000 queries cold, the same 1,000 twice warm, 30 s idle, memory again.
- Memory: a container's cgroup v2 (anon, and rss as anon plus mapped file pages); `/proc`
  (RssAnon, VmRSS) for the embedded engines. Idle CPU is CPU seconds over the 30 s.
- Latency: services are timed at the Python client (HTTP or libpq on localhost, about 1 ms of
  overhead per C1); strata and LanceDB inside their process.

## Results

Latency in ms; "warm" is the second warm pass, C1's convention, with the first warm pass's p99
beside it. Idle is measured right after start, then again after the queries.

| System | Tenants | Load s | Disk MB | Idle anon MB | Idle rss MB | Idle CPU s / 30 s | Cold p50 / p99 | Warm p50 / p99 (warm1 p99) | Idle anon after queries MB | Job (row) |
|---|---|---|---|---|---|---|---|---|---|---|
| strata | 100 | 5.2 | 297 | 3.4 | 7.6 | 0.01 | 0.82 / 11.9 | 0.95 / 1.26 (1.08) | 4.9 | 261118-002831-fab1cd |
| pgvector | 100 | 8.4 | 761 | 4.3 | 31.7 | 0.11 | 5.05 / 15.8 | 4.97 / 5.58 (5.65) | 5.9 | 261118-002859-39b455 |
| Qdrant | 100 | 95.5 | 368 | 45.9 | 427.8 | 0.10 | 2.62 / 4.02 | 2.59 / 3.92 (5.80) | 47.1 | 261118-044446-7f14e4 |
| LanceDB | 100 | 4.7 | 296 | 71.5 | 138.3 | 0.00 | 10.0 / 15.4 | 10.0 / 12.9 (14.0) | 95.3 | 261118-002847-822bcf |
| Elasticsearch | 100 | 107.2 | 311 | 8,802 | 9,147 | 28.4 (startup), 0.52 after | 14.5 / 51.9 | 5.61 / 9.60 (10.0) | 8,929 | 261118-044448-81dd25 |
| strata | 1,000 | 30.4 | 2,969 | 3.4 | 7.8 | 0.02 | 8.29 / 12.9 | 0.79 / 1.32 (1.39) | 16.9 | 261118-044452-9ca15f |
| pgvector | 1,000 | 96.7 | 5,062 | 4.3 | 31.8 | 0.05 | 12.6 / 20.1 | 5.30 / 7.38 (6.47) | 5.8 | 261118-044458-428db2 |
| LanceDB | 1,000 | 52.2 | 2,961 | 71.6 | 138.5 | 0.00 | 16.5 / 21.4 | 10.2 / 14.0 (18.0) | 109.6 | 261118-044455-07f613 |
| Qdrant | 1,000 | 979.9 | 3,171 | 103.6 | 3,129 | 0.09 | 3.18 / 4.69 | 3.04 / 8.20 (4.10) | 105.6 | 261119-173419-733f81 |
| Elasticsearch | 1,000 | 1,139.0 | 3,118 | 8,787 | 9,376 | 22.8 (startup), 15.8 after | 24.7 / 66.5 | 9.57 / 11.9 (14.3) | 8,972 | 261119-173425-b0afb9 |

Commits: strata 100 at strata:ed42f792, strata 1,000 at strata:3e8dcb93 (the harness changes
between them touch Qdrant and Elasticsearch only); Qdrant and Elasticsearch at 1,000 at
strata:7b02184a, recall@10 1.0 and 0.973 (Elasticsearch's ladder again stops at 0.62 without
`rescore_vector`); the rest at the commit of their job.

## Reading

1. Warm p99: strata leads every system at both counts. At 100 tenants it is 3.1× faster than
   Qdrant (2.3× after crediting Qdrant the 1 ms of client overhead), 4.4× faster than pgvector,
   7.6× faster than Elasticsearch and 10.2× faster than LanceDB. At 1,000 tenants it is 3.1×
   faster than Qdrant against its better warm pass (4.10 ms; 6.2× against the second, 8.20 ms),
   5.6× faster than pgvector, 9.1× faster than Elasticsearch and 10.6× faster than LanceDB.
2. Idle cost: strata's resident anon is 3.4 MB at both counts and grows to 16.9 MB after
   touching 1,000 tenants. pgvector is at parity (4.3 to 5.9 MB). At 1,000 tenants Qdrant holds
   30× more anon plus 3,026 MB of mapped segments (its whole collection; rss 405× strata's),
   LanceDB's process 21×, Elasticsearch's heap 2,560×. Idle CPU is at the noise floor for
   strata, LanceDB and pgvector, 0.06 to 0.10 s per 30 s for Qdrant; Elasticsearch spends 0.5 s
   at 100 tenants and 15.8 s at 1,000 (half a core, idle after the queries).
3. Cold p99: strata beats pgvector (1.3× and 1.6× faster), LanceDB (1.3× and 1.7×) and
   Elasticsearch (4.4× and 5.2×), but Qdrant is 3.0× faster than strata at 100 tenants and 2.7×
   at 1,000. Qdrant's "cold" is not cold: it maps its segments at start (rss 3,129 MB at 1,000
   before any query), and `drop_caches` does not evict mapped pages, so its resident collection
   is the price of that p99. strata's cold query reads the tenant's 3 MB of `f32` stored values
   (p50 8.3 ms at 1,000 tenants, where nearly every query is a first touch).
4. Disk and load: strata stores the raw bytes (2,969 MB for 2,930 MB of vectors) and loads
   1,000 tenants in 30 s, 1.7× faster than LanceDB and 3.2× faster than pgvector.

## Not measured, and why

1. 10^4 and 10^5 tenants of 1,000 vectors: 30 and 300 GB of raw vectors per system against
   46 GB free in the VM.
2. Qdrant truly cold: its segments are mapped at start and survive `drop_caches`, so no row
   measures Qdrant answering from disk, nor any system under a memory limit that forces it to.
3. `bit1` codes and a cheaper per-tenant cold open (A2's next levers, below): estimated, not run.

## A2: the code column read cold (`i8`)

The one optimization run: D3's `i8` code in place of the `f32` stored value, each vector a
772-byte document value (an `f32` scale and 768 `i8` codes) scored against the `f32` query, with
no rerank (bench arm `strata-i8`, added at strata:7b02184a; the `f32` arm is main's storage,
unchanged). Before and after are both at strata:7b02184a, one VM chain, loads and rows
alternating; raw JSON in `c1-rerun/a2/`.

| Tenants | Arm | Load s | Disk MB | Idle anon MB | Cold p50 / p99 | Warm p50 / p99 (warm1 p99) | Recall@10 | Anon / rss after MB | Job (row) |
|---|---|---|---|---|---|---|---|---|---|
| 100 | `f32` (before) | 3.6 | 297 | 3.4 | 0.79 / 11.4 | 0.81 / 1.07 (0.99) | 1.0 | 4.9 / 303 | 261119-173357-5a1aac |
| 100 | `i8` (after) | 2.0 | 76 | 3.4 | 0.50 / 5.81 | 0.50 / 0.72 (0.63) | 0.989 | 4.6 / 84 | 261119-173351-6dabcb |
| 1,000 | `f32` (before) | 32.1 | 2,969 | 3.4 | 7.80 / 10.8 | 0.80 / 0.91 (0.92) | 1.0 | 16.9 / 2,982 | 261119-173414-b5197c |
| 1,000 | `i8` (after) | 42.9 | 758 | 3.4 | 3.71 / 5.74 | 0.49 / 0.69 (0.84) | 0.990 | 14.2 / 757 | 261119-173402-06e84b |

1. Cold p99: 2.0× faster at 100 tenants and 1.9× at 1,000; cold p50 at 1,000, nearly every
   query a first touch, 2.1× faster. Against Qdrant's mapped "cold" in the same chain (4.69 ms at
   1,000) `i8` is 1.2× slower (1.6× net of Qdrant's 1 ms of client overhead), from 2.7× before.
   It is now 3.5× faster than pgvector, 3.7× than LanceDB and 11.6× than Elasticsearch.
2. Warm p99: 1.5× and 1.3× faster; at 1,000 tenants 6.0× faster than Qdrant's better warm pass
   and 10.8× faster than pgvector.
3. Bytes: disk and the pages a query touches fall 3.9× (rss after the queries 757 MB against
   2,982 MB); idle anon is unchanged at 3.4 MB.
4. Cost: recall@10 0.989 to 0.990 without a rerank (D4 reranks from `vector_full` to reach
   T1's ≥ 0.999); load 1.3× slower at 1,000 tenants from the scalar quantizer (1.8× faster at 100,
   within this harness's load noise).
5. Where the cold time goes: bytes fell 3.9× but cold time only 1.9×. A two-point fit over the
   1,000-tenant rows gives about 2.2 ms per MB read plus a fixed 4.1 ms per cold tenant at p99
   (1.8 ms per MB plus 2.3 ms at p50): the fixed part, opening a tenant's index from disk, is now
   70% of `i8`'s cold p99. `bit1` codes (104 bytes a vector, D3b) would leave about 4.3 ms p99
   and 2.5 ms p50 by the same fit, Qdrant's level with nothing resident; a clear cold lead
   needs the per-tenant open cut too. Both are estimates from two points, not measured.

Variance: the `f32` arm here against A1's rows at strata:3e8dcb93 (1,000 tenants: cold p99
10.8 against 12.9 ms, warm p99 0.91 against 1.32 ms) shows run-to-run and main-merge drift of
15 to 30% on p99; the before and after above share a commit and a chain, A1's rows do not.

## Idle cost in memory and dollars

At an assumed $4 per GiB-month of RAM (the figure `docs/perf/idle-index.md` uses; an
assumption, not a quote) and $0.04 per core-hour, for 1,000 tenants of 1,000 vectors (1M
vectors, 2.9 GB raw) on one host:

| System | Resident while idle | Monthly | What it buys |
|---|---|---|---|
| strata | 3.4 MB anon | $0.01 | Cold p99 5.7 ms (`i8`) to 10.8 ms (`f32`) from disk |
| pgvector | 4.3 MB anon, 32 MB rss | $0.02 to $0.12 | Cold p99 20.1 ms |
| LanceDB | 72 MB anon | $0.28 | Cold p99 21.4 ms |
| Qdrant | 104 MB anon plus 3,026 MB mapped | $0.40, $12.20 with the mapped collection | Cold p99 4.69 ms while the collection stays resident |
| Elasticsearch | 8,787 MB heap; 0.53 core idle after queries | $34.30 plus $15.40 of CPU | Cold p99 66.5 ms |

The mapped pages are clean and reclaimable, so Qdrant's figure is what its p99 costs, not a
floor it cannot drop below; how its p99 behaves once reclaimed is not measured (above). The
memory a system needs grows with the data for Qdrant's mapped collection and with the heap
setting for Elasticsearch; strata's 3.4 MB is its process floor, and per-index idle cost is
`docs/perf/idle-index.md`'s.

## Failures in this run's history

| Job | What | Cause |
|---|---|---|
| 261118-000128-f4c57b | First dry run, every system at 100 | Ran on macOS, not in the VM. Qdrant's connection reset (fixed by resending idempotent requests, strata:a950f1a4); Elasticsearch recall 0.5655 from C1's default `bbq_hnsw` (fixed by the tuning ladder, strata:3e8dcb93). Not evidence. |
| 261118-002809-0a06e7 | macOS dry run, every system at 100 | Qdrant 409: a Qdrant left by the cancelled dry run held the port, so the load talked to it. Fixed at strata:72990a82 (idempotent load, busy-port refusal, `C1_PORT_OFFSET`). Not evidence. |
| 261118-044500-19572d | VM load, Qdrant at 1,000 | The VM was powered off from outside the job 5.5 minutes into the load (exit 255): the console shows a clean systemd shutdown with no OOM or panic while the job's SSH session was open, and no campaign job drove the VM then. Elasticsearch's load and both rows (261118-044501-c7b37f, 261118-044503-2593f7, 261118-044504-13047d) were skipped. Rerun at strata:7b02184a: Qdrant loaded in 980 s (261119-173416-dd03e0), Elasticsearch in 1,139 s (261119-173422-9a6925). |

Also timed out: 261118-002908-5c7ce8, the first VM Qdrant load at 100, hit its 12-minute bound
with no log; the rerun at 20 minutes (261118-044444-9eed59) loaded in 95.5 s.
