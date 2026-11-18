# C1: the field at 10^4 and 10^5 small tenants on one node

Fit condition C1 of `docs/design/hybrid-search.md` (branch `design/hybrid`). No strata code runs here.
Question: how do Qdrant, LanceDB, pgvector and Elasticsearch behave with 10,000 and 100,000 small tenants
on one node, in their own multi-tenant shapes?

## Setup

- Strata Linux VM: x86_64, 8 vCPU, 16 GiB, Docker 29.1.3. Every phase ran under `quiet exclusive` (one 40-minute hold per batch).
- Data: 1,000,000 random unit vectors, 768-d, float32 (one seeded file, shared by every system). Public embeddings were not used.
- Tenant sizes are uniform, not Zipf. 44 GiB of free disk cannot hold 10^5 x 1,000 x 768-d, so the total is held at 1M vectors:
  - 10,000 tenants x 100 vectors.
  - 100,000 tenants x 10 vectors.
  - The per-tenant fixed cost is therefore measured with the same data volume at both counts. Tenants of 1,000 vectors were not run at 10^4 (31 GB raw).
- Query: top-10 inner product, one random unit query vector, a random tenant, one sequential HTTP/driver client, 1,000 queries.
  - Cold: first touch of 1,000 distinct tenants, right after a container restart and a page-cache drop.
  - Warm: the same 1,000 queries again; pass 2 is shown. Pass 1 matched pass 2 in every row except where noted.
  - If loading stopped early, the 1,000 queries sample only the tenants that loaded (n shown).
  - Latency is client-side and includes about 1 ms of Python and HTTP.
- Memory comes from the container cgroup (v2) after load, a page-cache drop and 30 s idle, before any restart.
  - "anon" is the process's own memory. "rss" is anon plus mapped file pages.
  - "after restart" is the same measurement after `docker restart` plus 30 s. Qdrant's mmap-everything behaviour shows up as page cache there.
  - LanceDB is embedded, so its figure is the RSS of a fresh Python process. A bare `import lancedb` costs 135 MB.
- Disk is `du` allocated bytes.
- Defaults everywhere, plus the documented multi-tenant settings named per row. Each row was a single run (no repeats), so the spread is unknown.
- Versions: Qdrant 1.20.0, PostgreSQL 17.11 with pgvector 0.8.7, Elasticsearch 9.5.5 (default 8 GiB heap, `xpack.security` off, 1 shard, 0 replicas, default dense_vector index options), LanceDB 0.40.0 (flat scan, no index).
- Not run: Weaviate, OpenSearch and pgvectorscale (time box).

## Results at a glance

| System and shape | Tenants | Loaded | Load s | Idle anon MB | Idle rss MB | Disk MB | Cold p50 / p99 ms | Warm p50 / p99 ms | Limit hit |
|---|---|---|---|---|---|---|---|---|---|
| Qdrant collection per tenant | 10^4 | 920 | 130 | 191 | 937 | 994 | 4.3 / 8.5 | 2.8 / 5.5 | **"Too many open files"** at collection 955 |
| Qdrant collection per tenant | 10^5 | 0 | not run | | | | | | 10^4 failed at about 955 |
| Qdrant payload tenancy, defaults | 10^4 | 4,400 | 165 | 86 | 1,421 | 1,410 | 2.7 / 4.0 | 2.8 / 4.0 | disk guard (<6 GiB free) |
| Qdrant payload tenancy, defaults | 10^5 | 43,000 | 328 | 82 | 1,380 | 1,405 | 3.1 / 6.0 | 2.8 / 4.0 | disk guard (<6 GiB free) |
| Qdrant payload tenancy, bulk upload | 10^4 | 10,000 | 336 | 114 | 3,077 | 3,161 | 2.8 / 3.9 | 2.7 / 3.9 | none |
| Qdrant payload tenancy, bulk upload | 10^5 | 100,000 | 698 | 117 | 3,066 | 3,159 | 2.9 / 5.2 | 2.8 / 4.1 | none |
| pgvector, one shared table + btree(tenant) | 10^4 | 10,000 | 40 | 7 | 164 | 5,111 | 3.9 / 5.6 | 1.6 / 2.1 | none |
| pgvector, one shared table + btree(tenant) | 10^5 | 100,000 | 78 | 7 | 146 | 5,113 | 2.6 / 4.0 | 1.2 / 1.9 | none |
| pgvector, table per tenant + HNSW | 10^4 | 10,000 | 195 | 6 | 146 | 9,250 | 5.6 / 10.8 | 1.8 / 2.2 | none |
| pgvector, table per tenant + HNSW | 10^5 | 100,000 | 464 | 17 | 173 | 12,828 | 6.7 / 10.8 | 1.5 / 2.0 | none (needs `--shm-size=1g`, see below) |
| pgvector, table per tenant, no index | 10^4 | 10,000 | 58 | 7 | 147 | 5,258 | 4.0 / 6.0 | 1.6 / 2.5 | none |
| pgvector, table per tenant, no index | 10^5 | 72,000 | 184 | 13 | 152 | 6,716 | 4.4 / 8.5 | 1.2 / 2.1 | crash at table 72,012, container 64 MB `/dev/shm` |
| Elasticsearch index per tenant (default limit) | 10^4 | 980 | 136 | 8,884 | 8,974 | 1,112 | 9.3 / 25.3 | 4.9 / 6.7 | **1,000-shard limit** |
| Elasticsearch index per tenant, limit raised to 40,000 | 10^4 | 3,700 | 576 (time cap) | 8,940 | 9,305 | 2,516 | 11.6 / 25.2 | 8.4 / 17.1 | time cap, still creating at about 6.4 indexes/s |
| Elasticsearch shared index (filtered kNN) | 10^4 | 10,000 | 502 | 8,848 | 9,728 | 4,957 | 15.5 / 31.7 | 9.6 / 11.8 | none |
| Elasticsearch shared index (filtered kNN) | 10^5 | 100,000 | 553 | 8,867 | 10,178 | 4,731 | 12.8 / 25.9 | 8.8 / 11.1 | none |
| LanceDB table per tenant | 10^4 | 10,000 | 72 | n/a | 146 (fresh process) | 3,242 | 13.7 / 17.5 | 8.9 / 12.0 | none (40,000 files) |
| LanceDB table per tenant | 10^5 | 100,000 | 684 | n/a | 168 (fresh process) | 5,862 | 16.8 / 19.9 | 11.6 / 14.6 | none (400,000 files; `table_names()` 2.8 s) |

Every query returned the expected number of hits (bad = 0 in all rows).

## Per system

### Qdrant

- **Collection per tenant** is not viable on one node.
  - It stopped at 920 collections (the 955th create failed): `PUT /collections/t955 -> 500 {"status":{"error":"Service internal error: IO Error: Too many open files (os error 24)"}}`.
  - Creation ran at about 0.14 s per collection with 4 client threads.
  - The 920 loaded collections reserve 314 GB of apparent (sparse) disk, 994 MB actually allocated.
  - A restart took 107 s, and afterwards the cgroup held 14.4 GB of mapped files (the whole 16 GiB guest) with 137 MB anon.
  - No 10^5 run was made: the limit at about 955 is far below it.
- **Payload tenancy** (one collection, `hnsw_config {m: 0, payload_m: 16}`, a keyword index on `tenant` with `is_tenant: true`):
  - Documented bulk upload (`indexing_threshold: 0` during load, then 20000 and wait for green) loads 10^4 and 10^5 tenants cleanly.
  - After a restart it idles at 107 to 110 MB anon (145 to 148 MB rss).
  - p99 is 3.9 to 5.2 ms cold and 4.1 ms warm at both counts. Cold and warm differ little because each tenant's vectors are one small payload-indexed block.
  - Plain upserts with default optimizer settings left 25 GB of segments for 1.4 GB of data mid-load (the optimizer churning), so the free-space guard stopped them at 4,400 and 43,000 tenants.
  - The idle-after-load rss of about 3 GB is page cache from mapped segments, not anon (114 to 117 MB).
  - Disk after settling is 3.2 GB for 3.1 GB of raw vectors.

### pgvector

- One shared table with `btree(tenant)` is the cheapest shape by memory: 7 MB anon, 15 MB after restart, 5.1 GB on disk.
  - Queries filter by tenant and sort exactly over 10 or 100 rows.
  - p99 is 5.6 ms cold and 2.1 ms warm at 10^4, and 4.0 and 1.9 ms at 10^5.
- Table per tenant with an HNSW index (3 relations each, so 300,000+ files at 10^5):
  - Loads 100,000 tables in 464 s.
  - Disk is 12.8 GB, 2.5 times the shared table.
  - Idle after restart is 16 MB rss.
  - Cold p99 is 10.8 ms (relcache and file open on first touch), warm p99 2.0 ms.
- The first no-index 100,000-table run died at table 72,012: `could not resize shared memory segment ... No space left on device`, then a backend segfault. Cause: Docker's default 64 MB `/dev/shm`, not PostgreSQL. With `--shm-size=1g`, the indexed run reached 100,000. The no-index run was not repeated.
- Default `shared_buffers` (128 MB) was used and not tuned. A cold restart on 100,000 tables took 7 s.

### Elasticsearch

- Index per tenant stopped at 980 indexes, the default `cluster.max_shards_per_node`: `this action would add [1] shards, but this cluster currently has [1000]/[1000] maximum normal shards open`.
- With the limit raised to 40,000 (an unsupported setting), indexes were created at about 6.4/s. The 576 s time cap was reached at 3,700.
  - The restart then took 431 s to go green. The 10^4 target would have needed about 26 minutes of load plus a slow restart.
- The shared index (filtered kNN, `num_candidates` 50) works at both counts.
  - Load took 502 s at 10^4 and 553 s at 10^5.
  - p99 is 31.7 ms cold and 11.8 ms warm at 10^4, and 25.9 and 11.1 ms at 10^5.
- The default heap is 50% of RAM, so the JVM takes 8.5 GiB before any data. Anon is 8.8 to 8.9 GB at idle in every Elasticsearch row, so memory per tenant is not visible beyond that floor.

### LanceDB

- A table per tenant loads at both counts, with no hard limit hit.
  - Per-tenant files: about 4 (40,000 and 400,000 files total).
  - Latency is the highest of the survivors except Elasticsearch: p99 17.5 ms cold and 12.0 ms warm at 10^4, 19.9 and 14.6 ms at 10^5, with warm queries reusing opened table handles.
  - The Python process grew to about 195 to 210 MB after 1,000 queries over 1,000 distinct tables (including the 135 MB import).
- Flat scan, no index. `list_tables` for 100,000 tables took 2.8 s.
- Memory is the process's, not a server's. There is no per-tenant resident cost until a table is opened.

## Reading it against C1

- C1 asks whether strata at 10^5 tenants can show a lower RSS and p99 than each system in its own recommended multi-tenant setup. Some of these baselines are low:
  - pgvector shared table: 7 MB anon, 2 ms warm p99.
  - Qdrant payload tenancy: 117 MB anon, 4 ms.
  - LanceDB: about 170 MB of process.
- Only the per-tenant-object shapes break: Qdrant collections (at about 955), Elasticsearch indexes (at 1,000 by default), and PostgreSQL tables in a container with default shm.
- The comparison is at 10 vectors per tenant at 10^5. At 1,000-vector tenants the same systems (and strata) behave differently. This run did not test that size at scale.
- It is a single node on one VM, uniform sizes, random vectors (no recall check) and an HTTP client at about 1 ms of overhead.

## Caveats

- The qdrant-payload default-settings rows are truncated by a disk guard. The optimizer's transient 25 GB use was seen, but its steady state was not measured.
- A time cap or disk guard truncates rows marked as such, so those "Loaded" counts are not limits of the system.
- Single runs; no A/A spread. Cache-drop and restart ordering are identical for all systems.
- The harness and raw JSON are in the untracked `.scratch/c1` directory of this worktree, not committed.
