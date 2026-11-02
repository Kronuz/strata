> Status: CURRENT (2026-10-30) as built on `phase/cold-tier-item5`; the design is
> `docs/design/cold-tier.md` (D3, D10, I5, I8; CT-R21, CT-R22, CT-R26) on `design/cold-tier`.

# Warm indexes: demote(), promote() and map_owed

A warm index is an open index that holds no mapping: its segments are records with no body (see
`segment-records.md`). `demote()` makes an index warm, a reader or `promote()` maps the bodies
again, and a `defer_io` host, whose `apply` may not touch the device, is told when bodies are owed
by `verdict::map_owed`. Idle indexes cost a publication object, the records and the reader state
instead of one map and one descriptor per segment.

## How it works

```
 hot ── demote() ──▶ warm ── reader's seg() maps one record ──▶ hot again
                       │
   defer_io only:      └─ apply ─▶ map_owed ─▶ host: promote() ─▶ apply again, accepted
```

- **demote()** (a chore under the lease). Refuses, and changes nothing, when a record is buffered,
  a merge is pending, a segment is unsynced, or a `map_owed` turn is outstanding. Otherwise it
  swaps **every** record for a twin, bodied or not (`open_segment::twin()`): the same `ref`, the
  bits (shared, copy on write), `dead`, `deletes_dirty`, `damaged`, the facts (learned first from a
  body a reader mapped), the load context, the manifest's vouch, and the failed marker. It forces a
  publication, since an unchanged state publishes nothing, and the publication's registry scan
  frees the views over the originals once no guard holds them. Nothing is written: a twin stands
  for uncommitted deletes, and the next commit writes their live-docs file. A late map through a
  retired view lands in the original the view keeps, never in the index.
- **promote()** (a chore). With the flag clear it returns at once. Otherwise it maps every record
  without a body through D9's failure rule: a failed check leaves the marker and counts as done, a
  host error rethrows unchanged with the bodies so far kept, the flag set and nothing fenced. Then
  it clears the flag. It writes and publishes nothing; readers reach the bodies through the records
  their view already names.
- **The flag** (`promote_owed()`, an atomic) is set by `demote()` under `defer_io` when some twin
  can still map. While it is set, `apply` answers `map_owed` after the replay check and before
  `flush_owed`: nothing applied, no device call. The host runs `promote()` and applies the same
  transaction at the same position. A debug build asserts, at the check, that with the flag clear
  every record is bodied or marked.
- **The turn's bit.** `apply` sets an owner bit when it answers `map_owed`; `demote()` refuses
  while it is set. An `apply` that returns anything but one of the three owed answers (or throws)
  ends the turn and clears it. So between `promote()` and the retry no `demote()` can undo the
  promotion, and a turn sees at most one `map_owed`, one `flush_owed` and one `drain_owed`: at
  most three retries, in any order. `refresh()` does not clear the bit: it is `flush_owed`'s chore.
- **Counters.** `unleased_maps()` counts input maps made on a thread holding no lease (a reader's):
  a host that reads only after `promote()` sees it stay still. `map_losses()` counts bodies that
  lost the install race. `views_held()` matches records by segment id, so a twin and the record it
  replaced count once.

## Why this mechanism

Excluding `defer_io` from warm would leave Xapiand, whose idle indexes are the case this tier
exists for, with hot and cold only. A host chore alone would let a forgotten wake map a file inside
`apply` on a reactor; the verdict costs one atomic load per apply. The bit exists because the
adversary the architect's model finds, a `demote()` between `promote()` and the retry, makes the
answer repeat without bound (`.scratch/arch-r3/owed_model.py` on `design/cold-tier`, variant 3).

## Invariants

- I5, demotion and promotion are invisible to reads: position, digest and contents are equal
  before and after (`invisible_to_reads`).
- I8, under `defer_io` an `apply` makes no device call, warm or hot (`owed_answers`,
  `answer_ends`: `faultfs::filesystem::calls()` unchanged).
- After the last guard releases a warm index holds no mapping (`publication_and_free`).
- A turn sees at most one of each owed answer (`turn_bound`, `turn_bound_drain`).
- Demotion and promotion write nothing, and a power loss around them loses no acknowledged
  commit and leaves no record without a body after the reopen (`crash_points`).

Tests: `test/warm_test.cc`; each K5 rule and its mutants are in `docs/status/cold-tier-item5.md`.

## Code

`include/strata/core/index.hh`: `demote()`, `promote()`, `promote_owed()`, `open_segment::twin()`,
`apply` (the check and `turn_guard`), `views_held()`.
