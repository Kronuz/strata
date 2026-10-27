# Segment records and bodies

> Status: CURRENT (2026-10-27) as built on `phase/cold-tier-step1`; the design is
> `docs/design/cold-tier.md` (D2, D9, D11) on `design/cold-tier`.

## What it does

An index's segment is a **record** plus a **body**. The record is the owner's: the manifest entry
(`ref`), the copy-on-write deleted bits (sized from `ref.doc_count`), `dead`, `deletes_dirty`,
`damaged`, `unsynced`, and three **facts** the manifest needs and the entry lacks (`structures`,
`version`, `total_length`). The body is the mapping (or owned bytes) and the decoded `segment`. A
record can exist with no body: that is a segment nobody has read since the index opened.

`options::eager_segments` (on by default) maps every segment at open, as before. Off, open reads
the manifest, the live-docs files and the directory scan, and maps nothing; a segment maps at the
first `seg()` call that needs it. `STRATA_EAGER_SEGMENTS=0` flips the default for a whole test run.

## How

```
record.body_  ──null──▶ reader maps: open file, checks, new body
   │                       │
   │                       ├─ compare-exchange null → body   won: use it
   │                       ├─ lost to a body                 destroy mine, use the winner's
   │                       └─ lost to the failed marker      destroy mine, throw corrupt_segment
   ├─ body       read through an acquire load
   └─ marker     (static) a first map failed its checks twice
```

- `open_segment::seg()` is the map point: acquire load; on null, `map_body()` (cold, noinline).
  `mapped()` is the body kind: it asserts a body exists and never maps.
- A body or the marker is installed once and never replaced or cleared before the record dies; the
  destructor frees a body, never the marker. A reader's pointer to the segment stays valid while
  the record (kept alive by its view) does.
- **Facts are owner-written.** A reader's map leaves them in the body. The owner writes them where
  it makes a body (`adopt()`: open, flush output, merge output before it is published, install) or
  in commit's prepare (`learn_facts()`), under the lease.
- `segment_view` holds the record; `seg()` maps on first use. `segments()`, `owned_segments` and
  `build_view` take addresses and map nothing.
- Counters readers' maps write (`media_retries()`, `merges_skipped()`) live in a heap-held
  `load_context` shared by the records, so a moved index keeps them and they are atomic.

### D9's map points, as built

| Read | Kind | On failure |
|---|---|---|
| merge selection, commit allowance, commit's erase and live-docs files | entry (`ref`) | none |
| manifest mask, versions, `total_length` | facts | none; commit fences if a fact is unknown |
| `live_length()` of a segment with new deletes | body, mapped in the prepare | the host's |
| get, scan, digest, `superseded` | `seg()` on the caller's thread | to the caller |
| `apply()` lookups | `seg()` under the lease | to the host (needs the rollback to be safe) |
| merge scheduling | `seg()` before anything is reserved | source marked `damaged` and left out, or the round dropped |
| merge job, `mark_damaged`, flush install | `mapped()` | none new |
| flush and merge output | `map_output()` | a host error keeps the bytes; no fence |
| install's own copies | lazy: unmapped record with the incoming body's facts; eager: today's map | eager: to the host, unfenced |
| `check()` | `seg()` on every record | to the caller |
| `heal()` | bodied, marked or `damaged` records only | host errors to the host |

**The failure rule.** `read_checked` runs the open and its checks twice. A check failing twice or
`EIO` twice is the medium's: the first map reports `corrupt_segment` and installs the marker. Anything
else (`EMFILE`, `ENFILE`, `ENOMEM`, `EACCES`) is the host's: rethrown unchanged, never counted,
never marking, never a fence.

**Commit's prepare** (`prepare_commit()`, after the flush, `publish_merges()` and `maybe_merge()`,
before `guarded(commit_files)`): maps every record whose facts are unknown, and every record with
new deletes and no body (for `live_length`). A host error or a marked record throws and leaves the
index as `refresh()` leaves it: flushed segments and installed merges in place, unpublished,
nothing durable, nothing fenced. A marked record throws on every commit until `heal()` replaces it.
Debug builds assert that no input map runs inside `guarded()`.

**Scheduling.** `maybe_merge()` swallows a dropped round (`map_failure`), beside `ENOSPC`, and counts
it in `merges_skipped()`; `merge()`, `force_merge()` and `force_merge_deletes()` rethrow it.

**Open (D11).** Each manifest-named segment's name and size are checked against the directory scan
(no system call); the rest of `load_segment`'s checks (version, footer CRC, identity, document
count, structures and versions against the manifest) run at the first map and report as
`corrupt_segment`.

## Why

Open cost and memory per idle index: a mapping, a descriptor and a vnode per segment are what bound
the number of indexes per host (`docs/design/cold-tier.md`, "Why"). The split lets a later step
twin a record without its body (demotion) and keeps the I1 rule: a mapping dies only with its record.

## Invariants

1. A body or the marker is installed at most once and is never replaced, cleared or freed before
   the record dies.
2. A loser of the install race destroys only its own body, on its thread, and uses what won.
3. Only the owner writes a record's facts; readers never write the record.
4. No input map runs inside `guarded()` or a noexcept chore; output maps never throw a host error.
5. A host error never marks a record, counts as damage, or fences.

## Numbers

None: functional claims only. The lead measures `get` and open on `main`.

## Code pointers

`include/strata/core/index.hh`: `open_segment`, `segment_body`, `load_context` (the record and
body); `open_body_of`, `load_segment`, `load_record`, `read_live` (open and checks);
`prepare_commit`, `commit_files`; `schedule` and `map_failure`; `heal`; `streamed_segment` and
`map_output`. Tests: `test/cold_segment_test.cc`.
