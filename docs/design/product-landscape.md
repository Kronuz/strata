> Status: CURRENT (2026-10-27) revision 0 of the product landscape; the AI steer is conditional on fit (owner).

# Elysian: product landscape and gap analysis

Revision 0 (2026-10-27). State: designing (ready for preliminary review). Reads strata
`98ce743`, prospect `9de01a1`, queryparser `09a4ea6`, Xapiand `da26127a3`. Web sources were read
on 2026-10-27; each cell cites one by ID (the [Sources](#sources) table, with dates). No GPL
source was opened; Xapiand's own MIT docs were.

Elysian is Xapiand rebuilt on strata (the segment core) and prospect (the search library).
The owner's direction (2026-10-27): "let's not lock us in what Xapiand had": match and
surpass Elasticsearch, Quickwit, and their peers wherever that is an improvement.

## Summary

1. **Where the field is weakest is where our workload lives.** No self-hosted engine
   documents more than tens of thousands of indexes per node: Elasticsearch caps 1,000
   shards per node [es9], Weaviate measured 18,000 to 19,000 active tenants per node
   [wv6], and Meilisearch, Typesense, Qdrant, and Algolia all steer tenants into one shared
   index [me12, ts10, qd8, al8]. Only hosted, closed Turbopuffer claims "unlimited (seen
   250M+)" namespaces [tp4], and Vespa's streaming mode gives up ANN and stemming for it
   [v9]. strata targets a million indexes per host, but has measured only 1,000 (D1).
2. **We lack four things every serious product has:** vector and hybrid retrieval,
   client authentication with tenant-scoped keys, typo-tolerant and prefix matching, and
   official client libraries (section 2).
3. **Of six candidate differentiators** (the brief's five plus indexing cost), **three hold
   on evidence today**: indexing cost, memory against JVM and RAM-resident engines, and the
   license against the app-search engines. **Two are designed but unbuilt** (per-index cost,
   consensus), and **one is not unique** (real-time reads: Vespa, Manticore, and ParadeDB
   already have them) (section 3).
4. **Recommendation:** position Elysian as the open, self-hosted engine for
   index-per-tenant search (application and SaaS search with millions of small indexes,
   each real-time, scored, filtered, and aggregated), not as a log-analytics engine. Prove
   the per-index row first (D1), then close the table stakes in the order of section 5.

## 1. The landscape

Kept from the brief: every product listed. Added: SeekStorm (Apache-2.0 Rust engine with
BM25F, vectors, multi-tenant server [ss1]). Mentioned only for their tenancy limits:
Pinecone (100,000 namespaces on Standard, 1,000,000 on Enterprise [pc1]) and Azure AI
Search (50 to 3,000 indexes per service by tier [az1]). Dropped:

| Product | Why dropped |
|---|---|
| ZincSearch | Dormant: its team moved to OpenObserve; a community maintainer took over in 2026-09 [zn1] |
| Sonic | Returns ids only, no ranking, filters, or replication [sn1]; not a search server in the same sense |
| Bleve, Marqo, Vald, Coveo, Vertex AI Search | Go library, vector layers, or managed enterprise SaaS; not engine peers (unverified: not fetched) |

"—" means the dimension was not surveyed for that product (vector databases: search
features only). "n.d." means the cited page documents no figure.

### Table A: search features

| Product | Query language | Relevance: lexical / LTR / vector and hybrid | Aggregations | APIs and SDKs | License |
|---|---|---|---|---|---|
| Elasticsearch 9.5 | Query DSL, ES\|QL [es1] | BM25 default [es2]; GBDT LTR, paid [es3]; HNSW, DiskBBQ, RRF and linear retrievers [es4, es5] | Metric, bucket, pipeline [es6] | REST; 8 official clients [es7] | AGPL-3.0, SSPL, or ELv2 [es8] |
| Elastic Serverless | Same, minus scripted metrics and `join` [sl1] | Same as the stack [sl2] | Same, minus scripted metrics [sl1] | Same clients; API keys only [sl1] | Hosted service |
| OpenSearch 3.9 | Query DSL, PPL with joins [os1] | Lucene BM25 (unverified); XGBoost and RankLib LTR [os2]; Faiss, Lucene, JVector ANN; normalization hybrid [os3, os4] | Metric, bucket, pipeline [os5] | REST; 9 clients [os6] | Apache-2.0 [os7] |
| Solr 10 | Lucene, eDisMax, JSON DSL, SQL [so1] | LTR: linear, trees, neural [so2]; HNSW with 1-, 4-, 7-bit quantization [so3]; RRF hybrid in 9.11 [so4] | JSON Facet API; exact distributed counts with `refine` [so5] | HTTP, SolrJ [so6] | Apache-2.0 [so7] |
| Vespa | YQL with grouping [v1] | BM25 k1 1.2, b 0.75 [v2]; LightGBM and XGBoost multi-phase [v3]; HNSW, int8, binarized [v4] | Grouping language [v1] | Document and query APIs, pyvespa, Java feed client [v5] | Apache-2.0 [v6] |
| Quickwit 0.9 | Own syntax; partial ES `_search` [qw1, qw2] | BM25 off by default [qw3]; no vectors documented (unverified) | Terms, histograms, percentiles; terms counts approximate [qw4] | REST, ES subset, OTLP, Kafka [qw2] | Apache-2.0 [qw5] |
| Tantivy 0.26 (library) | Query parser API [tv1] | BM25; no vectors [tv1] | Bucket aggregations [tv1] | Rust library [tv1] | MIT [tv1] |
| ParadeDB pg_search | SQL `@@@` [pd1] | BM25; RRF with pgvector [pd2] | Pushed down only with `@@@` [pd3] | SQL, ORMs [pd4] | AGPL-3.0, commercial EE [pd4] |
| Manticore 29 | SQL over MySQL protocol, JSON [mc1] | `bm25`, `proximity_bm25` rankers [mc2]; HNSW, RRF [mc3] | Facets, histograms [mc4] | MySQL clients, HTTP [mc5] | GPL-3.0+ [mc6] |
| SeekStorm 3.4 | AND, OR, phrase, NOT [ss1] | BM25F; IVF vectors; RRF [ss1] | Facets only [ss1] | REST, 4 SDKs, MCP [ss1] | Apache-2.0 [ss1] |
| Meilisearch 1.56 | Filter strings; typo tolerance on [me1, me2] | Rule-based buckets, not BM25 [me3]; embedder hybrid [me4] | Facet counts, min and max [me5] | REST; 8 official SDKs [me6] | MIT; sharding is EE (BUSL) [me7] |
| Typesense 30 | `filter_by`, typo tolerance [ts1] | Weighted text match [ts1]; HNSW, rank fusion [ts2] | Facet stats, `group_by` [ts1] | REST [ts1] | GPL-3.0 [ts3] |
| Algolia | Filter strings, facets [al1] | Tie-break rules, no BM25 [al2]; NeuralSearch on top tier only [al3] | Facet counts [al1] | 11 clients from OpenAPI [al4] | Proprietary (unverified) |
| Turbopuffer | JSON query API [tp1] | BM25 k1 1.2, b 0.75; ANN; RRF; rerank [tp1, tp2] | Count and sum, 8 per query [tp1] | REST; 8 languages [tp1] | Closed [tp3] |
| Qdrant 1.20 | Filters, full-text payload index [qd1] | BM25 on the text index (new in 1.20) [qd2]; RRF, DBSF [qd3] | Facet counts [qd4] | — | Apache-2.0 [qd5] |
| Weaviate | GraphQL, filters [wv1] | BM25, BlockMax WAND [wv1]; relative-score fusion [wv2] | Count, top, stats, group [wv3] | — | BSD-3 [wv4] |
| Milvus 3.0 | Expressions [mv1] | BM25 as sparse vectors [mv1]; RRF, weighted [mv2] | Facets new in 3.0 (vendor) [mv3] | — | Apache-2.0 [mv4] |
| LanceDB | SQL `where` [ld1] | BM25, no `OR`/`AND` in the string [ld2]; RRF [ld3] | n.d. [ld1] | — | Apache-2.0 [ld4] |
| OpenObserve | SQL, `match_all` [oo1] | No ranking documented [oo2] | Full SQL [oo1] | HTTP, gRPC, OTLP [oo3] | AGPL-3.0, commercial EE [oo3] |
| Loki 3.7 | LogQL [lk1] | None (grep-style) [lk1] | Metric queries [lk1] | HTTP, LogCLI [lk1] | AGPL-3.0 [lk2] |
| ClickHouse 26 | SQL `hasToken`, `hasPhrase` [ch1] | None: "not a relevance engine" [ch2] | Full OLAP [ch2] | SQL, HTTP [ch1] | Apache-2.0 [ch3] |
| Xapiand (Xapian) | JSON or MsgPack DSL; Xapian query strings [x1, x2] | BM25 (Xapian's) [x3]; no LTR, no vectors [x4] | Metrics and buckets; no date histogram [x5] | HTTP with custom verbs; no SDK [x6] | MIT; Xapian GPL [x7] |
| Elysian on strata | Same DSL; queryparser (Xapian dialect) [e1] | BM25, Lucene or Xapian presets [e2]; no vectors planned [e3] | Xapiand's set in Phase 4 [e4] | Same as Xapiand | MIT [e5] |

### Table B: system characteristics

| Product | Searchable after a write | Distribution and consistency | Storage and object tier | Many indexes (documented) | Operations | Where users say it hurts |
|---|---|---|---|---|---|---|
| Elasticsearch | 1 s refresh; realtime get by id [es10, es11] | Primary-backup; reads can see unacknowledged writes [es12] | LZ4 or zstd; searchable snapshots, Enterprise [es10, es13] | 1,000 shards per node; under 3,000 indexes per GB master heap [es9] | Heap ≤ 50% RAM and under ~26-30 GB [es14] | Oversharding heap pressure [es15]; JVM memory pressure [es16] |
| Elastic Serverless | 5 s refresh floor [es10] | Managed shards, no `_routing` [sl2] | Decoupled storage and cache [sl2] | 15,000 indexes per project [sl1] | Versionless; no snapshots [sl2] | Search compute never scales to zero [sl4] |
| OpenSearch | 1 s refresh [os8] | Primary-backup or segment replication; replicas lag [os9] | zstd codecs; remote store; searchable snapshots [os8, os11] | 1,000 shards per node [os12]; 25 per GiB heap (AWS) [os13] | Heap ~50% RAM [os14] | Shard-limit confusion [os15]; single-threaded cluster manager [os16] |
| Solr | Soft commit, no default [so8] | ZooKeeper leaders; "no hard guarantees" across replicas [so9] | HDFS removed in 10 [so6] | "Not designed for thousands of collections" [so10] | Java 21; 8-16 GB heap common [so11] | Overseer bottleneck [so12]; restarts at 5,000 collections [so13] |
| Vespa | Visible on acknowledgement [v7] | Buckets, no manual sharding; eventual, not linearizable [v7, v8] | Attributes in memory [v10] | Streaming mode: 45 B per document, exact search only [v9] | Memory hard to predict [v11] | Full memory blocks all feeding [v12] |
| Quickwit | Split commit, 60 s default [qw6] | Stateless searchers, Postgres metastore [qw7] | Splits on S3, GCS, Azure [qw7] | Failed at 1,027 indexes, since fixed [qw8] | 4 GB RAM per indexer core [qw9] | Owned by Datadog since 2025-01; recent releases security-only [qw10, qw11] |
| ParadeDB | At transaction commit [pd5] | Single primary; Citus for sharding [pd6] | Inside Postgres [pd7] | n.d. [pd6] | 1 GB build budget per process [pd8] | Index build halts standby replay [pd9] |
| Manticore | RT tables visible at once [mc7] | Galera multi-master, sharded tables [mc6] | Row or columnar; backup to S3 [mc6] | 128 MB RAM chunk per table by default [mc8] | ~40 MB empty (vendor) [mc6] | RSS 24.6 GB at a 1 GB limit [mc9]; vector results differ across nodes [mc10] |
| Meilisearch | Async task queue, no figure [me8] | Single node; EE leader-only writes [me9] | LMDB, mmap [me10] | No cap; ~8,000 indexes stalled listing (fixed) [me11]; per-tenant "not recommended" [me12] | Disk 10× raw data [me10]; upgrades by dump or `--upgrade-db` [me13] | Not built for real-time indexing (CTO) [me14] |
| Typesense | Synchronous writes [ts4] | Raft, full copy on every node, no sharding [ts5] | Whole index in RAM [ts6] | Per-collection fixed memory (maintainer, unverified) [ts10] | RAM 2-3× indexed fields [ts6] | Hours-long restarts (unverified) [ts7]; quorum loss stops all [ts5] |
| Algolia | Async task, seconds [al5] | Three-server clusters (unverified) [al6] | Index 2-3× raw, under 80% RAM [al7] | 1,000 indices on Premium [al8] | Fully managed [al8] | Metered cost per search and record [al3, al9] |
| Turbopuffer | Strong reads; over 128 MiB unindexed, up to ~1 h stale [tp5] | Serverless, up to 256 shards per namespace [tp4] | S3 with NVMe cache; cold ~400 ms+ [tp6] | Unlimited (seen 250M+) [tp4] | Nothing to run; no OSS [tp3] | Cold latency, slow writes [tp3, tp7] |
| Qdrant | `wait=true` waits for visibility [qd6] | Raft for topology only [qd7] | mmap vectors [qd1] | 1,000 collections per Cloud cluster; per-tenant collections discouraged [qd8] | — | Old BM25 path needed inference per document [qd2] |
| Weaviate | n.d. [wv5] | Raft for schema; leaderless data [wv5] | Tenants offloaded to S3 [wv7] | 18-19k active tenants per node measured; capped by open files [wv6] | — | 8 s BM25 on 1M documents, pre-1.30 [wv8] |
| Milvus | Strong to eventual levels [mv5] | Coordinator, etcd, WAL on object storage [mv6] | Object storage primary [mv7] | 65,536 collections [mv8]; under 10,000 advised [mv9] | — | Milvus Lite has no full-text [mv10] |
| LanceDB | New rows scanned until `optimize()` [ld2] | Embedded; enterprise cluster [ld5] | Object storage native [ld5] | n.d. [ld6] | — | FTS index not refreshing [ld7] |
| OpenObserve | Buffer queryable [oo4] | Stateless roles, relies on S3 [oo4] | Parquet on S3 [oo4] | n.d. [oo5] | Cache up to 50% RAM [oo5] | 49 s cold count (vendor) [oo6] |
| Loki | n.d. [lk3] | Microservices [lk3] | Chunks on object storage [lk4] | Per-tenant limits [lk5] | Blooms experimental [lk6] | "Not built for high cardinality" [lk7] |
| ClickHouse | Async inserts flush ≤ 200 ms [ch4] | — | Object storage; index ~45% of table [ch2] | n.d. [ch1] | GA in 26.2 [ch2] | Rows lost by the text index, open bugs [ch5] |
| Xapiand (Xapian) | Near real time [x7] | Raft for primaries; async pull replication loses acknowledged writes on primary crash [x8] | Local; Haystack blob volumes [x9] | No figure; fixed shard count [x10] | ~470 MB vs ES 1.4 GB on 20,000 documents [x11] | No client authentication [x12] |
| Elysian on strata | Designed: reads see acknowledged writes [e6] | Designed: consensus per shard, quorum acks [e7] | Designed: cold segments on object storage [e8] | Target 10^6 per host; measured 1,000 [e9, e10] | Indexing 3.49× Tantivy at 42% less RSS [e11] | Queries miss 1.0× on 39 of 40 classes [e12] |

## 2. Table stakes we lack

| Gap | Who has it | Elysian today |
|---|---|---|
| Dense vectors and hybrid fusion | ES, OpenSearch, Solr, Vespa, Manticore, ParadeDB, Meilisearch, Typesense, Turbopuffer, SeekStorm, and every vector database (Table A) | Nothing: no PLAN, DISTRIBUTION, or design file mentions vectors, kNN, HNSW, or embeddings [e3] |
| Client authentication, tenant-scoped keys | Meilisearch tenant tokens [me12], Typesense scoped keys [ts10], Algolia secured keys [al8], ES API keys [sl1] | "Anyone who can reach the port can use the API" [x12] |
| Typo tolerance, prefix and fuzzy expansion | On by default in Meilisearch, Typesense, Algolia [me2, ts1, al2] | Planned: automata (Phase 5) and expansion gap G8 [e1] |
| Official client libraries | 8 to 11 per product [es7, me6, al4] | None documented [x6] |
| Snapshots to object storage | ES, OpenSearch, Qdrant, Manticore [es13, os11, qd1, mc6] | MsgPack dump and restore only [x13] |

Aggregation depth (date histograms, percentiles, pipelines) is table stakes only against the
Lucene family; app-search engines stop at facets [me5, ts1, al1]. Learning to rank is not
table stakes: the Lucene family and Vespa ship it [es3, os2, so2, v3]; the app-search engines do not.

## 3. Differentiators, tested

| Candidate | Field evidence | Our evidence | Verdict |
|---|---|---|---|
| Per-index cost at millions of indexes | Self-hosted ceilings are 1,000 shards per node (ES, OpenSearch) to 18-19k tenants per node (Weaviate) [es9, os12, wv6]; Meilisearch, Typesense, Qdrant, Algolia say use one shared index [me12, ts10, qd8, al8]. Exceptions: Turbopuffer (hosted, closed, cold queries ~400 ms+) [tp4, tp6]; Vespa streaming (exact only, no stemming) [v9] | 1,000 indexes: 58% less RSS than RocksDB, 5 files each, creation 3.85× slower [e10]. The target (under 1 KB and no descriptor per idle index, 10^6 per host) is a note, not a design [e9] | **Strongest gap in the field, unproven by us.** D1 |
| Real-time reads without refresh | ES 1 s, Serverless 5 s, Quickwit 60 s, Turbopuffer up to ~1 h under backlog [es10, qw6, tp5]; but Vespa, Manticore, ParadeDB are visible on acknowledgement [v7, mc7, pd5] | Designed (write buffer, revision 4), step 1 landed; the multi-writer bake-off is queued [e6] | **Not unique.** Table stakes for index-per-tenant search; a differentiator only against the Lucene family and Quickwit |
| Memory | ES heap rules [es14]; Typesense and Algolia hold the index in RAM [ts6, al7] | One thread at 1M documents: 1,886 MB peak RSS against Tantivy's 3,247 MB [e11]; as a key-value store at 10× RAM, 1.6-2.1× RocksDB's RSS [e13] | **Holds against JVM and RAM-resident engines**; not yet against RocksDB |
| Consensus-backed distribution | ES reads can see unacknowledged writes [es12]; Solr: "no hard guarantees" [so9]; Vespa not linearizable [v8]; Qdrant and Weaviate use Raft for metadata only [qd7, wv5]; Typesense has Raft but no sharding [ts5] | Designed: per-shard groups, linearizable get by id at Phase 3R [e7]; not built | **Real gap, but users rarely cite it** in the pain columns. It matters if Elysian is a system of record (its key-value and blob indexes) |
| MIT license | Permissive peers abound: OpenSearch, Vespa, Solr, Quickwit, ClickHouse, Qdrant, Milvus (Apache-2.0), Weaviate (BSD-3) | MIT once Xapian is removed (Phase 8) [e5] | **Holds only against app-search peers**: Typesense GPL, Meilisearch's scale features BUSL, Algolia and Turbopuffer closed |
| Indexing cost (added) | Quickwit 7.5 MB/s per core [qw9]; Meilisearch slow bulk reindex [me14] | 3.49× Tantivy's throughput at one thread, 1M documents [e11] | **Holds, measured** (gate VM, Tantivy 0.26 as anchor) |

Query speed is not a differentiator yet: prospect misses 1.0× of the faster of Lucene and
Tantivy on 39 of 40 query classes in Phase 3's diagnostic rows [e12].

## 4. What Xapiand had that should go or change

| Xapiand feature | Field evidence | Recommendation |
|---|---|---|
| Custom HTTP verbs (`SEARCH`, `INFO`, `UPDATE`) [x6] | No other surveyed product was found using custom verbs (unverified: not searched per product); Xapiand itself documents a firewall workaround [x6] | Standard methods canonical; custom verbs removed at the Phase 8 rename (Q5) |
| Haystack blob volumes in the node [x9] | No surveyed engine documents a blob store (unverified: not searched per product); object storage holds bulk data (Turbopuffer, Quickwit, Milvus) [tp6, qw7, mv7] | Out of the search node, as DISTRIBUTION's open decision 1 already recommends [e8] |
| Fixed shard count at creation [x10] | Vespa never asks users to shard [v8]; oversharding is ES's top pain [es15] | No user-chosen shard count; split by size (Track D4) |
| Asynchronous pull replication [x8] | Loses acknowledged writes on a primary crash [x8] | Already replaced by quorum journals [e7] |
| Accuracy terms for numeric, date, geo ranges [x14] | Lucene-family engines use points and doc values (Table A) | Drop once Phase 4's `bkd` points land [e4] |
| Unbounded dynamic fields [x15] | Mapping explosion is an ES and OpenSearch pain [os10]; OpenObserve caps 1,000 fields per record [oo5] | Keep dynamic typing; add a per-index field cap and a strict mode |
| Xapian's BM25 constants as an option | Lucene parameters are 1.14-1.16× faster here [e2]; Vespa and Turbopuffer use k1 1.2, b 0.75 [v2, tp2] | Lucene preset for new indexes; Xapian's only for migrated ones |
| Shared (foreign) schemas [x16] | ES index templates; small indexes share schemas (strata note, item 6) [e9] | **Keep**: it is the per-index cost lever for many small indexes |

## 5. Proposals, ranked

Value is to an index-per-tenant user; cost is a size estimate (S, M, L as in
`xapiand-on-strata.md`), not a measurement.

| # | Decision proposed | Value | Cost | Touches |
|---|---|---|---|---|
| D1 | Prove per-index cost: measure idle open-index memory, descriptors, mappings, and open time at 1, 10, 100, 1,000 documents; then lazy open with an LRU, shared machinery, a small-segment profile | High: the only gap the field leaves open | M to measure, L to win | `perf/small-indexes`, format 13 structure flags, Xapiand `DatabasePool` |
| D2 | Authentication with HMAC-signed keys scoped to index prefixes and filters | High: blocks any shared deployment | S-M | Xapiand HTTP layer, beside the HTTPS D1 gate |
| D3 | Vectors: exact (flat) kNN per small index plus RRF fusion first; ANN for large indexes by bake-off later | High: table stakes everywhere | M exact, L ANN | prospect plugin and a reserved format 13 section kind (SYS-4's rule); Phase 4 |
| D4 | Visibility on acknowledgement as the API contract, with `ack`, `read`, `refresh` per request | High, already in flight | Done by the write buffer and 7b | write buffer steps 2-3; Phase 7b |
| D5 | Typo tolerance and prefix search on by default for text fields | Medium-high for app search | M | automata (Phase 5), queryparser, gap G8 |
| D6 | Cold indexes on object storage, opened by stateless searchers | High at 10^6 indexes, most idle | L | Track D6, Phase 6 residency |
| D7 | An Elasticsearch-compatible subset: `_bulk`, `_search` with match, bool, range, terms aggregations | Medium: migration and ingest tooling (Quickwit, Manticore, OpenObserve did it [qw2, mc5]) | M | Xapiand `query_dsl`, HTTP routes |
| D8 | Clients generated from an OpenAPI spec | Medium | S-M after D7 | API spec, new repos |
| D9 | Snapshots to S3-compatible storage | Medium | M | strata manifests, Track D6 |
| D10 | The section 4 removals: verbs, accuracy terms, Xapian BM25 default, field cap | Low-medium, mostly simplification | S each | Xapiand schema, Phase 8 |

First steps a developer can start now: D1's measurement (no format change, small-indexes
direction 1) and D2's design (Xapiand only, no strata dependency).

## 6. Gate rows

| Row | Target | Procedure |
|---|---|---|
| T1 idle index | Under 1 KB resident, zero descriptors, open under 100 µs (the note's working target) | Open N indexes of 1 to 1,000 documents, close to idle, read `phys_footprint` and `/proc/self/fd`; Linux and macOS |
| T2 many indexes | 10^6 indexes on one host within `vm.max_map_count`, with Elasticsearch, OpenSearch, Meilisearch, Weaviate at their documented ceilings beside it | Create, write one document each, query a random 1% under `quiet exclusive`; report RSS, files, p99 |
| T3 visibility | A search after an acknowledged write sees it, every time, with no refresh call | Write-then-search loop under concurrent writers; count misses (must be 0) and p99 |
| T4 memory against Elasticsearch | Equal data, equal queries, RSS ≤ 0.7× | Xapiand's `benchmarks.md` harness on Elysian |

## 7. Limits of this survey

- Agents read the web pages through a summarizing fetch; numbers are summary-level. Six
  load-bearing facts were re-fetched directly: Elasticsearch shard limits, Turbopuffer limits,
  Weaviate tenants, Vespa streaming, Quickwit's commit timeout, Solr's license.
- Vendor benchmarks are labeled vendor; no independent 2025-2026 BM25 comparison across
  engines was found.
- No product publishes a per-index or per-tenant memory figure, so section 3's ceilings
  are documented limits, not measured costs. T2 measures them.
- Silent risk: Turbopuffer and Vespa streaming already serve the many-tenant case. If T1
  lands far from its target, D1's position is weaker than section 3 implies, and nothing
  else in this document flags it.

## 8. Open questions for the owner

**Q1. Who is Elysian for?** (a) Index-per-tenant application and SaaS search; (b) a general
Elasticsearch replacement including logs; (c) both from the start. Recommendation: (a).
It matches the many-indexes workload and the field's weakest point; logs are crowded
(ClickHouse, Quickwit, OpenObserve, Loki) and reward columnar scans over per-index cost.

**Q2. Vector search in the first release?** (a) Exact kNN and RRF first, ANN later; (b) ANN
from the start; (c) not in scope. Recommendation: (a). Exact search is cheap per small
index (Vespa streaming's precedent [v9]) and needs no graph format.

**Q3. API shape.** (a) Xapiand's DSL redesigned as canonical, with an ES-compatible subset
as an adapter; (b) ES-compatible as canonical; (c) Xapiand's DSL only. Recommendation: (a).

**Q4. Query-string dialect.** (a) Xapian's for 7a, a Lucene dialect added after; (b) Lucene
only. Recommendation: (a): queryparser is built against Xapian oracle vectors for 7a [e1],
and Lucene syntax is what ES, Solr, and Quickwit users type [so1, qw1].

**Q5. Custom HTTP verbs.** (a) Remove at the Phase 8 rename; (b) keep. Recommendation: (a).

**Q6. A hosted tier.** (a) Keep the design serverless-capable (D6) without committing;
(b) commit now; (c) self-hosted only. Recommendation: (a).

## Sources

All accessed 2026-10-27 unless a page date is given. "Vendor" marks a vendor's own claim.

| ID | Source |
|---|---|
| es1 | [ES\|QL](https://www.elastic.co/docs/reference/query-languages/esql) |
| es2 | [Similarity](https://www.elastic.co/docs/reference/elasticsearch/mapping-reference/similarity) |
| es3 | [Learning to rank](https://www.elastic.co/docs/solutions/search/ranking/learning-to-rank-ltr) |
| es4 | [Retrievers](https://www.elastic.co/docs/solutions/search/retrievers-overview) |
| es5 | [Approximate kNN](https://www.elastic.co/docs/solutions/search/vector/knn/approximate-knn) |
| es6 | [Aggregations](https://www.elastic.co/docs/explore-analyze/query-filter/aggregations) |
| es7 | [Clients](https://www.elastic.co/docs/reference/elasticsearch-clients) |
| es8 | [LICENSE.txt](https://github.com/elastic/elasticsearch/blob/main/LICENSE.txt) |
| es9 | [Size your shards](https://www.elastic.co/docs/deploy-manage/production-guidance/optimize-performance/size-shards) (re-fetched) |
| es10 | [Index modules](https://www.elastic.co/docs/reference/elasticsearch/index-settings/index-modules) |
| es11 | [Get API](https://www.elastic.co/docs/api/doc/elasticsearch/operation/operation-get) |
| es12 | [Reading and writing documents](https://www.elastic.co/docs/deploy-manage/distributed-architecture/reading-and-writing-documents) |
| es13 | [Searchable snapshots](https://www.elastic.co/docs/deploy-manage/tools/snapshot-and-restore/searchable-snapshots) |
| es14 | [JVM settings](https://www.elastic.co/docs/reference/elasticsearch/jvm-settings) |
| es15 | [Heap issues at ~10,000 shards](https://discuss.elastic.co/t/elasticsearch-heap-issues/60828) (2016) |
| es16 | [High JVM memory pressure](https://www.elastic.co/docs/troubleshoot/elasticsearch/high-jvm-memory-pressure) |
| sl1 | [Serverless differences](https://www.elastic.co/docs/deploy-manage/deploy/elastic-cloud/differences-from-other-elasticsearch-offerings) |
| sl2 | [Serverless overview](https://www.elastic.co/docs/deploy-manage/deploy/elastic-cloud/serverless) |
| sl4 | [Serverless billing](https://www.elastic.co/docs/deploy-manage/cloud-organization/billing/elasticsearch-billing-dimensions) |
| os1 | [OpenSearch 3.0](https://opensearch.org/blog/unveiling-opensearch-3-0/) (updated 2025-12-01) |
| os2 | [LTR plugin](https://raw.githubusercontent.com/opensearch-project/documentation-website/main/_search-plugins/ltr/index.md) |
| os3 | [k-NN engines](https://raw.githubusercontent.com/opensearch-project/documentation-website/main/_mappings/supported-field-types/knn-methods-engines.md) |
| os4 | [Normalization processor](https://docs.opensearch.org/3.7/search-plugins/search-pipelines/normalization-processor/) |
| os5 | [Aggregations](https://raw.githubusercontent.com/opensearch-project/documentation-website/main/_aggregations/index.md) |
| os6 | [Clients](https://raw.githubusercontent.com/opensearch-project/documentation-website/main/_clients/index.md) |
| os7 | [LICENSE.txt](https://raw.githubusercontent.com/opensearch-project/OpenSearch/main/LICENSE.txt) |
| os8 | [Index settings](https://raw.githubusercontent.com/opensearch-project/documentation-website/main/_install-and-configure/configuring-opensearch/index-settings.md) |
| os9 | [Segment replication](https://raw.githubusercontent.com/opensearch-project/documentation-website/main/_tuning-your-cluster/availability-and-recovery/segment-replication/index.md) |
| os10 | [Mapping explosion](https://docs.opensearch.org/3.6/mappings/mapping-explosion/) |
| os11 | [Searchable snapshots](https://raw.githubusercontent.com/opensearch-project/documentation-website/main/_tuning-your-cluster/availability-and-recovery/snapshots/searchable_snapshot.md) |
| os12 | [Cluster settings](https://raw.githubusercontent.com/opensearch-project/documentation-website/main/_install-and-configure/configuring-opensearch/cluster-settings.md) |
| os13 | [AWS best practices](https://docs.aws.amazon.com/opensearch-service/latest/developerguide/bp.md) |
| os14 | [Install](https://raw.githubusercontent.com/opensearch-project/documentation-website/main/_install-and-configure/install-opensearch/index.md) |
| os15 | [Forum: max shards per node](https://forum.opensearch.org/t/max-shards-per-node-clarification/23777) (2025-03-14) |
| os16 | [Cluster-manager task throttling](https://docs.opensearch.org/3.1/tuning-your-cluster/cluster-manager-task-throttling/) |
| so1 | [Query parsers](https://solr.apache.org/guide/solr/latest/query-guide/query-syntax-and-parsers.html) |
| so2 | [Learning to rank](https://solr.apache.org/guide/solr/latest/query-guide/learning-to-rank.html) |
| so3 | [Dense vectors](https://solr.apache.org/guide/solr/latest/query-guide/dense-vector-search.html) |
| so4 | [Solr news](https://solr.apache.org/news.html) |
| so5 | [JSON Facet API](https://solr.apache.org/guide/solr/latest/query-guide/json-facet-api.html) |
| so6 | [Major changes in Solr 10](https://solr.apache.org/guide/solr/latest/upgrade-notes/major-changes-in-solr-10.html) |
| so7 | [LICENSE.txt](https://github.com/apache/solr/blob/main/LICENSE.txt) (re-fetched) |
| so8 | [Commits and transaction logs](https://solr.apache.org/guide/solr/latest/configuration-guide/commits-transaction-logs.html) |
| so9 | [SolrCloud shards and indexing](https://solr.apache.org/guide/solr/latest/deployment-guide/solrcloud-shards-indexing.html) |
| so10 | [Bloomberg, ApacheCon 2024](https://www.apachecon.com/acna2024/slides/03_Scaling_Solr_From_Desktop_to_Cloud_Scale.pdf) |
| so11 | [JVM settings](https://solr.apache.org/guide/solr/latest/deployment-guide/jvm-settings.html) |
| so12 | [SOLR-14927](https://issues.apache.org/jira/browse/SOLR-14927) |
| so13 | [SOLR-7191](https://issues.apache.org/jira/browse/SOLR-7191) |
| v1 | [YQL](https://docs.vespa.ai/en/querying/query-language.html) |
| v2 | [BM25](https://docs.vespa.ai/en/ranking/bm25.html) |
| v3 | [LightGBM](https://docs.vespa.ai/en/ranking/lightgbm.html) |
| v4 | [HNSW](https://docs.vespa.ai/en/querying/approximate-nn-hnsw.html) |
| v5 | [Clients](https://docs.vespa.ai/en/clients/index.html) |
| v6 | [LICENSE](https://raw.githubusercontent.com/vespa-engine/vespa/master/LICENSE) |
| v7 | [Consistency](https://docs.vespa.ai/en/content/consistency.html) |
| v8 | [Elasticity](https://docs.vespa.ai/en/content/elasticity.html) |
| v9 | [Streaming search](https://docs.vespa.ai/en/performance/streaming-search.html) (re-fetched) |
| v10 | [Proton](https://docs.vespa.ai/en/content/proton.html) |
| v11 | [Memory visualizer](https://docs.vespa.ai/en/performance/memory-visualizer.html) |
| v12 | [Feed block](https://docs.vespa.ai/en/operations/feed-block.html) |
| qw1 | [Query language](https://quickwit.io/docs/reference/query-language) |
| qw2 | [ES-compatible API](https://quickwit.io/docs/reference/es_compatible_api) |
| qw3 | [Querying](https://quickwit.io/docs/overview/concepts/querying) |
| qw4 | [Aggregations](https://quickwit.io/docs/reference/aggregation) |
| qw5 | [Repository metadata](https://api.github.com/repos/quickwit-oss/quickwit) |
| qw6 | [Index config](https://quickwit.io/docs/configuration/index-config) (re-fetched) |
| qw7 | [Architecture](https://quickwit.io/docs/overview/architecture) |
| qw8 | [Issue 4256](https://github.com/quickwit-oss/quickwit/issues/4256) (2023-12) |
| qw9 | [Cluster sizing](https://quickwit.io/docs/deployment/cluster-sizing) |
| qw10 | [Quickwit joins Datadog](https://quickwit.io/blog/quickwit-joins-datadog) (2025-01-09) |
| qw11 | [Releases](https://github.com/quickwit-oss/quickwit/releases) |
| tv1 | [Tantivy repository](https://github.com/quickwit-oss/tantivy) |
| pd1 | [ParadeDB overview](https://docs.paradedb.com/documentation/overview) |
| pd2 | [RRF](https://api.pgxn.org/src/pg_search/pg_search-0.25.7/docs/documentation/hybrid/rrf.mdx) |
| pd3 | [Aggregate limitations](https://api.pgxn.org/src/pg_search/pg_search-0.25.7/docs/documentation/aggregates/limitations.mdx) |
| pd4 | [ParadeDB README](https://github.com/paradedb/paradedb) |
| pd5 | [Introduction](https://www.paradedb.com/docs/welcome/introduction) |
| pd6 | [Limitations](https://www.paradedb.com/docs/welcome/limitations) |
| pd7 | [Architecture](https://www.paradedb.com/docs/welcome/architecture) |
| pd8 | [Index build tuning](https://docs.paradedb.com/documentation/performance-tuning/create-index.md) |
| pd9 | [Issue 6007](https://github.com/paradedb/paradedb/issues/6007) (2026-08) |
| mc1 | [Operators](https://manual.manticoresearch.com/Searching/Full_text_matching/Operators) |
| mc2 | [Search options](https://manual.manticoresearch.com/Searching/Options) |
| mc3 | [KNN](https://manual.manticoresearch.com/Searching/KNN) |
| mc4 | [Faceted search](https://manual.manticoresearch.com/Searching/Faceted_search) |
| mc5 | [Introduction](https://manual.manticoresearch.com/Introduction) |
| mc6 | [README](https://github.com/manticoresoftware/manticoresearch) (vendor figures) |
| mc7 | [Real-time table](https://manual.manticoresearch.com/Creating_a_table/Local_tables/Real-time_table) |
| mc8 | [Table settings](https://manual.manticoresearch.com/Creating_a_table/Local_tables/Plain_and_real-time_table_settings) |
| mc9 | [Issue 891](https://github.com/manticoresoftware/manticoresearch/issues/891) |
| mc10 | [Issue 4046](https://github.com/manticoresoftware/manticoresearch/issues/4046) (2025-12) |
| ss1 | [SeekStorm README](https://github.com/SeekStorm/SeekStorm) (vendor figures) |
| me1 | [Filter reference](https://www.meilisearch.com/docs/learn/filtering_and_sorting/filter_expression_reference) |
| me2 | [Typo tolerance](https://www.meilisearch.com/docs/learn/relevancy/typo_tolerance_settings) |
| me3 | [Ranking rules](https://www.meilisearch.com/docs/learn/relevancy/ranking_rules) |
| me4 | [AI-powered search](https://www.meilisearch.com/docs/learn/ai_powered_search/getting_started_with_ai_search) |
| me5 | [Facets](https://www.meilisearch.com/docs/learn/filtering_and_sorting/search_with_facet_filters) |
| me6 | [SDKs](https://www.meilisearch.com/docs/learn/resources/sdks) |
| me7 | [Repository](https://github.com/meilisearch/meilisearch) |
| me8 | [Asynchronous operations](https://www.meilisearch.com/docs/learn/async/asynchronous_operations) |
| me9 | [Sharding](https://www.meilisearch.com/docs/resources/self_hosting/sharding/overview) |
| me10 | [FAQ](https://www.meilisearch.com/docs/resources/help/faq) |
| me11 | [Issue 4694](https://github.com/meilisearch/meilisearch/issues/4694) (2024-06-11) |
| me12 | [Multi-tenancy guide](https://blog.meilisearch.com/multi-tenancy-guide/); [tenant tokens](https://www.meilisearch.com/docs/learn/security/multitenancy_tenant_tokens) |
| me13 | [Updating](https://www.meilisearch.com/docs/learn/update_and_migration/updating) |
| me14 | ["Meilisearch is too slow"](https://www.meilisearch.com/blog/meilisearch-is-too-slow) (2024-08-20, vendor) |
| ts1 | [Search API](https://typesense.org/docs/latest/api/search.html) |
| ts2 | [Vector search](https://typesense.org/docs/latest/api/vector-search.html) |
| ts3 | [Repository](https://github.com/typesense/typesense) |
| ts4 | [Migrating from Algolia](https://typesense.org/docs/guide/migrating-from-algolia.html) (vendor) |
| ts5 | [High availability](https://typesense.org/docs/guide/high-availability.html) |
| ts6 | [System requirements](https://typesense.org/docs/guide/system-requirements.html) |
| ts7 | [Forum: slow restarts](https://threads.typesense.org/t/29652771/hi-everyone-when-i-restart-typesense-it-takes-well-over-3-4-) (unverified, search summary) |
| ts10 | [Forum: collection limits](https://threads.typesense.org/t/29452327/hi-does-anybody-know-if-there-is-a-collection-limit-we-have-) (unverified); [scoped keys](https://typesense.org/docs/latest/api/api-keys.html) |
| al1 | [Faceting](https://www.algolia.com/doc/guides/managing-results/refine-results/faceting/) |
| al2 | [Ranking criteria](https://www.algolia.com/doc/guides/managing-results/relevance-overview/in-depth/ranking-criteria/) |
| al3 | [Pricing](https://www.algolia.com/pricing) |
| al4 | [SDKs](https://www.algolia.com/doc/libraries/sdk) |
| al5 | [Asynchronous operations](https://www.algolia.com/doc/guides/sending-and-managing-data/send-and-update-your-data/in-depth/index-operations-are-asynchronous) (search summary) |
| al6 | [Servers and clusters](https://www.algolia.com/doc/guides/scaling/infrastructure/classic/servers-clusters) (unverified, search summary) |
| al7 | [Scaling to larger datasets](https://www.algolia.com/doc/guides/scaling/scaling-to-larger-datasets/) |
| al8 | [Service limits](https://www.algolia.com/doc/guides/scaling/algolia-service-limits/); [secured keys](https://www.algolia.com/doc/guides/security/api-keys/how-to/user-restricted-access-to-data/) |
| al9 | [Sacra on Algolia](https://sacra.com/chat/h/b539c7aa-1823-4667-8e08-a87a1e79679b/) (analyst) |
| tp1 | [Query](https://turbopuffer.com/docs/query) |
| tp2 | [Full-text search](https://turbopuffer.com/docs/fts) |
| tp3 | [Tradeoffs](https://turbopuffer.com/docs/tradeoffs) |
| tp4 | [Limits](https://turbopuffer.com/docs/limits) (re-fetched) |
| tp5 | [Guarantees](https://turbopuffer.com/docs/guarantees) |
| tp6 | [Architecture](https://turbopuffer.com/docs/architecture) (vendor latency; cold p50 reads 874 ms or 402 ms by page) |
| tp7 | [Sacra interview, Indeed engineer](https://sacra.com/research/ai-engineer-indeed-turbopuffer-vespa-elasticsearch/) (2026-05-25) |
| qd1 | [Indexing](https://qdrant.tech/documentation/concepts/indexing/); [storage](https://qdrant.tech/documentation/concepts/storage/) |
| qd2 | [Qdrant 1.20](https://qdrant.tech/blog/qdrant-1.20.x/) (2026-10-22) |
| qd3 | [Hybrid queries](https://qdrant.tech/documentation/concepts/hybrid-queries/) |
| qd4 | [Qdrant 1.12](https://qdrant.tech/blog/qdrant-1.12.x/) |
| qd5 | [Repository](https://github.com/qdrant/qdrant) |
| qd6 | [Optimizer](https://qdrant.tech/documentation/concepts/optimizer/) |
| qd7 | [Distributed deployment](https://qdrant.tech/documentation/scaling/distributed_deployment/) |
| qd8 | [Multitenancy guide](https://qdrant.tech/documentation/guides/multiple-partitions/) |
| wv1 | [Inverted index](https://docs.weaviate.io/weaviate/concepts/indexing/inverted-index) |
| wv2 | [Hybrid search](https://docs.weaviate.io/weaviate/concepts/search/hybrid-search) |
| wv3 | [Aggregate](https://docs.weaviate.io/weaviate/api/graphql/aggregate) |
| wv4 | [Repository](https://github.com/weaviate/weaviate) |
| wv5 | [Consistency](https://docs.weaviate.io/weaviate/concepts/replication-architecture/consistency) |
| wv6 | [Data concepts](https://docs.weaviate.io/weaviate/concepts/data) (re-fetched) |
| wv7 | [Tenant states](https://docs.weaviate.io/weaviate/manage-collections/tenant-states) |
| wv8 | [Forum: slow text search](https://forum.weaviate.io/t/slow-text-search/2634) |
| mv1 | [Full-text search](https://milvus.io/docs/full-text-search.md) |
| mv2 | [Hybrid search](https://milvus.io/docs/multi-vector-search.md) |
| mv3 | [Milvus 3.0](https://zilliz.com/news/milvus-3-0-lake-native-vector-database) (2026-07-16, vendor) |
| mv4 | [Repository](https://github.com/milvus-io/milvus) |
| mv5 | [Consistency](https://milvus.io/docs/consistency.md) |
| mv6 | [Architecture](https://milvus.io/docs/architecture_overview.md) |
| mv7 | [Tiered storage](https://milvus.io/docs/tiered-storage-overview.md) |
| mv8 | [Limits](https://milvus.io/docs/limitations.md) |
| mv9 | [Multi-tenancy, v2.4](https://milvus.io/docs/v2.4.x/multi_tenancy.md) |
| mv10 | [llama-stack commit citing Milvus issue 40848](https://git.kvant.cloud/phoenix-oss/llama-stack-mirror/commit/0108bb1aa51d4f1359a9f4a428d2fed0c4a5dfb6) |
| ld1 | [Filtering](https://docs.lancedb.com/search/filtering) |
| ld2 | [Full-text search](https://docs.lancedb.com/search/full-text-search) |
| ld3 | [Hybrid search](https://docs.lancedb.com/search/hybrid-search) |
| ld4 | [Repository](https://github.com/lancedb/lancedb) |
| ld5 | [Enterprise](https://docs.lancedb.com/enterprise); [consistency](https://docs.lancedb.com/tables/consistency) |
| ld6 | [Namespaces](https://docs.lancedb.com/namespaces) |
| ld7 | [SimpleMem issue 78](https://github.com/aiming-lab/SimpleMem/issues/78) |
| oo1 | [Logs](https://openobserve.ai/docs/user-guide/logs/) |
| oo2 | [Tantivy index](https://openobserve.ai/docs/user-guide/advanced/query-tuning/tantivy-index/) |
| oo3 | [README](https://github.com/openobserve/openobserve) |
| oo4 | [Architecture](https://openobserve.ai/docs/architecture/) |
| oo5 | [Environment variables](https://openobserve.ai/docs/environment-variables/) |
| oo6 | [Tantivy 49 s to 2 s](https://openobserve.ai/blog/openobserve-tantivy-49s-to-2s) (2026-05-20, vendor) |
| lk1 | [LogQL](https://grafana.com/docs/loki/latest/query/) |
| lk2 | [Repository metadata](https://api.github.com/repos/grafana/loki) |
| lk3 | [Overview](https://grafana.com/docs/loki/latest/get-started/overview/) |
| lk4 | [Storage](https://grafana.com/docs/loki/latest/operations/storage/) |
| lk5 | [Configuration](https://grafana.com/docs/loki/latest/configure/) |
| lk6 | [Bloom filters](https://grafana.com/docs/loki/latest/operations/bloom-filters/) |
| lk7 | [Cardinality](https://grafana.com/docs/loki/latest/get-started/labels/cardinality/) |
| ch1 | [Text indexes](https://clickhouse.com/docs/engines/table-engines/mergetree-family/textindexes) |
| ch2 | [Full-text search GA](https://clickhouse.com/blog/full-text-search-ga-release) (2026-03-10) |
| ch3 | [Repository metadata](https://api.github.com/repos/ClickHouse/ClickHouse) |
| ch4 | [Asynchronous inserts](https://clickhouse.com/docs/optimize/asynchronous-inserts) |
| ch5 | [Issue 125547](https://github.com/ClickHouse/ClickHouse/issues/125547), [121690](https://github.com/ClickHouse/ClickHouse/issues/121690) (2026-10) |
| pc1 | [Pinecone object limits](https://docs.pinecone.io/reference/api/database-limits/object-limits) |
| az1 | [Azure AI Search limits](https://learn.microsoft.com/en-us/azure/search/search-limits-quotas-capacity) (2026-09-16) |
| zn1 | [ZincSearch issue 782](https://github.com/zincsearch/zincsearch/issues/782) |
| sn1 | [Sonic README](https://github.com/valeriansaliou/sonic) |
| x1 | Xapiand `docs/src/content/docs/reference-guide/search/query-dsl.md` |
| x2 | Xapiand `.../query-dsl/leaf-queries/text-queries.md` |
| x3 | strata `docs/design/xapiand-on-strata.md:68` (group E: default BM25) |
| x4 | Xapiand docs tree: no LTR or vector page (`find docs/src/content/docs`) |
| x5 | Xapiand `.../search/aggregations/` (five bucket and eleven metric pages) |
| x6 | Xapiand `.../reference-guide/api.md:52-73` (custom methods, firewall workaround); no client library documented |
| x7 | Xapiand `docs/src/content/docs/about.md:12,39` (MIT, Xapian GPL; near real time) |
| x8 | Xapiand `ARCHITECTURE.md:339-343` |
| x9 | Xapiand `.../documents/store-api.md:5-13` |
| x10 | Xapiand `.../indexes/replication.md:14-16` |
| x11 | Xapiand `docs/src/content/docs/benchmarks.md:15-26` (Xapiand on Xapian, 20,000 documents) |
| x12 | Xapiand `docs/src/content/docs/options.md:84-87` |
| x13 | Xapiand `.../indexes/backups.md` |
| x14 | Xapiand `.../schemas/accuracy.md` |
| x15 | Xapiand `.../schemas/dynamic-typing.md` |
| x16 | Xapiand `.../schemas/foreign-schemas.md` |
| e1 | strata `docs/design/xapiand-on-strata.md:88` (gaps G8, G9); `docs/PLAN.md:629-638` (Phase 5); queryparser `README.md:28-29,67` (Xapian oracle) |
| e2 | prospect `docs/perf/bm25-presets.md:1-15` |
| e3 | `grep -i 'vector\|knn\|hnsw\|embedding'` over strata `docs/PLAN.md`, `docs/DISTRIBUTION.md`, `docs/design/`: Xapian oracle vectors only |
| e4 | strata `docs/PLAN.md:598-614` (Phase 4) |
| e5 | strata `docs/PLAN.md:670-692` (Phase 8); prospect `README.md:8` (MIT) |
| e6 | strata `design/write-modes:docs/design/write-buffer.md` revision 4; `docs/status/README.md:16-19` |
| e7 | strata `docs/DISTRIBUTION.md:101-113`; `docs/PLAN.md:576-596` (Phase 3R) |
| e8 | strata `docs/DISTRIBUTION.md:94-99,115-126` |
| e9 | strata `docs/design/small-indexes.md:1,15,27`; `docs/DISTRIBUTION.md:10-36` |
| e10 | strata `docs/perf/core-vs-rocksdb.md:150-167` |
| e11 | prospect `docs/perf/inverter.md:1092-1104` (gate VM, strata `c6bf4d9`, Tantivy 0.26) |
| e12 | prospect `docs/gates/phase-3-ratchets.md:49`; prospect `README.md:35` |
| e13 | strata `docs/perf/remeasure-2026-10-26.md` (row 1) |

## Owner answers (2026-10-27)

- Q3 API shape: our own query language is canonical, with an Elasticsearch-compatible subset as an adapter.
- Q4 query-string dialect: Xapian's (improved on merit) at 7a, Lucene's after.
- Q1 audience: steer toward real AI use cases only if we are a clear fit and in a good position (owner, 2026-10-27); the candidate is open, self-hostable retrieval for AI applications, many small hybrid (full-text plus vector) indexes per host. Not locked; D1 (idle-index cost at millions per host) is the first proof.
- Q2 vectors: in scope for AI retrieval; exact per small index plus fusion first is the starting design (`docs/design/hybrid-search.md`). The owner asked whether vectors help with geolocation and AI; answered in the session (geo stays on spatial indexes; vectors are the AI hook).
