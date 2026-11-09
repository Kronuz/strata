> Status: PARKED (2026-11-03) the CPU is file churn per commit; the fix is the turn log (durability), not a tweak.

# Synchronous commit CPU: where it goes

Question: why does a synchronous commit cost 2.5-10x RocksDB's CPU per op (`core-vs-rocksdb-f14.md`, sync1 rows)?
Branch `explore/sync-cpu`. Development Mac (Intel i9-9980HK, APFS), `kv_strata` fillrandom, 100-byte
values, durability sync, batch 1 (one commit per put), strict hold.

## Findings

- Confirmed on main `356acbc`: 1.8-2.1 ms CPU per commit (`cpu_us_per_op`), 128 commits in 6 s.
- The CPU is system time in metadata syscalls, not barriers. With `F_FULLFSYNC` turned into `fsync`
  (`BARRIERS_WEAK=1`) CPU per commit is unchanged (1.6-1.9 ms) while throughput rises 10x. Thread CPU inside
  the barriers is 39 us per `F_FULLFSYNC` and 5-65 us per `fsync` (about 10% of the total).
- Per commit, from `bench/kv/barriers.c` (now also times open, unlink, rename, pwrite, close, fchmod): 2.1
  files created (a segment and the manifest temporary), 2 unlinks, 1 rename, 5 `fsync`, 2 `F_FULLFSYNC`.
  Interposed thread CPU: unlinkat 241 us, openat 165, renameat 126, pwrite 72, fchmod 30, fsync about 120,
  `F_FULLFSYNC` 80.
- Floor of the same file work in a plain C loop (`fsmicro.c`, 2000 rounds): append + fsync to one file 33 us;
  in-place overwrite + fsync 16 us; create + write + fsync 120 us; unlink 93 us; manifest cycle (temporary,
  write, fsync, rename, unlink the old one) 268 us. Strata's two files cost about 0.5 ms at that floor against
  1.5-1.9 ms measured; about 1 ms per commit is not attributed (the interposer sees open, unlink, rename, pwrite,
  close, fchmod, fsync, `F_FULLFSYNC`, flock, mmap, fstat; not directory reads or other threads' kernel work).
  RocksDB's one `F_FULLFSYNC` on an existing WAL costs 219 us per op.

## Experiments (switches in `index.hh`, `STRATA_X_*`)

Weak barriers (fsync for `F_FULLFSYNC`) so the A/B is CPU-bound and the shared disk's load does not decide it,
600 commits per run, 3 interleaved rounds, A/A first, strict hold. CPU us/op (spread max/min):

| Variant | CPU us/op | vs base | p50 commit |
|---|---:|---:|---:|
| base (6 runs, A/A) | 1904 (1759-2057, 17%) | | 44.8 ms |
| `STRATA_X_LAZY=16`: `retain()` every 16th commit (one fewer unlink and directory sync per commit) | 1765 (1746-1791) | 7% less, inside the A/A spread | 23.2 ms (1.93x faster wall) |
| `STRATA_X_DIRECT`: manifest written under its final name, no temporary or rename | 1716 (1634-1782) | 10% less, inside the A/A spread | 44.3 ms |
| both | 1498 (1470-1513, 3%) | 21% less, outside the spread | 23.2 ms |

Identity: fillrandom 150 records under sync, reopen, `readall`: get digest `a31a877d7f9e2a1f`, 150 hits for
base and all three variants. `DIRECT` is a CPU probe only: a torn manifest under its final name needs open to
fall back to the predecessor, which it does not do today.

## Verdict

The two cheap switches remove at most 21% of CPU. The cost is per file: every commit creates a segment file and a
manifest, unlinks old ones and renames, and each is 100-250 us of APFS work, against 33 us to append and fsync
a log that already exists. The single change that removes most is a turn log: a tiny commit appends to one open
file with one barrier and creates no file, leaving segment and manifest creation to flush and merge. That is
durability step 1 (`design/durability-modes`); this note is its CPU baseline (about 1.9 ms per commit to beat,
floor about 0.05 ms). Lazy retention is the one cheap piece worth keeping if the log slips: it halves wall time per
commit (a directory sync per commit gone) at 7% less CPU, but it holds predecessor manifests longer, a durability
and disk-space decision.

Not done: the unattributed millisecond (needs Instruments, absent here: only the Command Line Tools are installed;
`sample` shows wall time, which is mostly blocked in the barriers).
