# Reclamation: deletes, merges, and freeing files

Deletes are bits in live-docs files, so they cost nothing until the space comes back.
Space comes back in one of three ways, from cheapest to dearest:

1. **Drop a whole segment.** A segment whose documents are all deleted leaves the
   manifest at the next commit. Nothing is rewritten.
2. **Free a file.** Unlinking an obsolete segment returns its blocks. The filesystem
   does that work synchronously in the caller.
3. **Merge.** A reclaim merge rewrites a segment's live documents to drop the deleted
   ones. Merges are where nearly all of the bytes are spent.

This record covers what each one costs on every target platform, and what strata does
so that none of them stalls indexing or queries.

## Freeing a file, per platform

`.scratch/reclaim/probe.cc` and `interfere.cc` (in the strata checkout): a synced file,
unlinked whole or truncated from its end in steps and then unlinked, and `munmap` of a
fully touched mapping. Three runs each.

| 2 GiB file | unlink | Truncate in 64 MiB steps: total, longest step | `munmap` (touched) |
|---|---:|---:|---:|
| macOS 15.8, x86_64, APFS (i9-9980HK, internal SSD) | 118–171 ms | 144–146 ms, 24–26 ms | 38–47 ms |
| Linux 6.8, x86_64, ext4 (gate VM, virtio) | 256–269 ms | 250–277 ms, 10–13 ms | 39–42 ms |
| FreeBSD 14.4, x86_64, UFS with soft updates (VM, virtio) | 91–105 ms | 169–199 ms, 7.5–8.0 ms | 24–25 ms |
| macOS 27, arm64, APFS (M1 Pro) | 14–19 ms | 28–34 ms, 2.4–5.8 ms | 10–11 ms |

Random uncached 4 KiB reads of another file, while a 4 GiB file is freed. The table
shows p99 and p99.9 latency.

| | Idle | During unlink | During 16 MiB steps |
|---|---|---|---|
| Linux ext4 | 0.26–0.27, 0.34–0.35 ms | 9.7–27.6, 27.5–50.1 ms | 4.0–5.2, 11.9–30.8 ms |
| FreeBSD UFS | 1.34–1.80, 2.2–2.5 ms | 1.76–1.98, 2.4 ms | 1.04–2.11, 2.4–2.6 ms |
| macOS x86_64 APFS (disk 99% full) | 0.31–0.32, 0.36 ms | 0.56–0.93, 1.8–2.3 ms | 0.54–0.65, 1.7–2.1 ms |
| macOS arm64 APFS | not measured: the reads hit the cache | | |

What this shows:

- Freeing costs 7 to 130 ms per GiB, depending on the platform. That spread is too
  wide to tune per filesystem. The cost has to stay off the paths that serve queries
  and apply writes.
- On Linux ext4 the cost spills into other I/O: a 4 GiB unlink raises other reads' p99
  from 0.26 ms to 10–28 ms. Truncating in steps cuts that p99 to 4–5 ms, but p99.9
  stays at 12–31 ms. Steps bound each pause; they do not remove the interference.
- Truncating in steps costs up to 1.9× the total time of one unlink (on FreeBSD). In
  exchange, no single call is longer than one step.
- Truncating a file while a mapping of it is still open is unsafe: a read past the new
  end raises SIGBUS. Today, segments are read into memory, so truncation is safe. Once
  segments are mapped (ARCHITECTURE, "I/O"), a file is truncated only after its last
  mapping is released.

## What strata does

- **Whole-segment drop.** `commit` drops every segment with no live document before
  writing the manifest. Retention then frees its files.
- **No directory scan per commit.** The index keeps every file it owns and its size,
  plus each retained manifest's file set; an older manifest is read once. `retain()`
  diffs those sets in memory. A directory scan runs only at open, to find files left
  behind by a crash.
- **Stepped, bounded removal.** Obsolete data files go to a queue. Each file is
  truncated from its end `reclaim_step` bytes at a time (16 MiB), then unlinked.
  `commit` frees about `reclaim_slice` bytes (64 MiB), and `reclaim()` frees the rest.
  When storage admission is short of space, it drains the queue before it refuses a
  transaction, a merge, or an install. Manifests still leave first, followed by a
  directory sync, so a crash at any point leaves only orphans. The next open queues
  them. `index_test` cuts power after each of the 28 mutations of such a commit and its
  reclamation.
- **Merges at commit.** With `auto_merge` set, a commit now runs the policy after its
  flush. Before this change, only `refresh()` merged, so an index driven only by commits
  never merged: in the FIFO run below, it held 400 segments.

## Whole-segment drop, measured

`.scratch/fifo/fifo.cc` keeps a FIFO window of 20,000 live documents (TTL-style). Each
of 400 cycles adds 500 documents, deletes the 500 oldest, and commits. Run in faultfs;
the counts do not depend on the platform.

| | Write amplification | Segment bytes held, second half (mean, max) |
|---|---:|---:|
| No merges, no drop | 1.00 | 14.6 MB, 19.5 MB |
| No merges, drop | 1.00 | 1.9 MB, 1.9 MB (7.5× less) |
| Tiered merges, no drop | 2.00 | 1.5 MB, 1.8 MB |
| Tiered merges, drop | 2.08 | 1.4 MB, 1.6 MB |
| Merges over age-ordered windows, drop | 3.56 | 1.3 MB, 1.4 MB |

With merges running, the drop changes almost nothing. Tiered merging combines segments
of similar size regardless of age, so old and new documents end up in the same segment,
and a segment rarely dies whole. Merges pay for the reclamation instead: write
amplification is 2.0. The naive fix, ordering the merge windows by age, makes it worse
(3.56×) and breaks the balanced-merge bound in `merge_policy_test`, so it is removed.
The random-replacement steady state in `churn_test` is unchanged by all of this
(1.80× a fresh index at worst).

## Next (Track P)

- **P16: deletion affinity.** Merge only segments of adjacent age and similar size, so
  documents deleted in age order leave whole segments to drop. Measured on the FIFO run
  and on the churn steady state; it stays only if write amplification falls on FIFO
  without rising on random churn.
- **P17: rebase copy.** A merge copies each postings block with no deleted document
  verbatim, rewriting only its base, because the deltas inside a block do not change
  when every docid shifts by the same amount. Target: reclaim-merge throughput.
- **P18: holes for deleted stored records.** Punch holes over the stored bytes of
  deleted documents instead of rewriting the segment: `fallocate(PUNCH_HOLE)` on Linux,
  `fcntl(F_PUNCHHOLE)` on macOS, `fspacectl` on FreeBSD 14. Verified regions inside a
  hole are marked, so `check` does not report them. Target: disk under deletes of large
  documents.
- **P19: off-core reclamation.** Once background merges land (D18), stepped removal and
  `munmap` move to the background I/O thread, rate-limited. The row to report is
  foreground p99 while reclamation runs, on all four platforms.
