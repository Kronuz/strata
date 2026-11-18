> Status: CURRENT (2026-10-30) the single-document write path as of strata main `c82ac9f` and Xapiand master `e265acee6`. Source chapter for the Architecture and Design book, sections 3.4 and 10.1. The document is Xapiand's (`src/engine/strata/`); `find_docid` is strata's.

# One document in: the flat document and `find_docid`

Xapiand's bulk load writes one document per `Shard::replace`: build the document, read the stored version of the id, hand a transaction to strata. Two pieces make that cheap. `StrataDocument` holds a document's terms in a few flat arrays with a hash table over them. `basic_index::find_docid` reads a document's version from the write buffer without flushing it.

## How it works

```
 Shard::replace(did, doc)
    |
    |  1. build: add_term / add_posting
    |       StrataDocument: flat terms + one hash table + two arenas
    |
    |  2. version_of(did)   -> index->find_docid(did)       shard.cc:416
    |       active buffer's docid map, frozen buffers, then segments
    |       (no refresh, no flush, no merge)
    |
    |  3. fill the transaction (group() = one counting pass), apply
    v
 buffer record
```

### The flat document

`StrataDocument` (`document.h:65`) keeps one document's terms in four containers:

- **Terms.** `terms_` is a vector of `DocTerm` (`document.h:44`) in arrival order: the field, the term's offset and size in the byte arena, the wdf, a cached 32-bit hash, and the head and tail of the term's position chain.
- **One term table.** `table_` (`document.h:131`) is an open-addressing table over every term of every field, keyed by (field, term bytes). A slot holds the term's index plus one; 0 is empty. Up to 8 terms (`linear_terms`, `document.cc:151`) no table exists and `intern` scans the hashes in order. The table is built when the ninth term arrives and doubles when it is less than twice the term count, with 64 slots at least (`document.cc:238-251`).
- **Two arenas.** `bytes_` holds the bytes of every term, so a term costs no allocation of its own (`document.cc:233-237`). `nodes_` holds the positions as `(position, next)` nodes chained per term (`document.cc:281-296`). Positions are sorted and deduplicated when read, only if they did not arrive increasing (`positions_into`, `document.cc:299-313`).
- **Fields.** `fields_` lists the fields in arrival order. `field_of` finds a field through `slot_`, a table from the host's field id to the field's index, checked by name (`document.cc:338-364`).

`intern` (`document.cc:217`) hashes the bytes eight at a time (`hash_of`, `document.cc:129`), mixes in the field index, probes, and appends on a miss. `group` (`document.cc:266`) turns arrival order into per-field runs with one counting pass.

A bulk-load document has about 171 terms in 47 fields and 137 positions (`docs/perf/t11/e4-document-arena.md` in Xapiand). Each term used to own a string and a positions vector, a few hundred small blocks per document to make and free; the arenas replace them with a handful of growing arrays.

### `find_docid`: a version without a flush

A replace or erase of a document needs one fact: the version of the live copy, to check the caller's expected version and number the new one. `find_docid(docid)` (`index.hh:1630`) returns it through `locate_docid` (`index.hh:3667`): the active buffer's docid map first, the frozen buffers newest first, then the segments newest first, honoring their deleted bits and the pending deletes. Like every read of the writer's state it belongs to the lease holder (`lease_.check_read()`), and it writes nothing.

![Lookup order of find_docid](single-document-write.png)

The segment-only path, `locate` (`shard.cc:390`), reads flushed segments only. `locate` calls `core.refresh()` first (`shard.cc:395`), so a replace of a document still in the buffer wrote it as a tiny segment and ran the merge policy inline on the writer thread (profile in `docs/perf/t11/e2-replace-without-flush.md`). `version_of` (`shard.cc:416`) now calls `find_docid`; `replace` and `erase` use it (`shard.cc:669-672`, `shard.cc:762`). `get` and `find` paths that need the segment still call `locate`.

## Invariants

- `find_docid` returns the version `apply()` would read for the same docid: the newest live copy, buffers before segments, a tombstone or a deleted bit meaning absent. Test: `find_docid_reads_buffers` (`test/write_buffer_reads_test.cc:622`) checks the buffer, a flushed segment and an erased docid, and that the call writes no segment (`ix.segments().empty()`).
- A document's terms are unique under (field, bytes): `intern` returns the existing term on a hash and byte match.
- A document's byte arena stays under 2^32 minus 16 bytes; `intern` raises `errc::invalid` beyond it (`document.cc:234`).

## Numbers

Xapiand `harness/profile_bulk.sh`: 20,000 accounts documents in one RESTORE, a fresh `--solo` node per run, `sample(1)`, macOS x86_64 APFS, `quiet exclusive`, interleaved base, new, base (Xapiand `docs/perf/t11/README.md`). Ratios are the strata engine rows' CPU per document over Xapian's, on the engine row.

| Step | Change | Engine CPU, ratio to base (pairs) |
|---|---|---|
| E1 | flat document with a hashed term table | 0.89 (0.90 0.87 0.88 0.91) |
| E2 | `find_docid` in `replace` and `erase` | 0.72 (0.64 0.72 0.75 0.75) |
| E3 | the shard vouches for the terms it fills | 0.95 (marginal, inside its spread) |
| E4 | two per-document arenas | 0.83 (0.84 0.80 0.82 0.85) |
| E5 | one term table per document | 0.86 (noisy) |

Strata's engine share against Xapian's: **0.80x to 0.47x** CPU and **0.53x to 0.22x** on the critical thread, five interleaved rounds each. The whole process uses 0.838 to 0.780 ms of CPU per document (7% less) and 5,188 to 6,355 documents per second (1.42x Xapian's). Peak RSS falls from 1,160 MB to 833 to 855 MB. E2 is the largest step: before it, about 1,300 of a 6,100-sample strata row were the flush and the inline merge a replace provoked.

**The flush-wait correction.** The CPU row above counts the thread blocked in `F_FULLFSYNC` under `commit_files` as busy, in strata's rows and in the denominator; Xapian runs `DB_NO_SYNC`, so only strata's share is inflated. After the harness reports that wait as its own line (`docs/gates/phase7a.md` in Xapiand, "What moved"), the same rows read **0.357x** of Xapian's CPU on the closing run (0.1345 against 0.3763 ms per document, 10 of 10 pairs under 0.5; the estimator as first committed gave 0.387x), and 0.414x on the earlier rerun that had read 0.515x. The critical path is 0.226x either way. The engine row is not the process: the saving that moves hardware is whole-process CPU, 0.058 ms per document, about 16 CPU-hours per billion documents (the source rates a core-hour at roughly $0.04, so under a dollar per billion).

Not measured: Linux, FreeBSD and arm64.

## Code

Xapiand `src/engine/strata/document.{h,cc}`, `shard.cc` (`version_of`, `replace`, `erase`). Strata `include/strata/core/index.hh`: `find_docid`, `locate_docid`, `find_docid_in`. Records: Xapiand `docs/perf/t11/`, `docs/gates/phase7a.md`; strata `docs/perf/exploration-log.md` (T11 row).
