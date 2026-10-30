> Status: CURRENT (2026-10-30) triage of every remote branch in strata, prospect, http and termdict that is not simply main; the owner decides every deletion.

# Branch triage, 2026-10-30

Workstream: branch cleanup (maintenance engineer). Requested by the owner on 2026-10-30. Nothing was deleted and nothing was force-pushed. Every branch proposed for deletion carries an `archive/<branch>` lightweight tag on its tip, pushed to its repository, so a deletion loses no commit.

Classes: 1 landed already (its changes are on main, or it is merged); 2 docs only; 3 code not landed (prototype or phase branch; the code is never merged here, only the notes); 4 live. A branch a worktree is on is kept, whatever its class: remove the worktree first, then delete.

Documents landed: strata, the commit that adds this file (106 files under `docs/design/`, `docs/reviews/`, `docs/status/`, `docs/perf/` and `docs/research/`, plus this file and the `docs/book/OUTLINE.md` source map; each non-review document starts with a status line); prospect:2f65633; termdict:dc42a4f. Modified documents on the branches (`docs/design/limits.md`, `reordering.md`, `small-indexes.md`, `docs/perf/README.md`, `docs/reviews/calibration.md`, `docs/status/write-buffer.md`) were not landed: main has newer or unrelated versions. Where several branches carried different copies of one file (`docs/perf/inverter.md`, `docs/status/inverter.md`), prospect main already holds the inverter notes, so none were landed in strata. `perf/lanebuf` and `perf/lanebuf-rebased` carry the bake-off; `design/write-modes` supplies `docs/design/write-buffer.md` (revision 4).

## Counts

| Repo | Branches | Class 1 | Class 2 | Class 3 | Class 4 | Propose delete (tagged) | Keep |
|---|---|---|---|---|---|---|---|
| strata | 88 | 58 | 9 | 21 | 0 | 53 | 35 |
| prospect | 20 | 18 | 0 | 2 | 0 | 11 | 9 |
| http | 3 | 1 | 0 | 1 | 1 | 0 | 3 |
| termdict | 4 | 4 | 0 | 0 | 0 | 2 | 2 |

Live keeps counted under their class: strata `design/cold-tier` (merged, under re-check) and `design/hybrid` (parked until the cold tier), http `trace/hop` (never merge).

## strata

| Branch | Class | Docs landed at | Archive tag | Action | Reason |
|---|---|---|---|---|---|
| `design/cold-tier` | 1 | - | - | keep | live: under re-check; already merged to main |
| `design/durability-modes` | 2 | `design/durability-modes.md` (+3 more) | - | keep | worktree strata-wt; docs landed; design approved with conditions, PARKED behind engine speed; also carries bench/durability results and test code (324 non-doc files), not landed |
| `design/fields-and-keys` | 1 | - | `archive/design/fields-and-keys` | delete | merged into main (no commits beyond it) |
| `design/format-14-kv` | 1 | - | `archive/design/format-14-kv` | delete | merged into main (no commits beyond it) |
| `design/hybrid` | 2 | `design/hybrid-search.md` (+6 more) | - | keep | live: PARKED until the cold tier lands (owner Q1: not yet) |
| `design/landscape` | 2 | `design/product-landscape.md` (+1 more) | - | keep | worktree strata-wt; docs landed; CURRENT product landscape |
| `design/peer-transport` | 2 | `design/peer-transport.md` (+3 more) | - | keep | worktree strata-wt; docs landed; PARKED (distributed parks) |
| `design/query-gap` | 2 | `design/query-gap.md` (+2 more) | - | keep | worktree strata-wt; docs landed; HISTORY; also carries test fixtures and bench/quick edits, not landed |
| `design/queryparser` | 2 | `design/queryparser.md` (+2 more) | - | keep | worktree strata-wt; docs landed; CURRENT; parser lives in the queryparser repository |
| `design/shared-reader` | 2 | `design/shared-reader.md` (+2 more) | - | keep | worktree strata-wt; docs landed; CURRENT; implemented by the landed views step |
| `design/simulator` | 2 | `design/simulator.md` (+2 more) | - | keep | worktree strata-wt; docs landed; PARKED (distributed parks); sim/ code stays on the branch |
| `design/write-modes` | 2 | `design/write-buffer.md` (+1 more) | - | keep | worktree strata-wt; docs landed; CURRENT revision 4; the write buffer is built on it |
| `docs/book-outline` | 1 | - | `archive/docs/book-outline` | delete | merged into main (no commits beyond it) |
| `explore/bigdisk` | 3 | `perf/explore-bigdisk.md` | `archive/explore/bigdisk` | delete | note landed SUPERSEDED by the background reclaim; prototype code (bench) not merged |
| `explore/bigmem` | 3 | `perf/bigmem.md` | `archive/explore/bigmem` | delete | note landed SUPERSEDED by the streaming merges; prototype code not merged |
| `explore/bigval` | 3 | `perf/explore-bigval.md` | `archive/explore/bigval` | delete | note landed SUPERSEDED by the background reclaim; prototype code not merged |
| `explore/buffer-bytes` | 1 | `perf/buffer-bytes.md` | `archive/explore/buffer-bytes` | delete | note landed SUPERSEDED: the 8-byte slots landed as strata:8fcd4e3 (log) |
| `explore/c1-field` | 1 | - | `archive/explore/c1-field` | delete | merged into main (no commits beyond it) |
| `explore/deadratio` | 1 | - | - | keep | worktree strata-wt; note identical on main; bench change not landed; worktree strata-wt/deadratio |
| `explore/disk-1k` | 1 | `perf/disk-1k.md` | - | keep | worktree strata-wt; landed as strata:0156b57 (log); note landed SUPERSEDED; worktree |
| `explore/f1-ci` | 1 | - | `archive/explore/f1-ci` | delete | git cherry shows every commit on main; note identical on main |
| `explore/hazard-flake` | 1 | - | `archive/explore/hazard-flake` | delete | merged into main (no commits beyond it) |
| `explore/linux-1026` | 1 | - | `archive/explore/linux-1026` | delete | merged into main (no commits beyond it) |
| `explore/open-cost` | 3 | `perf/open-cost.md` | - | keep | worktree strata-wt; PARKED: folds into the turn log; note landed; worktree |
| `explore/peak-memory` | 1 | - | `archive/explore/peak-memory` | delete | landed as strata:5685428 (log); main already holds the later note docs/perf/peak-memory.md |
| `explore/remeasure` | 1 | - | `archive/explore/remeasure` | delete | merged into main (no commits beyond it) |
| `explore/retention` | 1 | - | `archive/explore/retention` | delete | note identical on main; retention landed (retention_test.cc, docs/reviews/retention.md) |
| `explore/rss` | 1 | - | `archive/explore/rss` | delete | merged into main (no commits beyond it) |
| `explore/scan` | 1 | `perf/scan-lazy-merge.md` | `archive/explore/scan` | delete | note landed SUPERSEDED: landed as basic_index::scan, strata:89f9fd7 (log) |
| `explore/write-p99` | 1 | `perf/write-p99.md` | `archive/explore/write-p99` | delete | note landed SUPERSEDED: landed as strata:5737867 (log) |
| `fix/apply-atomic` | 1 | - | `archive/fix/apply-atomic` | delete | merged into main (no commits beyond it) |
| `fix/flush-memory-bound` | 1 | - | `archive/fix/flush-memory-bound` | delete | merged into main (no commits beyond it) |
| `fix/install-errors` | 1 | - | `archive/fix/install-errors` | delete | merged into main (no commits beyond it) |
| `fix/install-own-files` | 1 | - | `archive/fix/install-own-files` | delete | merged into main (no commits beyond it) |
| `fix/repin-values` | 1 | - | `archive/fix/repin-values` | delete | merged into main (no commits beyond it) |
| `fix/stored-short-section` | 1 | - | `archive/fix/stored-short-section` | delete | merged into main (no commits beyond it) |
| `fix/write-buffer-followups` | 1 | - | `archive/fix/write-buffer-followups` | delete | merged into main (no commits beyond it) |
| `perf/commit-barriers` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `perf/core-vs-rocksdb` | 1 | - | `archive/perf/core-vs-rocksdb` | delete | merged into main (no commits beyond it) |
| `perf/encode-once` | 3 | `perf/encode-once.md` (+1 more) | - | keep | worktree; REJECTED (last commit removes encode-once); worktree strata-wt/encode-once; delete after the worktree is removed |
| `perf/f13-gate` | 1 | - | `archive/perf/f13-gate` | delete | merged into main (no commits beyond it) |
| `perf/f13-preflight` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `perf/flushmem` | 1 | - | `archive/perf/flushmem` | delete | merged into main (no commits beyond it) |
| `perf/idle-index` | 1 | - | `archive/perf/idle-index` | delete | merged into main (no commits beyond it) |
| `perf/index-size` | 3 | `perf/index-size.md` (+1 more) | - | keep | worktree; PARKED; reclaim landed separately; worktree |
| `perf/indexing-gap` | 3 | `perf/indexing-gap.md` (+1 more) | - | keep | worktree; HISTORY notes; code not landed; worktree |
| `perf/inverter` | 3 | - | - | keep | worktree; inverter landed in prospect (its notes are already in prospect main); superseded, delete after the worktree is removed |
| `perf/inverter-on-layers` | 1 | - | `archive/perf/inverter-on-layers` | delete | merged into main (no commits beyond it) |
| `perf/lanebuf` | 3 | - | - | keep | worktree; owner accepted C (2026-10-26) and the write buffer is built; worktree; delete after it is removed (same line as lanebuf-rebased) |
| `perf/lanebuf-rebased` | 3 | `perf/lanebuf-scan-tail.md` (+19 more) | `archive/perf/lanebuf-rebased` | delete | bake-off and scan-tail notes landed; prototype code (bench/kv/proto) never merged; the owner accepted C and the write buffer landed on main; perf/lanebuf is the same line |
| `perf/query-counters` | 3 | `reviews/query-counters-code.md` (+1 more) | - | keep | worktree; HISTORY; code not landed; worktree |
| `perf/query-exp` | 3 | `perf/ab-run-level.md` (+23 more) | - | keep | worktree; HISTORY; prototype code and fixtures not landed; worktree |
| `perf/reorder-speed` | 3 | `perf/reorder-speed.md` (+1 more) | - | keep | worktree; HISTORY; code not landed; worktree |
| `perf/scale-threads` | 3 | `perf/scale-threads.md` (+1 more) | - | keep | worktree; HISTORY; code not landed; worktree |
| `perf/small-indexes` | 3 | `perf/small-indexes.md` (+2 more) | - | keep | worktree; HISTORY; code not landed; worktree |
| `perf/t11-find-docid` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `phase/cold-tier-step1` | 1 | - | `archive/phase/cold-tier-step1` | delete | merged into main (no commits beyond it) |
| `phase3/bisection-pool` | 3 | `status/bisection-pool.md` | - | keep | worktree; in review, code not landed; worktree |
| `phase4/core-split` | 1 | - | `archive/phase4/core-split` | delete | merged into main (no commits beyond it) |
| `phase4/f13-query` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `phase4/f13m/540ae6b` | 1 | - | `archive/phase4/f13m/540ae6b` | delete | merged into main (no commits beyond it) |
| `phase4/f13m/7298fd3` | 1 | - | `archive/phase4/f13m/7298fd3` | delete | merged into main (no commits beyond it) |
| `phase4/f13m/77751a0` | 1 | - | `archive/phase4/f13m/77751a0` | delete | merged into main (no commits beyond it) |
| `phase4/f13m/825c516` | 1 | - | `archive/phase4/f13m/825c516` | delete | merged into main (no commits beyond it) |
| `phase4/f13m/831459e` | 1 | - | `archive/phase4/f13m/831459e` | delete | merged into main (no commits beyond it) |
| `phase4/f13m/85348eb` | 1 | - | `archive/phase4/f13m/85348eb` | delete | merged into main (no commits beyond it) |
| `phase4/f13m/e3222eb` | 1 | - | `archive/phase4/f13m/e3222eb` | delete | merged into main (no commits beyond it) |
| `phase4/f13m/ea98061` | 1 | - | `archive/phase4/f13m/ea98061` | delete | merged into main (no commits beyond it) |
| `phase4/format13` | 1 | - | `archive/phase4/format13` | delete | merged into main (no commits beyond it) |
| `phase4/format13-on-main` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `phase4/inverter-followups` | 1 | - | `archive/phase4/inverter-followups` | delete | merged into main (no commits beyond it) |
| `phase4/layers-refactor` | 1 | - | `archive/phase4/layers-refactor` | delete | merged into main (no commits beyond it) |
| `phase4/reclaim-bg` | 1 | - | `archive/phase4/reclaim-bg` | delete | merged into main (no commits beyond it) |
| `phase4/repo-split` | 1 | - | `archive/phase4/repo-split` | delete | merged into main (no commits beyond it) |
| `phase4/shared-reader-views` | 1 | - | `archive/phase4/shared-reader-views` | delete | merged into main (no commits beyond it) |
| `phase4/term-lists-rank` | 3 | - | - | keep | worktree; P2 term lists abandoned (R-P2, prospect:fc1a88d); worktree; delete after it is removed, the owner decides |
| `phase4/term-lists-rank-on-values` | 3 | - | `archive/phase4/term-lists-rank-on-values` | delete | superseded: P2 term lists abandoned (R-P2, prospect:fc1a88d); no docs |
| `phase4/values-rank` | 1 | - | `archive/phase4/values-rank` | delete | merged into main (no commits beyond it) |
| `phase4/write-buffer` | 1 | - | `archive/phase4/write-buffer` | delete | merged into main (no commits beyond it) |
| `phase4/write-buffer-reads` | 1 | - | `archive/phase4/write-buffer-reads` | delete | merged into main (no commits beyond it) |
| `phase5/automata` | 3 | `design/automata.md` (+2 more) | - | keep | worktree; implementing, code not landed; worktree |
| `phase7c/l5-defer-io` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `phase8/field-table` | 1 | - | `archive/phase8/field-table` | delete | merged into main (no commits beyond it) |
| `phase8/fields-f2` | 1 | - | `archive/phase8/fields-f2` | delete | git cherry shows every commit on main (landed as land/fields-f2) |
| `phase8/key-field` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `research/indexing` | 3 | `research/indexing.md` (+1 more) | - | keep | worktree; HISTORY note landed; prototype code in research/ stays; worktree |
| `research/no-stalls` | 3 | `research/no-stalls.md` (+1 more) | - | keep | worktree; HISTORY note landed; prototype code in research/ stays; worktree |
| `research/rocksdb-spec` | 1 | - | `archive/research/rocksdb-spec` | delete | merged into main (no commits beyond it) |

## prospect

| Branch | Class | Docs landed at | Archive tag | Action | Reason |
|---|---|---|---|---|---|
| `explore/merge-field-directory` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `explore/phrases` | 1 | - | `archive/explore/phrases` | delete | merged into main (no commits beyond it) |
| `explore/req-opt` | 1 | - | `archive/explore/req-opt` | delete | landed as prospect:59f36fe (log); explorer note landed SUPERSEDED |
| `explore/topk-count` | 3 | - | - | keep | PARKED until a quiet-host A/B (log); note landed PARKED; the 7-line change stays on the branch |
| `fix/collector-reserve` | 1 | - | `archive/fix/collector-reserve` | delete | merged into main (no commits beyond it) |
| `fix/strata-main-pin` | 1 | - | `archive/fix/strata-main-pin` | delete | merged into main (no commits beyond it) |
| `perf/commit-barriers` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `perf/phrases` | 1 | - | `archive/perf/phrases` | delete | merged into main (no commits beyond it) |
| `perf/t11-pin` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `perf/word-sum-memory` | 1 | - | `archive/perf/word-sum-memory` | delete | merged into main (no commits beyond it) |
| `phase4/host-collectors` | 1 | - | `archive/phase4/host-collectors` | delete | merged into main (no commits beyond it) |
| `phase4/repo-split` | 1 | - | `archive/phase4/repo-split` | delete | merged into main (no commits beyond it) |
| `phase4/term-lists` | 3 | - | - | keep | worktree prospect-wt/term-lists; P2 abandoned (R-P2); notes landed REJECTED; delete after the worktree is removed, the owner decides |
| `phase4/values-bytes` | 1 | - | `archive/phase4/values-bytes` | delete | merged into main (no commits beyond it) |
| `phase4/xapian-ops` | 1 | - | `archive/phase4/xapian-ops` | delete | merged into main (no commits beyond it) |
| `phase8/fields` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `phase8/key-field` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `phase8/phrase-alternatives` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `phase8/phrase-positions` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `rename-engine-prospect` | 1 | - | `archive/rename-engine-prospect` | delete | merged into main (no commits beyond it) |

## http

| Branch | Class | Docs landed at | Archive tag | Action | Reason |
|---|---|---|---|---|---|
| `phase7c/s1-lag-probe` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `trace/hop` | 4 | - | - | keep | live: never merge |
| `track/http-integration` | 3 | - | - | keep | 10 commits not on main (code); Detent adoption scratch worktrees are detached at its tip; not mechanical to land |

## termdict

| Branch | Class | Docs landed at | Archive tag | Action | Reason |
|---|---|---|---|---|---|
| `explore/forward-seek-a` | 1 | - | `archive/explore/forward-seek-a` | delete | the exact version landed as termdict:74d518a; explorer variant note landed SUPERSEDED |
| `explore/forward-seek-b` | 1 | - | `archive/explore/forward-seek-b` | delete | the exact version landed as termdict:74d518a; explorer variant note landed SUPERSEDED |
| `fix/validate-at-open` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |
| `perf/flushmem` | 1 | - | - | keep | merged into main; worktree holds it, delete after `git worktree remove` |

## Deletion commands for the owner (not run)

Each branch below has its `archive/...` tag pushed. Remote deletions first, then the local branches that exist. Branches held by a worktree are not listed; remove the worktree first, then add the branch.

```sh
# strata
cd ~/code/Kronuz/strata
git push origin --delete \
  design/fields-and-keys \
  design/format-14-kv \
  docs/book-outline \
  explore/bigdisk \
  explore/bigmem \
  explore/bigval \
  explore/buffer-bytes \
  explore/c1-field \
  explore/f1-ci \
  explore/hazard-flake \
  explore/linux-1026 \
  explore/peak-memory \
  explore/remeasure \
  explore/retention \
  explore/rss \
  explore/scan \
  explore/write-p99 \
  fix/apply-atomic \
  fix/flush-memory-bound \
  fix/install-errors \
  fix/install-own-files \
  fix/repin-values \
  fix/stored-short-section \
  fix/write-buffer-followups \
  perf/core-vs-rocksdb \
  perf/f13-gate \
  perf/flushmem \
  perf/idle-index \
  perf/inverter-on-layers \
  perf/lanebuf-rebased \
  phase/cold-tier-step1 \
  phase4/core-split \
  phase4/f13m/540ae6b \
  phase4/f13m/7298fd3 \
  phase4/f13m/77751a0 \
  phase4/f13m/825c516 \
  phase4/f13m/831459e \
  phase4/f13m/85348eb \
  phase4/f13m/e3222eb \
  phase4/f13m/ea98061 \
  phase4/format13 \
  phase4/inverter-followups \
  phase4/layers-refactor \
  phase4/reclaim-bg \
  phase4/repo-split \
  phase4/shared-reader-views \
  phase4/term-lists-rank-on-values \
  phase4/values-rank \
  phase4/write-buffer \
  phase4/write-buffer-reads \
  phase8/field-table \
  phase8/fields-f2 \
  research/rocksdb-spec
for b in design/fields-and-keys design/format-14-kv docs/book-outline explore/bigdisk explore/bigmem explore/bigval explore/buffer-bytes explore/c1-field explore/f1-ci explore/hazard-flake explore/linux-1026 explore/peak-memory explore/remeasure explore/retention explore/rss explore/scan explore/write-p99 fix/apply-atomic fix/flush-memory-bound fix/install-errors fix/install-own-files fix/repin-values fix/stored-short-section fix/write-buffer-followups perf/core-vs-rocksdb perf/f13-gate perf/flushmem perf/idle-index perf/inverter-on-layers perf/lanebuf-rebased phase/cold-tier-step1 phase4/core-split phase4/f13m/540ae6b phase4/f13m/7298fd3 phase4/f13m/77751a0 phase4/f13m/825c516 phase4/f13m/831459e phase4/f13m/85348eb phase4/f13m/e3222eb phase4/f13m/ea98061 phase4/format13 phase4/inverter-followups phase4/layers-refactor phase4/reclaim-bg phase4/repo-split phase4/shared-reader-views phase4/term-lists-rank-on-values phase4/values-rank phase4/write-buffer phase4/write-buffer-reads phase8/field-table phase8/fields-f2 research/rocksdb-spec; do git show-ref --verify --quiet refs/heads/$b && git branch -D $b; done
```

```sh
# prospect
cd ~/code/Kronuz/prospect
git push origin --delete \
  explore/phrases \
  explore/req-opt \
  fix/collector-reserve \
  fix/strata-main-pin \
  perf/phrases \
  perf/word-sum-memory \
  phase4/host-collectors \
  phase4/repo-split \
  phase4/values-bytes \
  phase4/xapian-ops \
  rename-engine-prospect
for b in explore/phrases explore/req-opt fix/collector-reserve fix/strata-main-pin perf/phrases perf/word-sum-memory phase4/host-collectors phase4/repo-split phase4/values-bytes phase4/xapian-ops rename-engine-prospect; do git show-ref --verify --quiet refs/heads/$b && git branch -D $b; done
```

```sh
# termdict
cd ~/code/Kronuz/termdict
git push origin --delete \
  explore/forward-seek-a \
  explore/forward-seek-b
for b in explore/forward-seek-a explore/forward-seek-b; do git show-ref --verify --quiet refs/heads/$b && git branch -D $b; done
```
