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
| Qdrant | 1,000 | not measured (denied) | | | | | | | | load 261118-044500-19572d failed; rerun refused by the permission classifier |
| Elasticsearch | 1,000 | not measured (denied) | | | | | | | | jobs 261118-044501-c7b37f, 261118-044503-2593f7, 261118-044504-13047d skipped after Qdrant's |

Commits: strata 100 at strata:ed42f792, strata 1,000 at strata:3e8dcb93 (the harness changes
between them touch Qdrant and Elasticsearch only); the rest at the commit of their job.

## Reading

1. Warm p99: strata leads every system at both counts. At 100 tenants it is 3.1× faster than
   Qdrant (2.3× after crediting Qdrant the 1 ms of client overhead), 4.4× faster than pgvector,
   7.6× faster than Elasticsearch and 10.2× faster than LanceDB. At 1,000 tenants it is 5.6×
   faster than pgvector and 10.6× faster than LanceDB.
2. Idle cost: strata's resident anon is 3.4 MB at both counts and grows to 16.9 MB after
   touching 1,000 tenants. pgvector is at parity (4.3 to 5.9 MB); Qdrant holds 13× more anon
   plus 382 MB of mapped segments, LanceDB's process 21×, Elasticsearch's heap 2,600×. Idle CPU
   is at the noise floor for strata, LanceDB and pgvector, 0.06 to 0.10 s for Qdrant, 0.5 s for
   Elasticsearch.
3. Cold p99: strata beats pgvector (1.3× and 1.6× faster), LanceDB (1.3× and 1.7×) and
   Elasticsearch (4.4×), but Qdrant is 3.0× faster than strata at 100 tenants. Qdrant's
   "cold" is not cold: its segments stay mapped, and `drop_caches` does not evict mapped pages,
   so its 382 MB idle rss is the price of that p99. strata's cold query reads the tenant's 3 MB
   of `f32` stored values (p50 8.3 ms at 1,000 tenants, where nearly every query is a first touch).
4. Disk and load: strata stores the raw bytes (2,969 MB for 2,930 MB of vectors) and loads
   1,000 tenants in 30 s, 1.7× faster than LanceDB and 3.2× faster than pgvector.

## Not measured, and why

1. Qdrant and Elasticsearch at 1,000 tenants: the VM was powered off from outside the job 5.5
   minutes into Qdrant's load (job 261118-044500-19572d, exit 255). The VM console shows a clean
   systemd shutdown with no OOM or panic, starting while the job's SSH session was open; no
   other campaign job drove the VM in that window. Elasticsearch's load and both rows were then
   skipped by the chain.
2. 10^4 and 10^5 tenants of 1,000 vectors: 30 and 300 GB of raw vectors per system against
   46 GB free in the VM.
3. The A2 optimization experiments (an `i8` code column read cold instead of `f32` stored
   values, among others) are not run here.

## Failures in this run's history

| Job | What | Cause |
|---|---|---|
| 261118-000128-f4c57b | First dry run, every system at 100 | Ran on macOS, not in the VM. Qdrant's connection reset (fixed by resending idempotent requests, strata:a950f1a4); Elasticsearch recall 0.5655 from C1's default `bbq_hnsw` (fixed by the tuning ladder, strata:3e8dcb93). Not evidence. |
| 261118-002809-0a06e7 | macOS dry run, every system at 100 | Qdrant 409: a Qdrant left by the cancelled dry run held the port, so the load talked to it. Fixed at strata:72990a82 (idempotent load, busy-port refusal, `C1_PORT_OFFSET`). Not evidence. |
| 261118-044500-19572d | VM load, Qdrant at 1,000 | The VM was powered off from outside the job (above); not the harness or an OOM. |

Also timed out: 261118-002908-5c7ce8, the first VM Qdrant load at 100, hit its 12-minute bound
with no log; the rerun at 20 minutes (261118-044444-9eed59) loaded in 95.5 s.
