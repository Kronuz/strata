# The cold tier: millions of indexes per host

> Status: DRAFT (2026-10-27) revision 2, designing (ready for the rechecker on C5). Step 1 follows
> the review and its re-check (`docs/reviews/cold-tier-design.md`); the catalog's findings stay
> open for step 2 ("Open for step 2 and step 3").

Cited at strata `main` `8798fe1`, whose `include/` is `98ce743`'s (the review's base); measurements
from `docs/perf/idle-index.md` on `main` and the architect's probes
(`strata-wt/review-cold-tier/.scratch/ct-review/probes/`, rerun 2026-10-27 with identical output);
shared-reader at `design/shared-reader` `15b801a`; write buffer at `design/write-modes` `ae2cf70`.
RocksDB 11.11 was read for its table cache; none of its code enters strata.

## Summary

An index lives in one of four tiers. **Hot** is today's open index. **Warm** keeps the index
object and its directory lock but no mapping and no view. **Cold** is closed: nothing in memory.
**Remote** is cold with its files on object storage (reserved). A catalog per pool partition pins
indexes for requests, opens them on demand, and demotes them by a sweep over counters of the OS
resources each tier holds, so mappings, descriptors and vnodes are bounded by the hot and warm set,
never by the number of indexes on disk.

The concurrency rests on a rule the shared reader already keeps (I1): **a mapping dies only with
its segment record, under the write lease, after no protected view holds it.** Demotion swaps every
record for an unmapped twin, as `heal()` swaps damaged records, and never strips a live one.

## Why

An idle 1-document index costs 5.9 KB dirty on macOS, 8.1 KB on Linux, 1 descriptor, 1 mapping,
and 2 vnodes on macOS. The walls come first: macOS fails with `ENFILE` at 126,507 open indexes
(`kern.maxvnodes` 263,168, system-wide, root-only), Linux at 65,491 under the stock
`vm.max_map_count` of 65,530, and strata has no handle cache (`idle-index.md`, "Limits"). Raising
the sysctls is the host's call and leaves memory linear in the total count, 8.1 GB per million.
The field: 1,000 shards per Elasticsearch node, 18 to 19 thousand Weaviate tenants per node, and
Turbopuffer's 250M+ namespaces as a hosted fleet with ~400 ms+ cold queries
(`product-landscape.md:93,122`, `design/landscape`).

## What this builds on, and what it changes

| Document | Used | Changed |
|---|---|---|
| shared-reader D1, D4 to D7, D9, D11 | views, hazard records, frees under the lease, held files never queued, `publish_through`, non-blocking close | demotion is a publication point; an "unpublished" sentinel beside null-means-closed (D3); `segment_view::seg` becomes an accessor (D2) |
| write-buffer D2 | checkout; chores by `chore_scope` or owed to the holder (`owner.hh:148-171`, `index.hh:725-739`) | demotion is a chore; a catalog pin is a checkout |
| write-buffer D5 | `apply()` is all or nothing | depends on CT-R5's rollback (`fix/apply-atomic`) landing on `main` before lazy segments are the default |
| small-indexes, the lead's 2026-10-18 ruling | checks at first use, on the condition that a segment damaged at its size fails every read path clean and `heal()` reports it | Q3 carries the condition and its twelve cases (D11) |
| `ARCHITECTURE.md` | residency, suspension at query start (`:173-174`), overload shed (`:179-180`) | "footers and small sections at open" (`:149`) becomes "at first map" (Q3; the lead edits it on landing) |
| `DISTRIBUTION.md` | per-node rows (`:28-30`) | its "Cold" (`:94-98`, object storage) is this design's remote |
| `small-indexes.md`, `limits.md` | direction 2, "open lazily, close idle"; caps name their mechanism | this design is direction 2; `limits.md` gains D6's caps |
| `idle-index.md` | the per-index costs and cuts | three heap rows are mislabelled (N1); `main`'s note is the lead's to correct |

## Decisions

### D1. Four tiers, each defined by what it holds

| Tier | Holds | Dirty per index | fds | vnodes (macOS) | maps |
|---|---|---|---|---|---|
| Hot | open `basic_index`, the segments touched since promotion mapped, a view | today's measured 5.9 KB macOS, 8.1 KB Linux; target: no regression | 1 | 1 + mapped segments | mapped segments |
| Warm | open `basic_index` and directory lock, manifest, one unmapped record per segment, no view, idle core, a catalog slot | restated from measured pieces per segment count before it gates (N1); estimate about 1.7 KB of heap per one-segment index before the slot, `PosixIO` and slack | 1 | 1 | 0 |
| Cold | nothing in the process | 0 | 0 | 0 pinned | 0 |
| Remote | a cold index whose data files are on object storage (D8) | 0 | 0 | 0 | 0 |

On disk a 1-document index is 738 B in 3 files, 12 to 16 KB with blocks.

### D2. A segment is a record plus a body mapped at most once (cut 1)

`open_segment` (`index.hh:2093-2111`) splits.

- **The record** keeps the owner-written state: the entry (`ref`, which carries `doc_count`,
  `files.hh:79-93`), copy-on-write deleted bits sized from `ref.doc_count` (today from the body,
  `index.hh:2103-2105`), dead count, `damaged`, `unsynced`, and three **facts** the manifest needs
  and the entry lacks: `structures`, `version`, `total_length` (16 B, a known flag). **Only the
  owner writes them**, under the lease: where it makes a body (flush, merge output, install), in
  commit's prepare, at scheduling, and into twins. A reader's first map leaves them in the body;
  the owner reads the record's facts when known, else the body's after an acquire load of it.
- **The body** (mapping or owned bytes, and the decoded `segment`) is immutable and reached through
  an `std::atomic<const body*>` in the record that holds null, a body, or a static **failed**
  marker (D11). A thread that loads null maps: it opens the file through the index's IO, runs the
  first-map checks (D11), and installs the body, or the marker when the checks fail, by
  compare-exchange from null (acq_rel). The loser of either race (body against body, body against
  marker) destroys only its own body, at once on its thread, and then uses what won: a good body
  that loses to the marker throws `corrupt_segment`. No other thread saw the loser's body, and the
  body type carries no owner check (the record keeps `~open_segment`'s, `:2109`). A body or marker
  is never replaced or cleared before its record dies; the record's destructor frees a body, never
  the marker, which is static.
- **Open stops mapping.** Recovery builds records from the manifest, reads live-docs files, and
  checks each named file against the directory scan (D11). `options::eager_segments` maps all at
  open through the same code; it is the default until CT-R5 lands (step 1's order).
- **One accessor.** `segment_view::seg` (`:926-931`) becomes `seg()`, holding the record and
  doing an acquire load that maps on null. Retries count on an atomic: `media_retries_`
  (`:4533`) is written from reader threads once they map (`read_checked`, `:4402-4411`).
- **prospect** fills its own `segment_snapshot::seg` raw pointer (prospect `searcher.hh:26-30`)
  where snapshots are built, in prospect's tests and Xapiand's seam; those sites call `seg()`,
  which a search does anyway (its statistics read every segment's count and length,
  `searcher.hh:95-101`). prospect pins strata by commit (`CMakeLists.txt:9`); the pin bump adopts
  `seg()`.

### D3. Demotion to warm: twins, then the view retires (cut 2)

Demotion to warm is a chore under the lease, only on an index warm's predicate admits (D10). It
swaps **every** record for a twin, bodied or not, as `heal()` swaps records (`:879`). A twin copies
`ref`, the facts, the bits (shared), `dead`, `deletes_dirty`, `damaged`, and the failed marker
(static, so sharing it keeps I1); it has no body. `heal()` copies the first four of the record's own
fields the same way (`:905-908`). Warm needs no commit (D10), so a twin may stand for uncommitted
deletes, and `commit_files()` writes a live-docs file only where `deletes_dirty` is set
(`:4023-4035`, else it keeps the old generation, `:4053-4057`). Demotion forces a publication, since
`publish_point()` skips an unchanged state (`:2379`), and calls `free_views()` (`:2479-2493`). A
record left in place could be mapped afterwards by a reader holding an older view (`build_view`
shares `segs_`'s pointers, `:2389-2392`), and its body would live in the warm index until `close()`;
with every record twinned, a late map lands in the retired view's record (`twin_race.out`: 6 of 10
interleavings leave a body in `segs_` when only bodied records are twinned, 0 of 10 when all are).

Demotion is complete when its retired views are freed. Shared-reader D6 scans at publish, commit,
`reclaim()` and close, none of which a warm index runs, so the sweep runs `free_views()` under
`chore_scope` on warm slots with retired views (step 2), and those views' bodies count in the
slot's maps until then. `views_held()` matches records by segment id, not by pointer
(`:1244-1250`), so a twin and its retired original count once.

With the sentinel (step 3), demotion publishes the **unpublished** sentinel, a static view never
freed, instead of a view over the twins, and drops the empty active buffer the last view made
(`:2403-2408`; the next write makes one). Its acquire contract is open (CT-R7). That is cut 2: a
warm index holds the publication object and reader state, not a view or a buffer.

### D4. The catalog, one per pool partition

Beside the partition's registry (shared-reader D4), it owns the partition's open indexes, a
key-to-directory IO factory, the counters, and the sweep's hand.

- **Slots** live in a chunked array never freed while the catalog lives. A slot holds the key,
  the index, its reader handle, a reference bit, a last-use time, waiters, and a 64-bit word
  `pins:32 | state:8`, state one of `opening`, `open`, `demoting`, `closing`.
- **The table** maps keys to slots in 64 shards, each an open-addressed map under a mutex held
  for one probe, insert or erase, never across I/O. Only non-cold indexes have a slot. RocksDB's
  table cache is this shape: sharded handles bounded by `max_open_files` (`options.h:792,1114`,
  `db/table_cache.h:64`). `lru-cache` is not reused: it relinks a list per hit and is not
  thread-safe.
- **A pin is a checkout**: under the shard mutex, a compare-exchange from `{p, open}` to
  `{p+1, open}`, setting the reference bit if clear. Unpin stores the time, then decrements.

Step 2 revises D4 to D6 with CT-R4, R6, R8, R10, R12, R13 before its code.

### D5. Transitions

```
  cold ──pin miss──▶ opening ──▶ open (hot ⇄ warm) ──sweep: {0,open}→{0,closing}──▶ closing ──▶ cold
                        ▲ waiters resumed                                              │ waiters
                        └──────────────────────────── reopen ◀─────────────────────────┘
```

- **Cold to open.** The first pinner inserts an `opening` slot holding its pin and opens on the
  host's executor (about 54 syscalls), never a reactor thread; later pinners wait on the slot and
  are posted to their executors. A failed open erases the slot and fails its waiters.
- **Warm to hot** is the first reader's view build (D3) and maps (D2).
- **Hot to warm.** The sweep compare-exchanges `{0, open}` to `{0, demoting}`, runs D3 under
  `chore_scope`, which never waits, and stores `open`; pinners meeting `demoting` wait. A held
  lease means busy: it restores `open` and moves on. The exchange keeps the host's lease-less reads
  (`get`, `scan`, which a pinned client may call, `index.hh:718-724`) from overlapping the chore.
- **Open to cold.** The sweep compare-exchanges `{0, open}` to `{0, closing}`, takes
  `chore_scope`, checks cold's predicate (D10), and calls `close()`. Under the shard mutex it then
  erases the slot, or sets `opening` and reopens if waiters came. The `closing` fallback is open
  (CT-R6).

### D6. Policy over resource counters

The catalog counts what the OS limits: **maps**, **fds**, **vnodes** on macOS, and **bytes** on a
`byte_budget`. Past a soft cap the sweep demotes; at a hard cap a promotion that cannot first
evict fails with `too_many_open`, ARCHITECTURE's overload error, never a wait.

| Platform | Mechanism (stock value) | Soft / hard |
|---|---|---|
| Linux | mapped segments, `vm.max_map_count` (65,530) less the process's maps at start | 50% / 90%: 32,765 / 58,977 |
| macOS | vnodes, `kern.maxvnodes` (263,168 on the 32 GiB Mac), system-wide | 40% / 60%: 105,267 / 157,900 |
| Both | open indexes, the soft `RLIMIT_NOFILE` (CT-R12 restates it) | open for step 2 |
| Both | serving bytes | the host's budget |

The sweep runs on the host's timer per partition at 1 Hz or faster, with bounded work per call,
and inline when a promotion crosses a soft cap. It skips pinned slots, demotes a hot slot over a
hot cap or idle 30 s, and a warm one over a warm cap or idle 10 min. The replacement policy is
open: one-bit CLOCK misses T4's θ = 0.99 row (CT-R10).

### D7. The idle core (cut 3)

Of the `index` object's 1,320 B, about 650 B is writer and maintenance state an idle index never
uses, plus 200 B of merge machinery (`idle-index.md`, cut 3; to recount on main's write-buffer
fields). It moves behind one pointer made at the first writer call, freed at demotion to warm,
once the reclaim queue's fate at demotion is settled (CT-R4).

### D8. Remote is reserved

A remote index is cold with no data files local; a pin miss fetches them on the executor under
`opening` before the cold path runs, so the catalog needs only that hook. Its design is
DISTRIBUTION's storage tiers (PLAN D6).

### D9. Every owner read of a segment is entry, map, output, or body

Each read or map of a segment in `index.hh` is one of four kinds. **Entry** reads the record
(entry, bits, facts) and never maps. **Map** may map a committed file and follows the failure rule
below. **Output** maps a file this index just wrote. **Body** reads a body its own path created or
mapped earlier, asserts it, and never maps.

| Read | Lines | Kind | On failure |
|---|---|---|---|
| merge selection | `:1550-1555` | entry: `ref.doc_count`, bits | none |
| commit allowance (via `settle_storage`, also from `~hold`) | `:3244-3249`, `:3209-3211` | entry: `ref.doc_count` | none |
| commit: all-dead erase, live-docs files, `live_count` | `:4006`, `:4027-4028`, `:4055` | entry | none |
| commit: manifest's mask, versions, `total_length`; `live_length` without deletes | `:4056`, `:4074-4080` | entry: facts | none |
| commit: `live_length()` of a segment with new deletes | `:4032`, `:4102-4107` | map, in commit's prepare | the host's; a marked record throws `corrupt_segment` |
| `segments()`, `owned_segments`, `build_view` | `:958`, `:2116-2118`, `:2389-2392` | entry: they take addresses | none |
| get, superseded, scan, digest through `seg()` | `:1883-1896`, `:1790-1792`, `:1912-1959`, `:2006-2011`, `:1603-1611` | map, on the calling thread | to the caller |
| `apply()` stage 1: `locate_docid`, `lookup_key` | `:3360-3365`, `:3380-3386` | map, under the lease | rolled back, to the host |
| merge scheduling | `:2520-2527` | map, under the lease, before the job leaves the owner | source marked, or round skipped |
| merge job on the pool | `:2725-2759` | body: scheduling mapped its sources | none new |
| merge output | `:2770-2773`, published `:2830-2831` | output, on the pool | bytes kept |
| flush output (`streamed_segment`, `written_segment`) | `:2280-2302`, from `flush()` in `guarded()` (`:748`, `:819`) | output, under the lease | bytes kept |
| `mark_damaged` | `:2858-2870` | body: the job's sources | none new |
| flush's install | `:3818-3830` | body: the flush's own segments | none new |
| `install()`'s incoming segments | `:1370`, `:1384-1393` | body: loaded whole | none new |
| `install()`'s own copies | `:1405-1413` | lazy: entry, an unmapped record with facts from the incoming body; eager: map, today's rule (`:1405-1406`) | none; eager: to the host, unfenced |
| `check()` | `:1325` | map, all segments | to the caller |
| `heal()` | `:886-910` | map: bodied and failed records only | host errors to the host |
| check-in chores, `run_chores` | `:2341-2353` | none: publish, build, scan, allowance | none |

**The failure rule.** An error at a map point splits as `read_checked` splits it (`:4414-4418`):
a check failing twice or `EIO` twice is the medium's and reports `corrupt_segment`; anything else
(`EMFILE`, `ENFILE`, `ENOMEM`, `EACCES`, `too_many_open`) is the host's, rethrown unchanged, never
counted as damage, never marking a record, never a fence. Per path:

- **Output maps** never fail on a host error. `map()` throws on an `mmap` failure (completion
  `posix.h:342-354`); a host error mapping a file this index just wrote keeps the bytes in memory,
  as a backend that cannot map does: `written_segment` holds them (`:2280-2286`), and
  `streamed_segment` and the merge read back through the file they hold (`:2296-2300`; the merge
  today opens the file again, `:2771-2772`), with no new descriptor.
  Today such an error inside `guarded()` or from a merge job fences a healthy index
  (`output_map_probe.out`: `refresh()` with `map()` failing `ENOMEM` throws "durability failure",
  and the next apply and commit "the writer is fenced"; `force_merge(1)` the same).
- **Commit** maps in a prepare step after the flush, `publish_merges()` and `maybe_merge()`
  (`:819-821`), since a flush can set deletes on records with no body, and before
  `guarded(commit_files)` (`:822`). It maps every record whose facts are unknown, or that has new
  deletes and no body, skipping records whose `dead` equals `ref.doc_count` (the erase at `:4006`
  drops them), and takes `total_length` from `ref.live_length` when the record has no live-docs
  generation (`:4053-4057` keeps them equal). A host error throws and leaves the index as
  `refresh()` leaves it: flushed segments and installed merges in `segs_`, unpublished until the
  next `publish_point()`, nothing durable, `applied_since_commit_` unchanged, nothing fenced; facts
  and bodies the prepare installed are true and stay. Inside `guarded()` (`:2268-2276`) no input map
  remains. Recovery's fallback commit (`:4365`) maps the same way inside open, whose failure fails
  the open as today.
- **Commit with a marked record** (CT-R19). A record whose first map fails its checks carries the
  marker (D11), and every later commit's prepare throws `corrupt_segment`, unfenced, until `heal()`
  replaces it. `needs_heal()` (`:804-807`) reports marked records beside `damaged` ones; the host
  heals and commits again. Today the same damage fails the open.
- **Scheduling** maps each source before reserving storage. A source failing its checks is marked
  damaged and left out, as `:2858-2870` does for a finished merge, and is never swallowed as a host
  error. A host error drops the round with nothing reserved. Only `maybe_merge()` (`:786-793`)
  swallows it, beside `ENOSPC`, so `refresh()` and `commit()` complete and the next round retries;
  `merge()`, `force_merge()` and `force_merge_deletes()` rethrow, as the host asked for them. Each
  swallowed round counts on an atomic the host reads, as `media_retries()`: skipped merges pile up
  segments, each a map per read, which feeds the shortage, and step 2's catalog reads the count.
- **`apply()`** relies on CT-R5: the rollback restores the buffer and positions, then rethrows.
- **Readers** (`get`, `scan`, digest, search) get the error; the index changes only by D11's
  marker and the retry count.
- **No input map** runs in a destructor, a `noexcept` chore, or inside `guarded()`; output maps
  there never throw a host error. Debug builds assert both.

A get maps segments newest first down to its hit, all of them on a miss; a scan maps every segment
whose key range it reads (`:1918`).

### D10. Quiescence for warm and for cold

- **Warm** (`demote()`, under the lease): no buffered record (`buffered_records()`, `:3702`), no
  pending merge (`pending_`, `:4512`), no unsynced record, not closing. Posted deletes do not
  block it: a delete touches only the IO and its channel (`:3028-3030`), which demotion keeps.
  Nothing need be committed: demotion writes nothing (I2).
- **Cold** (the sweep's close): warm's predicate, nothing uncommitted (`applied_since_commit_`,
  `:4547`), and, after a non-blocking `collect_deletes()` (`:3063-3081`), no posted delete.
  Otherwise the sweep retries on a later pass. `close()` waits for posted deletes without a timeout
  (`end_merges()` → `end_deletes()`, `:2977-2980`, `:3103-3110`), which a chore must never do;
  with `posted_` empty it waits only for a reported task's tail, and debug builds assert no task
  is unrun.
- So an idle index reaches warm whatever its last call posted, and reaches cold once the
  executor has run its deletes. Today `posted_` shrinks only when an owner call collects
  (`quiescence_probe.out`: 5 pending after a commit, 5 after the executor ran them and after a
  check-in, 0 after `reclaim(0)`); the sweep's collect is that call. CT-R4 adds the reclaim queue
  and debt to cold's predicate in step 2.

### D11. Open checks names and sizes; a failed first map is marked

- **At open**, recovery already records every scanned file's size (`track`, `:4318-4319`). Open
  checks each manifest-named segment's existence and size against those records, with no system
  call: a missing file fails the open with `corrupt_index("missing file")` and a size mismatch with
  `corrupt_index("file size differs from the manifest")`, as `load_segment_of` reports them today
  (`:4461-4466`, `:4471`). Live-docs and metadata files are still read and checked at open
  (`:4485-4497`).
- **At first map**, the rest of `load_segment_of`'s checks run (`:4472-4484`: version, footer CRC
  against the entry, identity, document count, structures and versions against the manifest, whole
  when eager) through `read_checked`'s retry, and report as `corrupt_segment`.
- **A failed map is marked.** A record whose checks fail in both attempts gets the failed marker by
  compare-exchange from null; later reads throw `corrupt_segment` at once, `needs_heal()` reports
  it, twins carry it (D3), and `heal()` reports it and replaces the record. Host errors never mark.
- **Q3's condition** (the lead's 2026-10-18 ruling; its test is strata `8b834ee` on
  `perf/small-indexes`): one byte damaged at the segment's size in the term index, the footer,
  postings, or stored records, then a search, a get by key with its stored record, or a merge.
  In all twelve cases every path that reads the damage throws `corrupt_segment`, any other answers
  exactly, and `heal()` reports the segment. Step 1 ports them to main's sections with lazy
  segments (K4).

## Correctness under concurrency

**I1. A mapping dies only with its record**, under the lease, after the registry scan found no
protected view holding the record, and with no merge job holding it (shared-reader D4, D6). A body
or failed marker is installed at most once and never cleared, so a guard's segments stay mapped
through any demotion.

**I2. Demotion changes no file set.** Twins name the same files; `retain()` references files by
name (`:4145-4154`) and "a held file is never truncated" (shared-reader D7) is untouched.

**I3. One instance per directory**: one slot per key by the shard mutex, one owner across
processes by the directory lock, one performer per transition by the slot word.

**I4. Cold only when unpinned, quiescent (D10) and committed.** The slot compare-exchange excludes
a pin; D10 excludes merges, posted deletes and buffered records; "nothing uncommitted" excludes
lost work, since `close()` drops it (`:577`).

**I5. Demotion is invisible to reads**: the next view has the same position and digest.

**I6. Counters are exact at quiescent points**: each changes at the event it counts.

**I7. A slot outlives what reaches it.** A lazy map uses the slot's IO; a slot is freed only after
`close()` completes, so after every view is gone.

## Gate rows

Lead-run on `main` under `quiet exclusive`, extending `bench/idle_index` with `catalog` and `tier`
modes; branches make functional claims only. T1 to T4 are restated before they gate (CT-R9,
CT-R11); T5 to T7 hold for step 1.

| Row | Target | Procedure |
|---|---|---|
| **T1** memory per tier | Hot within 5% of 5.9 KB macOS, 8.1 KB Linux; warm restated from measured pieces (N1), 0 maps; cold ≤ 16 B (noise), 0 fds, 0 maps | `idle_index open ROOT 100000 shared tier {hot,warm,cold}` on 1-, 100- and 1,000-document fixtures and a 10-segment one, idle 5 s, metered as `idle-index.md`; the counting build for the heap |
| **T2** first query per tier | Restated per fixture and segment count (1 and 10), cached and dropped per row, on re-measured baselines with raw files (CT-R11) | 100,000 indexes forced to one tier, one `get` each in random order |
| **T2w** write after open | Reported, not gated: decides Q5 | the 10-segment fixture opened cold, one `apply` and a `commit`; the maps and the time of that first commit, against `eager_segments` |
| **T3** count far above the caps | No `ENFILE`, `EMFILE`, `ENOMEM`; the kernel's maps, descriptors and vnodes within caps; stock sysctls stated (CT-R11) | open for step 2 |
| **T4** Zipf over 1M | Restated to the chosen policy and to caps D6 allows per platform (CT-R10, CT-R11) | open for step 2 |
| **T5** sanitizers | Zero TSan, ASan, UBSan reports; no truncation under a mapping | step 1: one index; readers get and scan, some holding guards across `demote()` and releasing on other threads; writers apply, refresh, commit, merge on `pool_executor`, `heal()`; an index idle after posting deletes; faultfs mapping on, 4 KiB `reclaim_step` (truncate abort as oracle), and POSIX IO; faultfs `map()` failures on flush and merge outputs. Step 2 adds 64 indexes under a catalog capped at 8 open and 16 maps |
| **T6** schedules | Every read equals the model; generations never decrease; mutants die | 1,000 seeded schedules on `inline_executor` (shared-reader T2a's shape) over the reader's load, claim, validate, body install, read, release; demotion's twin swap, publish, scan, free. Mutants: body cleared in place; body stored without compare-exchange; loser reads the body it destroyed; facts written by a reader's map (under TSan); twin only bodied records; twin drops `deletes_dirty` (oracle: a reopen); demotion with a buffered record or a pending merge; close with a posted delete; a map inside `guarded()` |
| **T7** counter fidelity | Counters equal the kernel's at quiescent points | T5 paused every 1,000 steps: maps against `/proc/self/maps` or `mach_vm_region`, fds against `/proc/self/fd` or `proc_pidinfo` |

## First buildable step

**Step 1: cut 1 and `demote()`, without the catalog** (one developer, about a week). Done
criteria K1 to K8 are in the ledger (`docs/status/cold-tier.md`).

1. Record, body and failed marker (D2), the compare-exchange install, owner-written facts on the
   record, the atomic retry count; `options::eager_segments` on by default.
2. `segment_view::seg()`, strata's reads migrated; prospect's construction sites in a companion
   change.
3. D9 in code: entry reads switched to the entry and facts, output maps that keep the bytes,
   commit's prepare, scheduling's map and its swallowed-round count, `needs_heal()` over markers,
   each map point's failure rule with a faultfs test.
4. D11: open's name and size check, first-map checks, the twelve cases with lazy segments.
5. `basic_index::demote()`: D3 without the sentinel (every record twinned with its listed
   fields, a forced publication, `free_views()`) under D10's warm predicate.
6. Lazy segments become the default once CT-R5's rollback is on `main`.

Items 1 and 2 may start now; they change no behavior while `eager_segments` is the default. Then
step 2, the catalog (D4 to D6, T3 to T5); step 3, the sentinel; step 4, the idle core.

## Limits

- **Warm holds one descriptor and one vnode** for the lock, so macOS warm caps near 100,000, not
  at memory (Q1).
- **Misses cost an open.** At θ = 0.99 over 1M with 110,000 open, 22% of requests miss under
  one-bit CLOCK (CT-R10). A macOS open is 0.4 to 1.1 ms p50, half its 54 syscalls repeat
  `fstat`/`geteuid`, so a miss-heavy load spends cores on opens; T4's opens per second show it.
- **A synchronous `get` on a warm index blocks for one map per segment it reads** (D9), estimated
  33 to 50 µs each from the old base's lazy segments; tasks map at their residency point.
- **The first commit after an open maps** each segment whose facts are unknown (D9), unless a
  manifest field carries them (Q5), and a segment whose first map failed blocks commits until
  `heal()` (D9).
- **Corruption surfaces at first use**, as `corrupt_segment` to the reader (D11).
- **Out of scope**: cold under `strata::group` (needs the journal checkpoint; Track C).
- **FreeBSD is unmeasured**; it has `kern.maxvnodes`, so the macOS counters apply until measured.

## Notes

**N1. Warm's budget, corrected (CT-R9).** `warm_bytes_probe.out` (one-document indexes on
`PosixIO`, requested heap): hot is 5,394 B in 34 allocations; retiring and freeing the view
returns 304 B; the empty active buffer returns 920 B (`sizeof(buffer)` 896); the segment objects
1,866 B in 14; 2,304 B stay (`basic_index` 1,320 B, the publication 136 B, the reader state 96 B,
the manifest and file maps). So `idle-index.md`'s "the `view` 920" is the empty active buffer, its
"publication 208" is the view (`sizeof(view)` 208), and its "reader state 136" is the publication;
cut 2's 1,360 B saving rests on those labels. The review's arithmetic, not a measurement: warm as
D3 states it, with a record of about 200 B per segment and cut 3's 850 B, keeps about 2.6 KB of
heap per one-segment index, about 1.7 KB with the empty buffer dropped (step 3), both before the
130 B slot, `PosixIO` and allocator slack; step 1's demoted index, which keeps a view and the
buffer, about 3.7 KB. T1 restates warm from the measured pieces per segment count before it gates.

## Open for step 2 and step 3

From the review, not solved here; each is required before the step named.

- **CT-R4 (step 2).** `close()` drops the reclaim queue and frees the storage permit for bytes
  still on disk (`:602`, `:4530`): 5 files queued, 6,574 B on disk and 7,093 B on the budget before
  close; 0 B on the budget after; the reopen queues all 5 again. Cold also needs an empty queue and
  no debt, or the catalog keeps a closed index's bytes on the budget.
- **CT-R6 (step 2).** The `closing` fallback makes pinners wait on a guard holder without bound;
  cold only from warm after a scan finds no retired view, and a synchronous pinner gets
  `too_many_open`.
- **CT-R7 (step 3).** The sentinel reads as an empty index: `acquire()` guards any non-null pointer
  (`hazard.hh:119-130`, `index.hh:1164-1168`); it must throw `index_unpublished` or publish, never
  be in `pub_->current`, never be retired; the warm-to-hot build skips refresh's flush and merges.
- **CT-R8 (step 2).** Eviction is not tied to the resource over the cap: per-slot counts of maps,
  descriptors and vnodes, reservation by `fetch_add` before the system call, returned on failure
  or a lost compare-exchange.
- **CT-R10 (step 2).** One-bit CLOCK at T4: 0.782 at θ = 0.99 and 0.954 at θ = 1.2, against
  targets 0.80 and 0.95; LRU 0.775 and 0.952; S3-FIFO 0.817 and 0.961; a perfect frequency cache
  0.837 and 0.967. A frequency-aware policy with one counter store per hit, chosen by T4.
- **CT-R11 (T3 and T4 before step 2; T1 and T2 before measuring).** T2's hot 10 µs is already
  missed (10.2 µs at 1,000 documents on macOS); T3's 10M is 120 to 160 GB and hours of opens;
  T4's 10,000 hot and 110,000 open exceed macOS's vnode soft cap (120,000 against 105,267); T4's
  p99 compares 8 threads at 22% misses to a one-thread latency.
- **X-POOL (step 2, lead).** Xapiand's `DatabasePool` (`src/database/pool.cc:1096-1140`) is an
  LRU of open shards sized from `RLIMIT_NOFILE / FDS_PER_DATABASE` (`src/main.cc:358`): it closes
  an endpoint idle 60 s once the LRU is over capacity, any endpoint idle an hour, and has no warm
  state. Once the catalog lands, residency belongs to strata alone; the pool keeps checkout and
  writer ownership and loses its eviction and its descriptor arithmetic (callers migrated, no shim).
- **CT-R12 (step 2).** Descriptor caps take the soft `RLIMIT_NOFILE`, raised to the hard limit
  at construction: macOS's default is 256 and systemd's 1,024, where D6's formula gives 0.
- **CT-R13 (step 2).** Slot orders: unpin stores the time before the decrement; `demoting`'s
  store and waiter resumption under the shard mutex; the sweep reads a slot's fields only after its
  compare-exchange. A pin is three or four read-modify-writes; the hottest key at θ = 0.99 takes
  6.5% of requests and the top 10 take 19.2%.
- **CT-R15 (step 2).** Reuse `hash_slots` (`:226-307`) for the shard table; a destroyed catalog
  leaks closing slots' IOs with their locks; key-to-directory resolution must be injective;
  debug builds check a pin at every call on a catalog index.
- **C4.** When durability-modes' checkpoint batch lands, cold's predicate gains "no view the owner
  keeps".

## Questions for the owner

Q1 to Q4 are ruled (the lead's answers, upheld by the review).

**Q1. Should warm drop its descriptor?** Ruled (a): keep the per-index lock for steps 1 to 4; one
lock per partition is step 5 with its own review.

**Q2. Does the catalog belong in strata core?** Ruled (a): core, the host supplying caps, timer
and key-to-directory resolver.

**Q3. Segment checks at first map?** Ruled (a), with `eager_segments` for tools, `check` and
installs, on D9's failure rules, D11's open check and twelve cases, and CT-R5 on `main` first.

**Q4. A summary per cold index?** Ruled (a): none.

**Q5. Should the manifest carry each segment's facts?** Ruled (a) for step 1, decided on numbers
before step 2. (a) No: the first commit after an open maps each segment whose facts are unknown
(D9), once per open. (b) Yes: `segment_ref` gains `structures`, `version` and `total_length` (16 B
per segment; a format change, open until release). Commits then map only records with new deletes
and no body (`live_length()`, `:4032`, `:4102-4107`), and a damaged segment no longer blocks
commits (D9, commit with a marked record). T2's write-after-open row decides it.

## What the recheck should look at

C5's items: CT-R2's output maps and `install()`'s copies in D9; CT-R17 in D2; CT-R18 in D3;
CT-R19 and CT-R20 in D9; the prepare's outcome wording in D9; C6 in the ledger.
