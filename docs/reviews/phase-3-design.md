# strata review: Phase 3 design

- Date: 2026-10-12.
- Reviewed: `docs/design/phase-3.md` at strata `474b29c`.
- Checked against:
  - PLAN's Phase 3 scope and gate, Track C's ordering, and Phase 3R.
  - ARCHITECTURE (Data model, Write path, Segment format, Manifest and commit, Read
    path, Concurrency and async, I/O, Libraries), REPLICATION (Determinism contract,
    Write path, Checkpoints and repair, consistency modes), and BENCHMARKS' indexing
    rows.
  - `docs/design/phase-2.md`, the Phase 2 gate review and its re-check (C1–C11), and
    the Phase 2 gate report, at `474b29c`.
  - `include/strata/{format,writer,reader,search,scorer}.hh` at `474b29c`.
  - `cluster/completion/io.h` and the journal's test model,
    `cluster/test/journal_test.cc`, at cluster `ca82aad`; `fusefilter` at `b4f4ce1`.
  - Detent's `docs/LIBRARY_BOUNDARIES.md` at `d3925ef`.
  - Xapiand's docid, version, and id-term handling (`src/database/shard.cc`,
    `handler.cc`, `utils.h`) at `361a2ebf6`.
- Checked by reading the files above. Nothing was built or run.

## Verdict

**APPROVE WITH REQUIRED CHANGES.**

The structure is sound:

- One writer per shard, immutable segments, live-docs generations as side files, and a
  checksummed manifest whose rename is the commit point.
- Fail-closed recovery with an explicit operator path.
- A live version map in front of per-segment key lookups.
- Tiered merges that preserve docids.
- Metadata committed through the manifest.
- REPLICATION's deterministic core interface, and a digest independent of
  segmentation.
- faultfs and the memory budget kept inside strata, so nothing in `cluster` or Detent
  changes before the split is coordinated.

It is not yet safe to implement:

- Under its own fault model, a commit can leave an index that fails closed after a
  clean power loss (D1).
- Its statistics can make term weights negative, which pruning does not allow (D13).
- Nothing carries deletes that arrive during a merge into the merged segment (D11), and
  re-adding a deleted key can resolve differently by refresh and merge timing (D8).
  Both make replicas diverge.
- Its tie rule does not order hits by docid across segments (D14).
- It omits operations Xapiand uses (D7), where versions live on disk (D16), how one
  index indexes on several threads (D18), and how a full disk is handled (T5).

D1–D20 land in the design before implementation starts, except D5 and D11, which land
before `install` and merge code, and D18's measurement, which lands before the writer's
buffer code. A revision check covers D1–D20 and T1–T9. Implementation also waits for
the Phase 2 gate's approval: G5's quiet-host re-run is pending, and the fixes for the
re-check's G1 residual and G6 (`e5fafec`) await review.

## Required changes

### Commit and recovery

1. **D1. The commit's barrier order.**
   - The design states the protocol twice, differently. The convention gives every file
     its own rename and directory fsync (`phase-3.md:19-20`): safe, but one barrier per
     file, and refresh writes segments without any fsync (`138-140`). The commit text
     has one directory fsync, after the manifest's rename (`54-56`).
   - Under the design's model, a rename is durable only after its directory's fsync
     (`204`), and a create changes the directory the same way. With one barrier at the
     end, power loss can keep the manifest's rename and drop a new file's name. The
     highest manifest then names a missing file, and recovery fails closed (`57-60`) on
     an index that only lost power.
   - Required order:
     1. Write each new data file under its final name and fsync it.
     2. Fsync the directory.
     3. Write the manifest under a temporary name and fsync it.
     4. Rename it to `manifest-(N+1)`.
     5. Fsync the directory. The commit is acknowledged here; retention runs only after
        this barrier succeeds.
   - Only the manifest needs a temporary name. A data file no manifest references is an
     orphan in any state, provided names are never reused (D3).
   - Cost: two directory barriers per commit. The gate report states commit latency
     beside the VM's measured fsync latency.
2. **D2. Failed barriers.**
   - A failed fsync leaves the file's contents unknown: Linux can mark the dirty pages
     clean and report success on the next fsync. The writer never retries fsync on that
     file and never trusts it.
   - A segment flushed at refresh no longer has its buffer at commit time, so it cannot
     be rewritten. A failed fsync of any file in a commit fences the writer, and
     reopening selects the last durable manifest. Later transactions return from the
     journal (Phase 3R) or Xapiand's WAL (Phase 7a); standalone, they were never
     acknowledged.
   - A rename or directory fsync that reports failure may still have taken effect, as
     the journal's tests assume (`journal_test.cc:398, 1339`). The writer does not
     acknowledge, runs no retention, and fences; reopening settles which manifest is
     current.
   - ENOSPC from a write, before any fsync, is recoverable: a flush keeps its buffer
     until its file is complete, removes the partial file, and retries later (T5).
     Today's `file_output` and `file_spill` use stdio and throw an untyped
     `runtime_error` (`writer.hh:106-163`), so ENOSPC is indistinguishable from EIO
     (D19).
3. **D3. Recovery: which manifest, and opening an older one.**
   - "Opens the highest `manifest-N` whose CRC verifies" and "if the highest manifest
     fails, recovery fails closed" (`57-60`) read as two rules. One rule: recovery
     selects the highest final-named manifest, and temporary files are never
     candidates. If its CRC fails, or a file it references is missing or differs in
     size or footer CRC, the open fails closed and changes nothing on disk. Under D1,
     power loss alone never produces that state (T1).
   - `recover --to N` (`60-61`), and group recovery, which opens the manifest the
     journal's checkpoint references (ARCHITECTURE, Manifest and commit), open N while
     higher manifests exist. The next commit writes `manifest-(N+1)` over a different
     history; if `manifest-(N+2)` exists, the next open selects it and resurrects the
     abandoned history. That is an inconsistent open.
   - Required: an open of an older manifest durably retires every higher one (deletes
     it, or renames it to a name recovery never selects, then fsyncs the directory)
     before its first write. Every counter (manifest generation, segment id, live-docs
     and metadata generations) starts above the highest value present on disk, not the
     selected manifest's.
4. **D4. Retention, pins, and orphans.**
   - "Retained" is undefined (`62-65`): how many manifests stay, how an old manifest is
     deleted, and whether pins survive a restart.
   - Required:
     - The retained set is the current manifest, the pinned ones, and a stated number of
       predecessors (I14). A manifest leaves it when its file is deleted, before its
       exclusive data files are.
     - Pins survive restarts. Under groups the journal holds the checkpoint references,
       so the core receives the pinned set at open, or records it durably; otherwise
       orphan collection at open deletes a pinned checkpoint's files.
     - Orphan collection runs only after a successful open, never while failing closed,
       under the owner lock, and only on names in strata's patterns.
   - Snapshots: the reference-counted snapshot (`135-137`) is not ARCHITECTURE's
     per-core handle, published by message with no shared refcount (Read path, 1). A
     segment's file is mapped or held open from the moment it joins a snapshot, so
     unlinking it is safe on Linux, macOS, and FreeBSD, and retention never reads
     snapshot counts. Open handles decide only when space returns, which storage
     accounting counts (T5). The design states this ownership model.
5. **D5. `install`.**
   - `install(checkpoint)` "replaces the index with a checkpoint's files and manifest"
     (`183`). The checkpoint's files carry the source replica's segment ids, which can
     name other files in the target directory, including ones its current manifest
     references.
   - Required:
     - Incoming files take fresh local names from the target's counters (D3), or land in
       a fresh directory switched by a rename and a parent-directory fsync.
     - They are verified whole before the switch.
     - The installed manifest goes through D1 at a generation above every existing one,
       and the old files become orphans.
     - The buffer, pending live-docs, and the live version map are discarded;
       `lastdocid`, the schema version, and the applied position come from the
       checkpoint.
     - The index UUID belongs to the shard: every replica shares it and `install` adopts
       it, as with Xapiand's database UUID (FEASIBILITY, "Database UUID").
   - A crash at any point opens the old index or the installed one, never a mix (T1).

### Formats (before the writer)

6. **D6. Formats the design leaves open.**
   - **Live-docs file:** a header (magic, version, segment id, generation, document
     count), the encoding (a bitset, or sorted dead ordinals when sparse), the live count
     and live total length, and a CRC. The reader rejects unknown versions and checks the
     live count against the bits.
   - **Manifest:** ARCHITECTURE's manifest carries total length
     (`ARCHITECTURE.md:142-144`); the table (`40-52`) does not. The design states the
     byte order and integer encoding (ARCHITECTURE's reuse table names `varint` for
     manifest integers), the temporary name, the schema version (D7), and the key prefix
     if D16 adopts one.
   - **Segment:** the `pk_filter` and versions (D16) sections bump the format, with a
     fixture and a test that version 9 is rejected or read (Phase 2 gate review,
     ruling 4).
   - **Metadata:** `termdict` cuts blocks at 1 KiB and 48 entries, and Xapiand's schema
     value exceeds a block. The design states that a large value takes a block of its
     own, or uses a plain sorted file (I8).

### Semantics and determinism

7. **D7. The operation set and docids.**
   - The design has operations by key only, with docids assigned in stage 1 (`79-81`,
     `107`). Xapiand also addresses documents by docid:
     - Numeric ids map to docids (`QN` terms, `shard.cc:1528, 1566`), and the write
       replaces at that docid (`wdb->replace_document(info.did, doc)`, `1642`).
     - Its WAL records operations by docid (FEASIBILITY, "Stable docids").
   - Replacing at a deleted or never-used docid creates the document there, below
     `lastdocid`, reusing the docid. "Never reused" (`13`) holds only for docids the
     engine assigns.
   - Required: the operations apply accepts:
     - Add with an engine-assigned docid, `lastdocid + 1`.
     - Replace and delete by key.
     - Replace and delete by docid; a replace at an absent docid creates it there, and
       `lastdocid` becomes the larger of the two.
     - Set and delete metadata.
     - Set the schema version, which the core holds (D6).
   - The design states what happens when a key and an explicit docid disagree (a
     deterministic rejection), and the invariant: at most one live copy of each docid and
     of each key.
   - Ids that depend on `lastdocid` (Xapiand's numeric auto ids, `shard.cc:1539`) are
     resolved by the leader before it proposes (REPLICATION, Determinism contract), so
     they reach apply as explicit docids.
8. **D8. Keys and versions independent of timing.**
   - The live version map keeps a deleted key only until refresh (`98-100`). After that,
     a lookup reaches the segments, where a deleted copy survives or not depending on
     merges. Re-adding the key then finds the old version or none, and the old docid or
     a new one, by refresh and merge timing; replicas diverge.
   - Required:
     - Lookups consider live copies only. A key whose copies are all deleted is absent:
       re-adding it takes a new docid and version 1. That is Xapiand's behavior: a delete
       removes the document's `V` term, and `replace_document_term` gives a key with no
       posting `get_lastdocid() + 1` and version 1 (`shard.cc:1591-1596, 1625`).
     - An expected version of 0 requires absence (Xapiand's `"\x80"`, `1596`); any other
       must equal the current version; a mismatch rejects with the current version; no
       expectation is unconditional.
     - The version map is a cache of writer state, never a source of truth: clearing it
       at refresh or at flush changes no verdict.
9. **D9. Transactions: cancel and visibility.**
   - The Phase 2 writer has no `truncate` (`117-118`), and undoing a transaction takes
     more than truncation: `max_wdf` and the maximum position count are maxima,
     `counts_are_wdf` flips once and copies the wdf stream, and new terms sit in an
     open-addressing table and an arena (`writer.hh:241, 246-251, 409-428`).
   - Stage 1 already decides every verdict (`114-116`). Required:
     - Stage 1 evaluates the whole transaction on an overlay (key → docid, version,
       deleted) and passes it to stage 2 only when every check passes.
     - A rejected transaction leaves no trace in the buffer, the version map, pending
       live-docs, or `lastdocid`. It consumes no docids, so Xapiand's
       `_doc_del = lastdocid − doccount` stays as it was.
     - Validation that can reject moves into prepare or stage 1, so stage 2 cannot fail
       for a deterministic reason: a term repeated within a document (`add` throws
       today, `writer.hh:235`), term length, position and length overflow, and
       stored-record size.
     - `truncate` goes. A host cancel (Xapiand's `cancel_transaction`, `MSG_CANCEL`) is
       a transaction the host never submits: transactions reach apply whole.
   - Visibility: refresh, budget flushes, commits, and checkpoints cut at transaction
     boundaries. A transaction larger than the buffer may flush part of itself, but that
     segment is published only with the transaction's end.
   - "Batch" names two things: the atomic transaction and the group-commit log entry
     (REPLICATION, Write path). A version conflict cancels its transaction, not the
     entry. The applied position names a transaction boundary, and apply ignores
     positions at or below `applied_position()`, so a replay that starts early applies
     nothing twice.
10. **D10. Local failures never become verdicts.**
    - Apply's verdicts are the deterministic ones: version and schema-version mismatches,
      and document validation.
    - ENOSPC, EIO, and memory exhaustion in flush, merge, or commit never cancel or
      reject a transaction apply accepted. The core stops applying until the condition
      clears, or fences and repairs (D2); standalone admission may reject new
      transactions before stage 1 (T5). Otherwise a voter that ran out of disk diverges
      from one that did not.
    - Memory pressure suspends stage 1 (D17); it never rejects.
11. **D11. Merges: deletes during a merge, and a bound on deletes.**
    - A merge reads its sources as of its start and never blocks the writer
      (`152-156`). Deletes applied to a source while it runs set bits in that source's
      pending generation (`84-85`), which the merged segment never sees; when it replaces
      its sources, those documents come back. The outcome depends on merge timing.
    - Required:
      - A merge records the live-docs generation it read from each source.
      - Installing the merged segment runs on the owner core between transactions. Every
        document deleted in a source after that generation is found by docid in the
        merged segment and marked dead in its first pending generation, as Lucene's
        `IndexWriter` does when it commits a merge.
      - A buffer flushing in the background is handled the same way: deletes after it is
        frozen go to the new segment's first pending generation.
    - The policy counts deletes against size (`148-151`), but a segment at the 5 GB
      maximum never merges again, so its deletes accumulate without bound: statistics
      drift (D13), count shortcuts stay off (I3), and space is not reclaimed. The policy
      rewrites such a segment alone once its deleted fraction passes a stated bound, as
      Lucene's tiered policy does past `deletesPctAllowed` (I1).
12. **D12. The digest, and what it proves.**
    - The digest (`190-194`) omits state that decides future verdicts, `lastdocid` and
      the schema version, and the document length, which the host passes to `add`
      (`writer.hh:224`) rather than the writer deriving it. Two replicas can agree on it
      and assign different docids next.
    - Equal digests at the end of a run do not show that apply decided the same things
      on the way: a replica that rejected a replace and then applied a delete of the same
      key converges. Tests also compare the sequence of verdicts (accepted; rejected with
      the current version; assigned docids), and the digest after every transaction on
      small runs.
    - "In docid order" over each document's (term, wdf, positions) needs term lists,
      which are opt-in; from postings it means inverting the index. Required: an
      order-independent multiset hash, the sum modulo 2^64 of a specified hash over each
      (docid, term, wdf, positions) and each (docid, version, length, stored record),
      plus `lastdocid`, the schema version, and the metadata entries. One term-major pass
      over each segment's postings computes it, skipping dead ordinals, with no sort and
      no term lists. A sum, unlike XOR, does not cancel a duplicated copy. The hash and
      byte layout are specified, so digests compare across platforms.
    - The pass asserts at most one live copy per docid and per key, and every docid at
      or below `lastdocid`.
    - Revision is a local commit counter and stays out of the digest; a state's
      replicated identity is its applied position.

### Statistics and ranking

13. **D13. Statistics under deletes.**
    - N and total length come from live documents, while document frequencies keep
      deleted ones (`124-126`), so df can exceed N: a term in every document, after every
      document is replaced once and before a merge, has df = 2N.
    - Both weights then go negative. In Xapian BM25, tw = (N − df + 0.5)/(df + 0.5) is
      below 0, `tw * 0.5 + 1` (`search.hh:76`) lands in (0.5, 1), and its logarithm is
      negative. Lucene's log(1 + (N − df + 0.5)/(df + 0.5)) (`62`) is negative for
      df > N + 0.5. Pruning assumes non-negative contributions (`sum_upper`, `102-105`;
      MaxScore's essential split).
    - The mix also drifts further from Xapian than one population does. With N = 1,000,
      df = 100, and 500 replaces of uniformly chosen documents, 50 holding the term:
      Xapian's exact tw is 900.5/100.5 = 8.96; counting deleted documents everywhere
      (N = 1,500, df = 150) gives 8.97; the design's mix (N = 1,000, df = 150) gives
      5.65.
    - "As in Lucene and Xapian" (`126`) holds for neither. Lucene keeps deleted documents
      in maxDoc, docCount, sumTotalTermFreq, and docFreq alike until merge. Xapian's
      glass backend reads a deleted document's term list and decrements its terms'
      frequencies, so its statistics are exact.
    - Required: scoring statistics come from one population. Term lists are opt-in, so
      df cannot drop deleted documents; scoring's N and total length include them until
      merge, as Lucene's do, and df ≤ N holds by construction. The live count and live
      total length serve what the host reports: Xapiand's `doccount`, `av_length`, and
      `_doc_del = lastdocid − doccount` (`handler.cc:1902-1911`).
    - Scores then differ between replicas until their merges converge; logical content
      does not.
14. **D14. Ties between equal scores.**
    - Visiting segments "in docid order of their first document" (`16-18`) does not make
      arrivals ascend by docid. A replaced document sits at a late ordinal of a newer
      segment, whose docids interleave with older segments'. The collector rejects
      `score <= threshold` and breaks ties by ordinal (`search.hh:168, 172`), which is
      right only for ascending arrival (C2).
    - Required:
      - The collector orders hits by score descending, then docid ascending, and its
        rejection compares that pair.
      - Scorers skip only blocks and windows whose bound is below the k-th score, not
        equal to it, except past a point where every docid ahead exceeds the k-th hit's
        (D15 makes that an ordinal within a segment).
      - A test: duplicate documents (equal scores) replaced across several flushes and
        merges, pruned top-k equal to a brute-force sort by (score, docid), under both
        schemes, at k = 1, 10, and 100.
15. **D15. Ordinal order within segments, and documents deleted in the buffer.**
    - Merges write docid order (`152-153`); flushes write arrival order. This is a
      format property, decided now. Recommended: docid order in every segment, sorted at
      flush.
      1. Ordinal order is docid order inside each segment, so D14's exception applies
         within a segment and only hits from different segments compare docids.
      2. The docids section becomes monotone. Docid → ordinal is a search over it, with
         no second sorted array (`14-15`, C1), and it compresses: Elias–Fano takes about
         2 + log2(span/count) bits per document, where kind 1 takes 64.
      3. Docid-order walks are ordinal walks merged across segments: Bool weighting, the
         digest, and Xapiand's dump resume with `skip_to`.
      4. An append-only buffer is already sorted, so the append-only rows pay nothing;
         the replace-heavy row measures the cost.
    - A flush drops documents deleted inside the buffer rather than writing them dead
      (`86-87`): they were never visible. Every segment then holds at most one copy of
      each docid and each key; a key term has df 1 in every segment, its term block
      inlines the ordinal (Phase 2's single-posting form), and a key lookup never opens
      postings. Segments are also born without deletes and keep their count shortcuts.
    - Either way, the docids section, its docid → ordinal lookup, and its validation at
      open (strictly increasing, or unique) are specified now.
16. **D16. Versions on disk, and the key term.**
    - Versions are checked "from the map or the segments" (`101-103`), but no segment
      section stores them; Xapiand keeps one in a `V` term, a value slot, and the stored
      record (`shard.cc:1625-1633`). Required: a versions section, one value per ordinal,
      bit-packed from a minimum, or an equivalent, in D6's format bump.
    - The key term. Xapiand already indexes a unique id term in every document, prefixed
      `Q` (`utils.h:61`; `shard.cc:1528`). A reserved `\0pk:` term (`91-93`) duplicates
      it, needs a namespace the host cannot write into, and must be hidden from all-terms
      and prefix enumeration. Recommended: the host designates its unique term's prefix
      per index, recorded in the manifest, and strata indexes nothing extra. Either way,
      the design states the choice, how stage 1 enforces it, and the hiding.
    - Dictionary size: one key term per document adds on the order of 100 MB to the 5M
      corpus's `terms` section (139 MB at C11's per-term averages of 8.5 bytes of key and
      19.2 of value), against 100.6 MB today. C11's target of 1.5× `.tim` states whether
      key terms count, against a Lucene index that indexes its id if they do.
    - "Document frequency 1 per live copy" (`93`) is 1 per copy, live or deleted, until
      merge; D15's flush rule keeps it at 1 per segment.

### Resources and libraries

17. **D17. The memory budget's accounting and backpressure.**
    - The budget flushes the largest buffer once the node total, reported after every
      batch, passes it (`169-171`). The must-win row is peak RSS within the node budget
      plus 10% (BENCHMARKS, Indexing). Two things break it:
      1. Unaccounted memory: frozen buffers while they flush, the flush's sort and spill
         buffers, the live version map, pending live-docs, merge buffers, and filter
         construction (`fusefilter` builds from a 64-bit hash per key, plus scratch, for
         every key of a merged segment).
      2. No hard limit: under ingest faster than flushes, buffers grow past the budget.
    - Required: every category is charged as it is allocated, not after the batch.
      Flushing starts below the limit by the in-flight flush size. At the limit, stage 1
      suspends until a flush settles. The version map counts against the budget and
      forces a flush past a stated share, as Elasticsearch counts its version map against
      the indexing buffer.
18. **D18. Writer parallelism and the N-thread row.**
    - The gate has multi-thread must-win rows (PLAN; BENCHMARKS, "Throughput,
      N threads", to one committed index), and the design has one inversion buffer per
      index. Phase 2 indexes 8,490 docs/s on 5M with analysis on the same thread; Lucene
      and Tantivy reach about 14,000 with their multi-threaded writers (FEASIBILITY,
      Ingestion throughput).
    - ARCHITECTURE builds partitioned writers when single-core inversion measures below
      twice the target (Write path, 12). Required: the design states the N-thread target,
      the stage-2-only rate of the Phase 2 writer measured on a quiet host, and the
      decision.
    - If one core is not enough, parallel inversion is designed in this phase, because it
      shapes buffers, delete routing, and atomic publication. One shape: stage 1 stays
      sequential on the owner core, so verdicts, docids, and keys stay deterministic in
      one place, and dispatches transactions to K inversion buffers that flush their own
      segments; each buffer takes contiguous docid runs, and a refresh cuts every buffer
      at one transaction boundary.
19. **D19. faultfs: the interface and the model.**
    - **Interface.** ARCHITECTURE routes segment and manifest I/O through `completion`'s
      `IO` "so one model covers them" (Libraries, `faultfs`). That interface exists:
      `kronuz::journal::IO` in `cluster/completion/io.h` has `create_exclusive`,
      `write_at`, `sync`, `replace`, `sync_directory`, `read_at`, `remove`,
      `scan_directory`, and the owner lock (`63-134`). The design's new `io`
      (`198-199`) duplicates it. Required: strata includes that header, unchanged, at a
      pinned commit, and faultfs implements it; the flush's output and spill files
      (`writer.hh:106-163`) write through `File`. Consuming the header changes nothing in
      `cluster`; any addition to it, such as preallocation or free space, is a
      `completion` change and goes through Detent. Phase 3R then runs the journal and the
      segment files under one model, which its gate needs.
    - **Model.** The journal's test model is not the one the design describes. It keeps a
      whole visible and a whole durable copy of each file and of the namespace
      (`journal_test.cc:45-86`): unsynced data is lost entirely, the namespace survives
      all or nothing (`power_loss`, `78-85`), and faults are untyped, before or after
      their effect (`56-63`). It has no torn sectors, no surviving unsynced data, no
      partial namespace, and no ENOSPC from a capacity. faultfs is a new, stronger model
      that starts in strata; "the same model" (`208-209`) is replaced by that statement.
      Moving it into the split repositories, with the journal's tests adopting it, is the
      step coordinated with Detent.
    - The model specifies:
      - Unsynced data lost, kept, or torn per 512-byte sector, and file growth that
        persists as zeroed sectors.
      - Any subset of unsynced creates, renames, and unlinks surviving; a rename is
        atomic, leaving the old binding or the new one, never neither.
      - A failed fsync that drops unsynced data, with a later fsync succeeding (D2).
      - Failures reported before or after their effect, typed `EIO` or `ENOSPC`; ENOSPC
        from a capacity; short writes.
      - Reads from the model's bytes: the reader takes a `string_view`
        (`reader.hh:58`), so faultfs hands it the file's visible content.
20. **D20. The memory budget and Detent.**
    - PLAN has strata extract `ledger` if split step 5 has not landed
      (`PLAN.md:139-140`); the design does not extract it (`172-174`). Extraction changes
      code Detent owns (LIBRARY_BOUNDARIES, step 5: `journal::Admission`, `DiskLedger`,
      and `StorageLedger` become wrappers over the extracted core), so the design's
      choice is the one the coordination rule allows. PLAN changes to match.
    - `strata::memory_budget` is then a sixth copy of the idea LIBRARY_BOUNDARIES counts
      five times ("One idea, implemented five times"). It takes ledger's core shape, a
      capacity, move-only permits, explicit settlement, and a pressure signal, so the
      swap is mechanical. The design records the catalog check: `journal::Admission` is
      single-executor while the budget spans owner cores, and Detent's `StorageLedger` is
      not a library.
    - Coordination happens before `memory_budget` is written, not at the swap: ask the
      Detent agent, or German, for step 5's timing and API, and register strata as a
      consumer of `completion/io.h` (D19) so the split keeps it building.
    - Storage admission, ledger's other use in ARCHITECTURE's reuse table, is T5's
      mechanism. It reuses `journal::Admission` unchanged if a per-index owner core fits
      its single-executor model; otherwise it joins this coordination.

## Decided during implementation

Each within the constraint stated.

1. **I1. Merge policy values.** The floor, merge width, segments per tier, and maximum
   size equal the pinned Lucene 10.4.0 `TieredMergePolicy` defaults, read from its source
   rather than assumed, or T3 sets them explicitly for both engines; likewise D11's
   deleted-fraction bound.
2. **I2. Key lookup cost.** A per-segment minimum and maximum key ahead of the filter; a
   create-only flag that skips the lookup for ids the leader knows are new, as
   Elasticsearch does for generated ids; stage 1's lookups through residency, suspending
   instead of faulting, since stage 1 runs on the owner core. `fusefilter` measures
   5.6 ns per lookup at 1M keys and 14.6 ns at 10M, so the filters' cost per insert grows
   with the segment count; the gate report states it.
3. **I3. Counts under deletes.** C3's rule as written (`128-131`). Subtracting deleted
   matches instead of iterating is allowed where the deleted set is small, with a test
   per shortcut. A term matching every document still counts, as the live count
   (`search.hh:221`). Live bits AND into each window's match bits.
4. **I4. Segment order** for pruning: largest first, or by bound. D14 makes any order
   correct.
5. **I5. The version map** clears at flush or at refresh. Linearizable get-by-id is
   served from it (REPLICATION, read consistency), so an entry locates an unrefreshed
   document in the buffer or an unpublished segment, not only its version.
6. **I6. The docids encoding** under D15: Elias–Fano, or bit-packed deltas with a sparse
   index.
7. **I7. Stored frames** copy raw when none of their documents is deleted and they stay
   contiguous in the merged order; the frame index's first ordinals are rewritten.
8. **I8. The metadata file:** `termdict` with large values in blocks of their own, or a
   plain sorted file with a CRC.
9. **I9. Revision** counts commits with logical changes; a commit that only installs
   merges keeps it. Xapiand's WAL replay keys on revision (FEASIBILITY, "Revision").
10. **I10. Where stage 2 runs:** in budgeted slices on the owner's reactor, or on writer
    threads (ARCHITECTURE, Concurrency and async), within D18's decision.
11. **I11. The applied position's second component** stays only if commits fall between
    transactions of one log entry (D9).
12. **I12. `fusefilter`** is pinned at a published commit (local `b4f4ce1`, already in
    `cpp-libraries`), with the hash from key bytes to its 64-bit keys specified in the
    format.
13. **I13. The header's 16-byte segment id:** the 64-bit id, what fills the rest, and a
    check against the manifest entry at open.
14. **I14. The number of retained predecessors** (D4).

## Gaps against the gate

1. **T1. Power-loss injection.**
   - "Acknowledged" is defined. Phase 3 has no journal, so a transaction is acknowledged
     when a `commit()` covering it returns.
   - "Inconsistent open" is defined: an open whose digest matches no commit point from
     the last acknowledged commit to the last attempted one, or a fail-closed open after
     power loss alone. Otherwise failing closed passes the gate. Injected corruption is
     tested apart and must fail closed.
   - The workload covers adds, replaces, and deletes by key and by docid; rejected
     transactions; metadata; refreshes; budget flushes; merges with concurrent deletes;
     commits; `checkpoint`; `install`; and `recover --to`.
   - Crash points include recovery itself (manifest retirement, orphan collection) and a
     second crash during recovery. Each crash samples D19's choices. Seeds are recorded,
     and a failing seed replays deterministically.
2. **T2. The digest.** Two replays, and indexes built under different schedules: a flush
   after every transaction, and none; a merge whose sources lose documents while it runs
   (D11); a refresh between a delete and a re-add (D8); a merge that drops a deleted copy
   before its key or docid returns (D7); replay after crash recovery and after `install`.
   Digests and verdict sequences are equal (D12).
3. **T3. Write amplification.** BENCHMARKS defines it: bytes written over final index
   bytes. "Equal segment budgets" is defined: the same indexing buffer, merge width,
   segments per tier, floor, maximum size, and deleted-fraction bound for both engines;
   one corpus in two workloads, append-only and 50% replaces; bytes written counted the
   same way for both (process write counters, or summed sizes of files written), commits
   included.
4. **T4. Indexing rows.**
   - PLAN ratchets single-thread indexing to ≥ 1.0× Tantivy in Phase 3
     (`PLAN.md:99-101`); the design's gate omits it. Phase 2 measured 0.87×, and Phase 3
     adds key lookups, version checks, deletes, budget flushes, merges, and commits to
     that path. The design names where the margin comes from.
   - The N-thread row's target (D18).
   - BENCHMARKS' must-win rows that Phase 3's features create: replace-heavy (the
     Xapiand-shaped corpus, 50% updates) and indexing memory (D17). The design states
     whether they gate Phase 3; if not, PLAN moves them, with a reason.
5. **T5. Disk full.** The design restates the gate (`217`) with no mechanism. Required:
   - Storage admission reserves a commit's bytes (manifest, live-docs, metadata), so a
     commit that frees space can always be written.
   - Merges reserve their output before starting and abort cleanly on ENOSPC.
   - Standalone admission rejects new transactions while space is short; apply never
     rejects (D10).
   - ENOSPC at write time recovers in place once space returns. ENOSPC or EIO at fsync
     time fences (D2) and recovers by reopening. Neither loses an acknowledged
     transaction or corrupts the index.
   - The test fills faultfs's capacity during flushes, merges, and live-docs, metadata,
     and manifest writes, frees space, and checks continued operation and the digest
     against the model. Free space comes from outside `IO` (D19).
6. **T6. The rules every gate carries.** One CPU-hour of fuzzing per new decoder: the
   manifest, live-docs, metadata, `pk_filter` (through `fusefilter`'s `deserialize`),
   versions, and the docids section's new form. A fixture per new format. TSan over
   flush and merge threads and snapshot publication.
7. **T7. Parity across segmentation.** The 300 parity queries pass unchanged on an index
   built with many flushes and merges and no deletes, which exercises global statistics
   and cross-segment ties. With deletes, the drift (`126-127`) is measured against Xapian
   vectors recorded from the same update stream; after a merge drops every deleted
   document, parity is exact again.
8. **T8. Query classes on Phase 3's indexes** (report): the five commands on the
   multi-segment index the indexing row produces, and on one with deletes, against
   Phase 2's single-segment rows. A regression outside the noise band carries a cause;
   the count classes move most (I3).
9. **T9. Phase 2 conditions.** The gate report lists each: C1 (the docids section and its
   docid → ordinal lookup, D15, before merge code); C2 (D14, before hits merge across
   segments); C3 (with deletes); C11 (key terms in its target, D16, before key lookups
   depend on it). C6, C7, and C10 have landed (`e073258`, `03e2544`, `7f48e42`).

## Minor

1. The journal's test model is `cluster/test/journal_test.cc` (`200`).
2. "Deletes of committed documents" (`84`) means flushed documents; a committed document
   is one in a durable manifest.
3. `fusefilter` exists (`b4f4ce1`); the design cites it rather than calling it new
   (`94-95`).
4. The writer's diagram writes live-docs at flush (`76`), the commit text at commit
   (`54`). Either works under D1; the design picks one.

## Unaffected

These parts stand as written:

- One index per shard, one writer on one owner core.
- Single-file immutable segments; live-docs generations as side files; per-file sizes
  and footer CRCs in the manifest, which bind each file to it.
- The live version map in front of newest-first key lookups, with a filter per segment.
- Snapshots kept by a query across suspensions; the 1 s refresh default.
- Merges that preserve docids, run in the background, and are rate-limited.
- Metadata committed atomically with documents through the manifest.
- The deterministic core interface, and a digest independent of segmentation, ordinals,
  and merge history.
- faultfs and the memory budget starting inside strata, with nothing in `cluster` or
  Detent changed until the split is coordinated.
