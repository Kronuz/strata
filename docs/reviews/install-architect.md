# strata review: `install()`'s reopen of the installed segments (install-own-files CR1, CR3)

- Date: 2026-10-26. Architect ruling on two design questions escalated by the code review; a fresh context.
- Reviewed: main `540df28` (the fix merged at `53c088d`, code `17e8b3c`): `include/strata/core/index.hh:1136-1215`
  (`install`), `:803-830` (`heal`), `:1700-1740` (`guarded`, `written_segment`, `streamed_segment`), `:3111`
  (`write_new`), `:3405-3510` (`read_checked`, `load_files`, `load_segment_of`); completion at the pin `7734544`
  (`include/completion/posix.h:340-365`); `test/install_own_test.cc`.
- Checked against: D5 (`docs/design/phase-3.md:203-215`), `docs/design/media-faults.md` (fault model, "What strata
  does", "Next"), the install paragraph of `docs/design/durability-modes.md` on `origin/design/durability-modes`
  ("Installing", "verifies each whole as it arrives"), `docs/reviews/install-own-files-code.md` (CR1, CR3, CR4, CR5),
  `docs/status/install-own-files.md`, the memory note on the many-indexes workload.
- Checked by: ASan+UBSan build (Homebrew LLVM, Debug, `-O1`) of main `540df28` at
  `.scratch/review/install-architect/build-asan`, sources reused from `build/_deps` with every pin checked equal;
  `install_own_test` mapped (`STRATA_FAULTFS_MAP=1`) rc 0; probe `reopen_emfile.cc` (output `reopen_emfile.out`), no
  sanitizer report. The code reviewer's `dmg.cc` results (`strata-wt/review-iof/.scratch/out/dmg.log`) are reused for
  write damage, not rerun.
  - Not run: a `whole=true` reopen (no timing; the ruling does not rest on its cost), a real device fault (no
    dm-flakey or equivalent on this host), TSan, Linux and FreeBSD, the full ctest, CI.

## Verdict

**APPROVE WITH CONDITIONS: (1) install need not verify what it wrote: D5's "verifies each whole" is the checkpoint as
read from the source, and the copy follows the rule every written file follows; (2) a non-EIO error from the reopen
must not fence the writer: it propagates as itself and leaves the target as it was, unfenced, as the pre-copy load
does.**

What holds, and reproduces:

- The source is read and verified whole before anything changes (`index.hh:1154`, `load_files(source, theirs, true,
  ...)`, which runs `verify_all` per segment at `:3493`).
- A failure anywhere before the swap leaves the target's state untouched: `got` is local until `segs_ = std::move(...)`
  (`:1191`); the code reviewer's CR4 and CR5 showed it, and this probe's reopen of a target fenced mid-install finds
  the old content (`reopened target: kold=present`).
- `install_own_mapped` passes on main.

What blocks: nothing for main as landed. IN3 is a fix owed before the next gate (C2).

## Findings

### IN1. D5's text orders the verify after the copy; the code and the replication design put it on the source

`phase-3.md:205-206` reads "copies the checkpoint's files under fresh local names ..., verifies each whole, writes their
manifest". The code verifies the source before copying (`index.hh:1154`), and the durability-modes design describes
the follower's install as verifying "each whole as it arrives", that is, the received bytes before they are written.
Ruling: D5 means the checkpoint's bytes as read from the source. The sentence is fixed (C1).

### IN2. A whole-segment verify of the written copy would check the page cache, not the medium

completion's POSIX backend writes through the page cache and maps `MAP_SHARED` (`posix.h:353` at `7734544`), and the
pinned completion has no `O_DIRECT`, `F_NOCACHE`, `MS_INVALIDATE` or `POSIX_FADV_DONTNEED` anywhere (`git grep`, no
hits outside a platform note about `O_DIRECTORY`). `write_new` writes and syncs (`index.hh:3111`), so when the reopen
runs, the copy's pages are resident, and `verify_all` over the mapping reads back the bytes `write_new` put into the
cache. That catches corruption between strata's buffer and the cache, which `media-faults.md` puts out of the model
("Memory or CPU corruption"). It does not catch what the model does include (a misdirected, lost or corrupted device
write); that shows only when a page is read from the medium again: after eviction, at `heal()`'s re-read, at open, or
at a scrub. faultfs stores damage at write time, so in the reviewer's probe a `whole=true` reopen would look effective;
on a real filesystem it mostly would not. Requiring it would buy a test-only guarantee for one CRC pass per install.

This is the rule the other write paths follow: a flush takes its footer CRC from what it wrote (`streamed_segment`,
`index.hh:1725-1740`; `written_segment`, `:1716-1722`) and is checked per region at first read, as the reviewer noted.
The reviewer's table holds under the ruling: no run gave a wrong answer; a mapped target finds the damage at first
read and `heal()` names it.

One limit, shared with unmapped flush outputs and not new: an unmapped target serves the verified source bytes from
memory, and `heal()` verifies the in-memory segment first (`index.hh:803-818`), so a copy damaged on the medium stays
latent until the next open (the reviewer's "branch, unmapped" row: heal 0, after reopen 40 throw). The scrubber
(`media-faults.md`, "Next") is what closes that window; C5.

### IN3. A host error on the reopen fences the writer, and the fence protects nothing (CR3, now by probe)

`reopen_emfile.cc` wraps faultfs in an IO that throws `EMFILE` from `open_existing` of a segment file created during
the install, mapped run:

```
install, EMFILE once on the reopen:  ok (EMFILE thrown 1)
  media_retries counted for that host error: 1
install, EMFILE twice on the reopen: index_fenced (EMFILE thrown 2)
same target, apply afterwards:     index_fenced
same target, install retry:        index_fenced
reopened target: kold=present k5=present
reopened target, install retry:    ok
```

The second target (reopened from the first, so it already held the checkpoint, plus `kold` written before the
failing install) keeps exactly that content after the fence: `kold` present. `read_checked` (`index.hh:3413-3428`) retries once and rethrows a non-EIO `system_error`; the reopen sits inside
`guarded` (`:1703-1710`), which fences on every `system_error` but a space error. `mmap` failure lands the same way:
`PosixMapping` throws on `MAP_FAILED` (`posix.h:353-354`), so ENOMEM from `map()` fences too.

The fence exists for writes and syncs whose durable outcome is unknown (a failed `fsync` may have lost data that a
retry reports as written). At the reopen there is no such doubt: every copy is written and synced, the head manifest
is still the old one, and nothing in memory has changed (the probe's reopen shows the old content; the written files
are tracked orphans, CR4). EMFILE and ENOMEM are the host's, as `read_checked`'s own comment says (`:3424-3425`). With
millions of indexes per host, descriptor exhaustion hits many indexes at once; fencing each turns a retryable host
condition into a reopen per index, which itself needs descriptors.

Fix: the fenced region covers writes, syncs and the commit, nothing else. Either split the lambda (write and sync
every copy inside `guarded`; reopen the mapped ones outside it; swap and `commit_files()` inside `guarded` again), or
classify at the reopen as `space_error` does. EIO on both reads keeps becoming `corrupt_index` (the copy is damaged;
the target is not), also unfenced, as CR4 observed for header damage.

### IN4. `read_checked` counts host errors as media retries (pre-existing)

The first catch takes every `system_error` and bumps `media_retries_` before retrying (`index.hh:3416-3420`); the
probe's single EMFILE counts 1. `media-faults.md` ties transient retries to device health ("a device past a threshold
drains"), so a descriptor shortage would push a healthy device toward draining. It affects open and install alike.
Retrying a host error once is harmless; counting it as the medium's is not.

### IN5. The write-damage behaviour the ruling relies on has no test on main

The reviewer's silent-write-damage knob lives only in a shadow faultfs (`strata-wt/review-iof/.scratch/probes/shadow/`),
and no test on main pins "a copy damaged in the write is never a wrong answer". IN2 rests on that behaviour, so it
gets a test (C4).

## Blocking items

None for main as landed. C2 is owed before the next gate.

## Conditions

C1. D5's sentence (`phase-3.md:205-206`) says what is verified and when: "reads the checkpoint and verifies each file
whole before anything changes; writes the copies under fresh local names from the target's counters; the copies are
checked as every written file is (footer, identity and live docs when a mapped copy is reopened, regions at first read,
whole by `heal()`'s re-read, open and the scrubber)". The ledger's "Extra bytes" sentence
(`docs/status/install-own-files.md`) records this ruling in place of the open question.

C2. The reopen leaves the fenced region (IN3), and `install()`'s doc comment (`index.hh:1136-1143`) says a failure
reopening a copy also leaves the index as it was, unfenced, with the written files queued for removal. Test, from
`reopen_emfile.cc`'s wrapper: EMFILE on both reads of an installed copy makes `install` throw `system_error` with
EMFILE; an `apply` afterwards is accepted (not fenced); the content is the old one; an in-process retry succeeds; the
same with the error from `map()`. Mutant: the reopen back inside `guarded` fails the test.

C3. `read_checked` counts only media faults (EIO, `corrupt_index`, `corrupt_segment`) in `media_retries_` (IN4). Test:
one EMFILE on the reopen leaves `media_retries()` unchanged and the install succeeds.

C4. faultfs gets the silent-write-damage knob (the reviewer's `damage_writes`), and a test pins IN2's behaviour for
install: mapped, the install succeeds, a read over the damaged region throws `corrupt_segment` (never a wrong answer),
and `heal()` names the segment; unmapped, reads succeed from memory and the next open reports the damage.

C5. For the scrubber's design (`media-faults.md`, "Next"): it verifies files as read from the medium, not the
in-memory segment, so unmapped installed and flushed segments are covered. A note for that design, not a fix here.

## Probes and files kept (`.scratch/review/install-architect/`)

`reopen_emfile.cc` (the EMFILE wrapper over faultfs; output `reopen_emfile.out`), `build.sh` (configures
`build-asan` from `build/_deps`, builds `install_own_test`, then the probe from its compile and link lines;
`compile_cmd.txt`, `link_cmd.txt`, `configure.log`, `ninja.log`), `build-asan/`.

Not run: a `whole=true` reopen or any timing, a real device fault, TSan, Linux and FreeBSD, the full ctest, CI.
