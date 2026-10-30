# strata security review: durability and replication modes, Part A (design, preliminary)

- Date: 2026-10-18.
- Reviewed: `docs/design/durability-modes.md` at `8f8b043` on `design/durability-modes`
  (worktree `~/code/Kronuz/strata-wt/durability`; `HEAD` equals `origin/design/durability-modes`
  after a fetch), with its ledger `docs/status/durability-modes.md` and the branch's only code,
  the two probes under `bench/durability/` (`sizing.cc`, `count_syncs.cc`, `counting_io.hh`,
  `syncspy.c`) and their raw results (`results/2026-10-18-t1-macos.txt`,
  `results/2026-10-18-t2-macos.txt`, `results/arith.out`). The architect reviews the design as a
  whole; this review covers what an attacker can do under it: the segment fetch, the `forgetful`
  flag, the new frames and fields, segment identity, and the new length fields. No code exists for
  D1 to D16; the code read is what they extend.
- Surfaces read: `docs/REPLICATION.md` (the guarantee, corruption defense), `docs/reviews/
  calibration.md` (ten rows, applied below), `docs/design/limits.md`, `docs/ARCHITECTURE.md`
  ("Segment format", "Manifest and commit"), `docs/TESTING.md` (the fuzzing row), `docs/PLAN.md`
  ("Recovery tooling"), `docs/design/track-c.md` (D5, D10, D12, D13, D15, D18); the peer transport
  design, revision 2 at `38a6c6f` (`docs/design/peer-transport.md`: the threat model, D5, D8, D9,
  D11, D16, limits) with its security review and both re-checks (`peer-transport-security.md`, SR1
  to SR28) and the architect's (`peer-transport-design.md`, verdicts only); strata `main` `734949e`
  as merged at `c6df449`: `include/strata/index.hh` (`install` 783-842, `load_files` and
  `load_segment` 2400-2446, `segment_name` 85), `files.hh` (the manifest decoder's bounds 113-147),
  `format.hh` (footer and table-of-contents checks 196-238), `fuzz/fuzz_segment.cc` and
  `fuzz/fuzz_files.cc`, `CMakeLists.txt` (pins 10-32, fuzz targets 145-156); consensus `61149a2`
  (`core.h`: 476-490, 597-609, 720-732, 850-920, 955-972, 1140-1165; `types.h`: 55-67, 131-170;
  `transport/codec.h`; `snapshot_transfer.h`; `worker.h`: 290-325; `CMakeLists.txt`: 17-26);
  journal `cb66f3d` (`turn_log.h`: 270-310, 440-460, 525-560); `Kronuz/sha256` at `2bb34f0`
  (`sha256.cc`, 464 lines; `README.md`); `Kronuz/hashes` (`hashes.hh`, `crc32c.hh`);
  `phase4/format13`'s `docs/status/format13.md:113-114`; `research/no-stalls` at `4613018`
  (`HEAD` equals `origin`; the design cites a local `b9dddfd` that no longer exists as such).
- Checked by: reading and arithmetic. `quiet check` exited 1 (a strict hold:
  `scale-threads-mac2` until 23:45:51, `nostalls-core-r3` light until 23:53:44), so nothing was
  compiled, started, fuzzed, or measured; no probe needed a build, since none of D1 to D16 exists
  as code. Every figure below is the design's, cited code's, or arithmetic done by hand and shown.
  - Not run: the last line of this file.
- Citations: `design:N` is line N of `docs/design/durability-modes.md` at `8f8b043`; `pt:N` of
  `docs/design/peer-transport.md` at `38a6c6f`; code is at the commits above.

## Verdict

**APPROVE WITH CONDITIONS. The shape is right: a segment file is bound to a committed log entry
by its size, CRC-32C, and SHA-256, so no member can serve forged bytes to another; the fetch is
scoped by group and by a committed binding, which the peer transport's D5 already enforces for
group frames; segment identities are integers (the producing term and a sequence), so no name on
the wire becomes a path; the two cursors keep `quorum` and `all` at today's durability; and D5's
safety argument holds as written (the intersection argument was re-derived here). No finding is
high. Six are medium, and each is a bound or a rule the design states in words without a number
or a mechanism: the fetch has no stated limit on fetches, bytes, or chunks per requester, and the
server-side cap is "a fixed number" (DMS-1); the `forgetful` rewind is unbounded, so one key
makes a leader resend its retained log or its whole image at every reply and hold every outgoing
transfer slot (DMS-2); a false `forgetful` in one vote grant turns today's fail-closed conflict
into a silent rollback of `memory` writes, which the threat model's row prices at "only its own
resends" (DMS-3); segment mode removes the independent copy the corruption defense compares
against, so a poisoned or corrupt image reaches every replica and has no repair source (DMS-4);
the SHA-256 the binding relies on is specified by a portable implementation beside the
accelerated one already linked, at a speed that alone exceeds T2's follower budget, which is the
pressure behind the "CRC-32C from the leader" asymmetry (DMS-5); and the append's `commit` field
changes meaning for a reader one wire version back, which persists it as a durable commit (DMS-6).
Three are low (DMS-7 to DMS-9): value rules, a format-downgrade lever, and client-facing inputs.
Conditions bind step 3's rewind code, step 4's wire bodies, step 5's fetch and install code, and
T2's procedure; steps 1 and 2 may start with DMS-7's clean-close rule.**

What holds, and why:

- **Identity and path handling.** Segment files are named from integers today
  (`index_detail::segment_name`: `"seg-" + std::to_string(id) + ".sx"`, `index.hh:85`), the design
  adds the producing term to that integer identity (`design:404-408`, format 13's reservation at
  `format13.md:113-114`), and the fetch names identities, not names (`design:392-394`). A file name
  is never read from the wire, so there is no traversal to defend. The threat row for a non-member
  with a pinned key is right: host frames are node-level and the host authorizes them by the
  session's node (`pt:442-446`, PT-R5), and the fetch's group and incarnation are checked against a
  committed membership (`design:649`).
- **Binding.** A fetched file is checked whole against the size and checksums in a committed
  `Image` entry (`design:388-391`). Today's install already reads the whole image and checks the
  size, the footer's table-of-contents CRC, the document count, every section's checksum
  (`verify_all`), and the live-docs file against its manifest entry (`index.hh:2415-2446`), so the
  incremental install inherits those checks; SHA-256 adds what CRC-32C cannot give against a member
  that forges bytes, since a CRC is matched by appending four bytes. The manifest and segment
  decoders bound each count and length before allocating (`files.hh:123-141`, `format.hh:196-238`)
  and are fuzzed with checksums recomputed so mutations reach the decoders (`fuzz/fuzz_segment.cc`,
  `fuzz_files.cc`).
- **Replay and reordering.** Every new frame and field rides the peer transport's per-session TLS
  (`pt:205-228`, its replay row); `Image` entries are log entries, idempotent by position; the
  `Recovery` entry's term gates replacement only for entries older than it (`design:321-324`), so a
  stale leader's append fails Raft's term check before any of this applies.
- **The cursors.** `commit_durable` only grows, and a follower never replaces an entry at or below
  it (`design:321-324`); a checkpoint never records a position above it (`design:155-158`). The
  compromised-leader row is right as far as it goes (`design:652`): what it can roll back is what
  `memory` already admits losing.
- **D16's sizes** reproduce: `VoteResponse` 56 to 64 (one 8-byte boolean, as the codec's
  `boolean()` reads them), `AppendRequest` 104 to 136 (four 8-byte fields), `AppendResponse` 96 to
  112 (two), the records 48 to 64 (two each); `control_frame_bound` becomes `136 + 32·rpc_entries +
  rpc_bytes` against the transport's `104 + ...` (`pt:596-598`).
- **Raw results exist** for every figure the design cites as measured (calibration row 3): T1 and
  T2 at `0b407c9`, with `arith.py` and `arith.out`. T2's "Bytes per follower" column (`design:438-439`)
  is the input to DMS-5.

## Attackers considered

| Attacker | Under the design | Gains | Findings |
|---|---|---|---|
| On the network, no key | Everything new rides the transport's sessions (pt D1, D2); D11 rejects a datagram path | Nothing new | — |
| A member holding one key: a voter, learner, witness, or vouched source, in k groups with the victim (about 31 for an average peer, SR22's arithmetic) | Fetches any file of those groups; sets `forgetful`; reports `synced`; offers wire version N − 1; claims the segment formats it reads | The leader's disk, bulk lane, and outgoing transfer slots, for as long as it likes; a group-wide format downgrade | DMS-1, DMS-2, DMS-6, DMS-8 |
| A compromised voter | Grants with a false `forgetful`; lies in `synced` | A silent rollback of `memory` writes above `commit_durable` at honest replicas, where today's core fails closed; a `quorum` acknowledgement counting a copy not on disk (the row's residual, right) | DMS-3 |
| A compromised leader | Proposes an `Image` naming crafted files; sends false `commit_durable`, `sync_through`, `recovery_term` | Every follower installs the same bytes: a decoder fault fences them all, and no member holds an independent image to repair from; barrier storms on followers' devices | DMS-4, DMS-7 |
| A malicious input file: a segment from the network | Fuzzed decoders (`fuzz_segment.cc`), checksums first | A fence at best; the poisoned image persists past the leader's removal | DMS-4 |
| A client with valid credentials (through strato) | Session tokens; per-index settings; `quorum` writes on a shared core; the `manual` recovery command | A `session` read that waits on a token no log holds; an unclean election if the command rides the API; today's barrier cost on co-located indexes (not new) | DMS-9 |
| Local file access on a node | A false boot identity or clean-close record | That node, as the row says (`design:654`); the clean-close record needs its barrier rule | DMS-7 |
| A compromised dependency | `Kronuz/sha256` would be a new pin | Pinned by commit as the others are (`CMakeLists.txt:10-32`), or it floats (`GIT_TAG main` is the README's example) | DMS-5 |
| Replay or reordering across reconnects | TLS per session; entries by position; chunks need a token and offset | Nothing, once chunks match a request | DMS-1 |
| Version downgrade | A floor of N − 1 (pt D6, D7) | At a `written` group, a safety loss at the older reader, or a group-wide feature switch, whichever rule the design picks | DMS-6 |

## Rulings

| Decision | Ruling | Findings |
|---|---|---|
| D1 Commit in three steps | Sound; no network surface | — |
| D2 `memory` | Sound as stated; the precedents say the same thing | — |
| D3 The bounded barrier | Sound; `RFTW0002`'s new fields and the clean-close record need value rules | DMS-7 |
| D4 Hard state | Sound | — |
| D5 Forgetful voters | The safety argument holds (re-derived); the rewind has no bound and learners' flags are not addressed | DMS-2 |
| D6 Recovery | Sound within the model; "a conflict outside a recovery stays a detected bug" is defeated by one bit from one voter | DMS-3 |
| D7 Two cursors | Sound; `commit`'s meaning changes for a reader one version back; value rules | DMS-6, DMS-7 |
| D8 Segment mode | The binding to a committed entry is right; the fetch has no numbers; the hash's implementation and the leader asymmetry; the digest's independence is lost; the format rule holds a group back | DMS-1, DMS-4, DMS-5, DMS-8 |
| D9 The failover gap | No surface | — |
| D10 One vocabulary | Tokens and floors are client inputs without a bound or an authorization statement | DMS-9 |
| D11 No multicast | Right: no datagram path | — |
| D12 Partition-tolerant writes | Later; its own review when designed (the HLC is the leader's stamp; side logs are local) | — |
| D13 No leaderless replication | Right, for the digest reason the design gives, which DMS-4 shows segment mode weakens too | DMS-4 |
| D14 No stalls | The image-shipping row relies on the fetch's bounds | DMS-1 |
| D15 Live tuning | Replicated settings need an authorization statement | DMS-9 |
| D16 Formats and wire | Sizes reproduce; version-1 semantics of `commit`; value rules | DMS-6, DMS-7 |
| Threat model (`design:641-654`) | Row 649 right in shape, no numbers; row 650's asymmetry; row 651 understates both flags; row 652 omits the poisoned image's persistence; row 653's "fixed number" unnamed; row 654 right | DMS-1 to DMS-5 |

## Calibration checks applied

- Row 1 (a second implementation): SHA-256 from `Kronuz/sha256` beside OpenSSL 3's `EVP_sha256`,
  which `consensus::transport` requires (`consensus/CMakeLists.txt:26`) and strata::group therefore
  links; `Kronuz/hashes` has CRC-32C only (`hashes.hh`, `crc32c.hh`). A finding (DMS-5). The
  checkpoint batcher reuses small-indexes' D3-A as SI-R6 asked; the fetch reuses the transport's
  host frames, as the directory fetch does (`pt:1076-1079`).
- Row 2 (CI per platform): the branch's commits are docs and probes; the ledger says the probes
  build apart from strata's targets, so CI does not build them (`status`, "Last verified"). The
  code commits read are the pins at `c6df449`.
- Row 3 (baselines with raw files): T1 and T2 have theirs; no row cites a measurement it lacks.
- Row 4 (rulings addressed to this design): the peer transport's security conditions that touch
  host frames and transfers are SR22 (per-source transfer slots), SR25 (held credit requests), SR5
  and SR6 (host control pools), and PT-R5 (host frames authorized by the session's node); the
  design meets PT-R5 and SR6 and does not answer SR22's outgoing counterpart or size its fetch
  against SR5 (DMS-1, DMS-2).
- Row 5 (HEAD against origin): equal for `design/durability-modes` and `design/peer-transport`;
  `research/no-stalls`'s cited local commit `b9dddfd` has since moved to `4613018` on both.
- Row 6 (limits against Lucene, Tantivy, Xapian): no limit recommended here has a counterpart in
  the three engines (none ships a replication fetch), and none lowers a row of `limits.md`; the
  `Image` entry's file count needs no cap of its own, since the command limit bounds it (at the
  1 MiB default and about 76 bytes per file, about 13,800 files).
- Row 7 (mechanisms a fix relies on): the leader's hint-driven backoff exists (`core.h:962-966`);
  `snapshot_concurrency`'s 4 outgoing and 4 incoming transfers are Track C's D13 design
  (`track-c.md:673`), absent from consensus at `61149a2` (`git grep` finds no `snapshot_concurrency`),
  and SR22's per-source slot rule is owed (its C16); both are named as owed below, with the lead
  routing them as SR22 did.
- Row 8 (the honest peer whose view lags): for each rule proposed below, the honest case is
  constructed in the finding: a voter with two unclean restarts (two sessions), a follower whose
  `Image` entry is truncated, a learner before its membership commits, a node not yet upgraded.
- Row 9 (digest inputs from replicated state): T4's digest for a segment-mode follower is computed
  from the leader's bytes, so it is not an independent input (DMS-4).
- Row 10 (quote the cell): T2's "Bytes per follower" column, rows "One flush" and "16 MiB buffer"
  (`design:438-439`): 1,614 and 2,394 bytes per document; T2's target, "a segment-mode follower's
  CPU per document at most 0.1× a document-mode follower's, in both of T2's buffer configurations"
  (`design:737`).

## Findings

### DMS-1. The segment fetch has no stated bounds: fetches, bytes, and chunks per requester, and the server's slot rule (medium)

Attacker: a member holding one key, honest or not, in k groups with the server (a voter, a
learner, a witness, or a vouched source; the design lets any member fetch, `design:392-394`, and
read copies are learners up to every node, `design:126`).

Evidence:

- The threat row says the server "serves at most a fixed number of fetches per requesting node at
  once" (`design:653`) and names no number; the limits the row cites are the transport's for host
  control frames (4 KiB, 8 pending per connection) and host data "at its credit" (`pt:627-628`).
  Credit is granted by the receiver, which here is the requester: it grants group zero's window on
  the bulk lane to any peer (`pt:749-751`), so the requester sets its own pull rate, and the
  server's only brake is its send budget, which every peer shares round-robin
  (`track-c.md:744-747`). SR22 found the incoming counterpart of this gap for transfers; the fetch
  is the outgoing one, and the design's "one mechanism moves segment files" (`design:392`) also
  carries Track C's D18 retry and D10's repair fetches, so every file movement in strata goes
  through this unbounded path.
- The request and reply have no field rules: nothing says a fetch names an offset and a length,
  that `offset + length` stays within the entry's size without overflow, that a reply chunk carries
  the request's token and offset and arrives contiguous, or that a chunk for no outstanding request
  drops. The snapshot channel states each of these for its chunks (`snapshot_transfer.h`: a 65,536
  cap, `offset != offered_bytes` refused, `bytes.size() > application_cap - offset` refused;
  `worker.h:313-317`), and D9's fragments do too (`pt:636-644`).
- The requester's side has no bound either: how many fetches it keeps in flight per group and per
  peer, what group-zero credit it grants for them, and whether it fetches files for an `Image`
  entry before the entry commits. A deposed leader can propose `Image` entries that never commit,
  each naming new files (`design:383-391`); a follower that fetches ahead of commit pays disk
  reservations and bandwidth for files the truncation discards. Today's install reserves disk
  before it writes a byte (`index.hh:794-798`); the incremental install must reserve from the
  entry's declared sizes before the first fetch, and an `Image` the reservation refuses must be
  reported, not retried.
- A member removed from the group, not from the cluster, stays bound to its incarnation until
  compaction drops the entries naming it (`pt:387-392`, rule 2.1), so it can fetch that index's
  files for that window. It held them already; the row should say so.

What they gain: a server's disk read bandwidth and bulk-lane share for as long as they like, on
every group they share with it, and the eviction of honest fetches (D18 retries, repairs, and
installs) behind theirs; on the requester's side, disk and bandwidth spent on images a deposed
leader proposes.

Fix: in D8 and the threat model, numbers with mechanisms, each at least what an honest follower
needs for D9's replay bound: at most one fetch in flight per requester and group and 4 per
requester node at a server, work-conserving across requesters (SR22's rule, its outgoing form); a
fetch's `length` at most the receiver's announced host data bound (8 MiB, `pt:1079`) and
`offset + length` at most the entry's size, checked without overflow; chunks carry the request's
token and offset, arrive contiguous, and a chunk for no outstanding request drops, counted; group
zero's bulk-lane credit above w0 is granted and returned only while a fetch is outstanding on that
connection (the analogue of "bytes admitted from a node that no longer leads are not returned",
`pt:770-771`), so a peer that stops serving and keeps sending strands at most w0; a follower
fetches only for a committed `Image` entry, or at most one uncommitted entry ahead, and reserves
the entry's bytes first. The row's sentence on a removed member. T14 gains a row with one peer
fetching at its full window while another's install completes within 1.5× of the unslowed run,
as SR22's T6 case does for transfers.

### DMS-2. `forgetful` is an unbounded rewind lever: one key makes a leader resend its retained log, or its whole image, at every reply (medium)

Attacker: a member holding one key, in k groups with the leader. `AppendResponse` carries the
flag for every member (`design:373`), so a learner sends it too; D5 discusses voters only.

Evidence:

- D5: "A forgetful voter's replies say so, and the leader then lowers its matched index for that
  voter to what the voter holds and resends from there" (`design:253-254`). "What the voter holds"
  is the reply's `matched`, which the voter chooses. Today's core refuses exactly this for the
  reason it states: "Durable peers cannot lose a matched prefix on restart; never rewind below
  verified progress" (`core.h:957-958`), and the design removes the rule without a bound in its
  place (`design:255-257`; `track-c.md:730-731`'s "`matched` never moves back").
- The flag is state ("I am forgetful until I re-learn", `design:244-251`), sent on every reply
  until re-learning, while the rewind is an action; the design does not say the leader rewinds
  once per restart rather than on every reply that carries the flag.
- A `matched` at the log base makes the leader resend its retained log for the group, up to the
  group's share of D17's 16 GiB per node; one below the base makes it send a snapshot, the whole
  image, through the outgoing transfer slots, of which a node has 4 (`track-c.md:673`, a design
  figure, absent from consensus at `61149a2`). Nothing bounds outgoing transfers per destination
  node, the mirror of SR22's finding for incoming ones, so one node holding 4 slots across its k
  groups holds them all, and honest laggards' installs queue "without a timeout" (`track-c.md:
  674`). Each completed transfer is followed by another `forgetful` at 0.
- The threat row prices a false `forgetful` at "only its own resends" (`design:651`). The resends
  are the leader's work: its disk reads, its send budget, its transfer slots, and the bulk lane
  every honest follower shares.

What they gain: a leader's disk and egress for every shared group, continuously; honest laggards
that never catch up on any group that leader leads; after a reconnect, the same again.

Fix, in D5 and D16, with T6's mutants:

1. A `forgetful` reply lowers `matched` to no less than the highest `synced` that voter reported
   to this leader. An honest voter can lose only what it never synced (within the model a
   completed barrier is durable; D5's errseq residual is outside it, as the design says,
   `design:287-291`), so the rule loses nothing, and the honest cost of a rewind is the voter's
   unsynced window, at most `max_unsynced` (64 MiB per core, `design:594`). A reply below that
   floor is counted (`forgetful_below_synced`) and treated as the floor.
2. The leader rewinds at most once per peer per session (a restart is a new session with a new
   boot identity in its hello, `pt:550`); later replies in the session carrying the flag change no
   index and are counted (`forgetful_repeated`). The flag's standing meaning, that the voter grants
   no vote, is unaffected. First contact under a new leader is standard Raft: `matched` starts at
   0 and the hint-driven backoff (`core.h:962-966`) walks back, bounded by the retained log or one
   snapshot, as any restarted peer costs today.
3. The outgoing counterpart of SR22's rule: at most one outgoing transfer per destination node
   while any other destination waits, else up to `snapshot_concurrency`; the lead routes it to D11
   or Track C's D13 with SR22's C16.
4. T6 gains "a voter that flags `forgetful` on every reply with `matched` 0: the leader's resend
   bytes stay within one unsynced window per session, and no honest install waits on it".

### DMS-3. A false `forgetful` in one vote grant turns a fail-closed conflict into a silent rollback (medium)

Attacker: a compromised voter. Raft does not survive one, and the design says so
(`design:644-645`, `651`); the finding is that the row prices the flag wrong and that D6's own
claim, "a conflict outside a recovery stays a detected bug" (`design:323-324`), is defeated by one
bit from one voter.

Argument (rf = 3: A honest, B compromised, C honest and behind): e is committed at the memory
majority {A, B} in term T; C campaigns at U > T lacking e; B grants it, which an honest B would not
(its log holds e), flagged `forgetful`. C's quorum held a forgetful grant, so C writes a `Recovery`
entry and names U as `recovery_term` in its appends (`design:316-319`). At A, e sits above
`commit_durable` (it was a `memory` write) and at or below `commit_memory`, with term T < U, so A
replaces it and repairs to its checkpoint below the divergence point (`design:321-328`). Without
the flag, the same election makes C leader and the same append reaches A, where today's core
throws "conflict with committed log prefix" (`core.h:868-871`) and fences: the loss is the same,
but it is loud (a fenced replica with that message), and the operator learns a voter lied. With
the flag, A rolls back e and continues; readers may have seen e (`design:334-336`).

What they gain: covert rollback of `memory` writes above `commit_durable` at every honest replica,
with nothing but a counter to show for it; and `relaxed` groups under `aggressive` lose them
within the model too, as D6 already admits (`design:310-312`).

Fix: the row (`design:651`) names this as the flag's cost beside the resends. A leader that writes
a `Recovery` entry logs, at warning level, every forgetful grant that made it (incarnation, node,
the grant's last index and term) and raises a counter; a follower that replaces entries under
`recovery_term` logs the range it dropped (first index, count, their terms) and raises one, so the
loss is loud where today's fence was. T6's mutant list gains "a voter that flags `forgetful` on a
grant it would otherwise refuse: the simulator's checker counts the resulting rollback as a
compromised-voter outcome, outside the model, never as a pass". Within the model nothing
changes, which is why this is medium and not high.

### DMS-4. Segment mode removes the corruption defense's independent copy: a poisoned or corrupt image reaches every replica and has no repair source (medium)

Attacker: a compromised leader, or no attacker: a leader whose memory corrupts a buffer before
checksumming (REPLICATION.md:19, outside the model but what the digest comparison exists to
catch) or whose flush has a bug.

Evidence:

- REPLICATION's determinism contract rests on comparing a logical digest across replicas, "because
  divergent replicas pass every checksum" (REPLICATION.md:64-66), and D13 rejects leaderless
  replication partly for lacking "a digest to compare replicas by, which strata's corruption
  defense relies on" (`design:578-579`). A segment-mode follower applies nothing (`design:377-382`)
  and holds the leader's bytes, so its digest at p is the leader's by construction: T4's oracle,
  "each replica's digest equals the leader's at equal applied positions" (`design:739`), is
  vacuous for it, and so is "replicas never diverge" (`design:328`) as a check.
- A segment file from the network is a new input to the format decoders. They are fuzzed with
  checksums recomputed (`fuzz/fuzz_segment.cc:1-10`, 78-98, 170-193), so a crafted file fences at
  worst; but every follower installs the same file, so one file fences them all at once. Today a
  fenced replica repairs from "an image from any member of the group, verified whole before
  activation" (`track-c.md:421-423`); in an all-segment-mode group every member's image is the
  same bytes, and a promoted follower replays its log from its installed position
  (`design:409-411`), the poisoned one. The peer transport's residual for a compromised leader,
  "fence replicas, and stall the group" (`pt:213`), ends when the leader is removed; this does
  not. Rebuilding from the log needs a replica that applies, and the retained log reaches back only
  to the last checkpoint, which in segment mode is "its durably installed image" (`design:419`).
- The leader's own corruption before hashing is the same case without an attacker: the hash binds
  the corrupt bytes correctly.

What they gain: with one file, the read service of every follower of a group, persisting past
their own removal; a corrupt leader loses the one check that catches it.

Fix, for the architect to choose among and the design to state: (a) one applying replica per group
(a document-mode voter or learner, whose digest is independent; it costs that replica document
mode's CPU, so the group figure at rf = 3 rises from 0.37× toward 0.69×, arithmetic from T2's
parts), or (b) a sampled apply-check on segment-mode followers: for a random sample of committed
entries, the follower looks up each document of the batch in the installed image (get-by-id and
version) and compares with what apply would have produced, which costs O(sample) and catches a
leader whose segments disagree with its log, or (c) the `Image` entry carries the leader's logical
digest at p and followers in document mode, where any exist, compare theirs, with (b) where none
do. Whichever: T4 states an oracle that is not the leader's own bytes for segment-mode groups, the
threat model's row 652 gains the persistence sentence, and the test engineer's gate row for
Phase 3R fuzzes the install entry (`load_segment` with `whole`, then every reader format 13 adds:
points, stored blocks, the primary-key filter, the docid map) with the network as the source,
one CPU-hour per decoder as TESTING.md's row asks.

### DMS-5. The binding hash is specified by a portable implementation beside the accelerated one already linked, and its cost is what drove the "CRC-32C from the leader" asymmetry (medium)

Attacker: none; this is the cost and the correctness of the mechanism every other binding claim
rests on.

Evidence:

- D8 names "SHA-256 from Kronuz/sha256" (`design:386`). That library is one 464-line portable
  source file (`sha256.cc` at `2bb34f0`); a search for SHA-NI or ARMv8 SHA2 intrinsics
  (`sha256rnds2`, `_mm_sha256`, `vsha256`, `__ARM_FEATURE_SHA2`) finds none, and its README states
  no throughput. OpenSSL 3, which `consensus::transport` requires (`consensus/CMakeLists.txt:26`)
  and strata::group links through it, dispatches `EVP_sha256` to those instructions. One mechanism
  twice (calibration row 1), and the slow one chosen.
- Arithmetic on T2's own bytes (`design:438-439`): at a portable implementation's typical 300 MB/s
  (public figures, not measured here), 1,614 bytes per document cost 5.4 µs and 2,394 cost 8.0 µs;
  at 2 GB/s with the extensions, 0.8 and 1.2 µs. The measured follower costs are 1.81 and
  2.61 µs, and T2's target is 0.1× of 42.80 and 129.30 µs, so 4.28 and 12.93 µs. With the portable
  hash the one-flush row fails (1.81 + 5.4 = 7.2 µs, 0.17×) and the 16 MiB row passes (10.6 µs);
  with the accelerated one both pass (2.6 and 3.8 µs). The leader hashes every shipped byte once
  too: 5.4 µs on 42.8 is 13% of its CPU per document, against 2% accelerated.
- The threat model then hashes only files from non-leaders: "A file fetched from anyone but the
  leader must match the SHA-256 ...; CRC-32C covers corruption from the leader" (`design:650`),
  while D8 says a follower "verifies each whole against the sizes and checksums the entry names"
  (`design:388-389`). Two rules for one check. The asymmetry also needs the follower to know,
  per session, which node is "the leader" in the trust sense; its own view can lag a term, and a
  deposed leader's files for an entry that never commits are discarded at truncation anyway, so
  the distinction buys nothing a single rule does not.
- The `Image` entry's per-file cost is about 76 bytes (identity 16, size 8, CRC 4, SHA-256 32, a
  live-docs reference); at 37 segments and one refresh a second per group, 2.8 KB per second per
  group, 2.8 MB/s of log at 1,000 groups: not a cost that argues for dropping the hash.

What this costs: at the portable speed, T2's target is missed in one of its two rows by the hash
alone, which is the pressure that produced the leader asymmetry; a second SHA-256 implementation
in the link set; a pin that floats if the README's `GIT_TAG main` is copied.

Fix: one rule, SHA-256 over every fetched file whatever its source, streamed while the file is
written and compared before the rename, with CRC-32C staying what today's install checks per
section; the implementation is the accelerated one in the link set (OpenSSL's `EVP_sha256`), or
`Kronuz/sha256` once it dispatches to the SHA extensions with a measured figure, pinned by commit
as every other library (`CMakeLists.txt:10-32`); T2's procedure (`design:737`) measures with the
chosen one on both platforms' instruction sets, and the design's "until then it is an estimate"
(`design:650`) names the implementation it estimates.

### DMS-6. The append's `commit` field changes meaning, and a reader one wire version back persists it as a durable commit (medium)

Attacker: a member that offers wire version N − 1, which the floor allows (`pt:486-530`; the
transport's row prices it at "features only", `pt:228`); or no attacker, since format activation
is procedure only until Track D8 exists (`pt:1414-1416`) and a node can lag an upgrade.

Evidence:

- D7: "the append's `commit` carries `commit_memory`, clamped to the verified prefix as today
  (`core.h:914`); new fields carry `commit_durable` ..." (`design:371-373`). A version-1 session
  has no new fields. Its follower takes `commit` as its commit index (`core.h:914`), persists it
  with the batch as hard state (`design:111-112`; `core.h:785`), applies up to it, refuses to
  replace anything at or below it (`core.h:868-871`), and its older host checkpoints at it, with no
  D1 ceiling at `commit_durable`. After a D6 truncation above `commit_durable`, that follower fails
  closed on the conflict, and its checkpoint can hold a truncated entry, so it cannot rejoin as the
  same incarnation. D16 says the feature "keeps off in a group while any member's session runs the
  older version" (`design:623-624`, citing `pt:523`) and names no mechanism: who refuses the
  `acks: written` configuration entry, and what a `written` group does when a version-1 session
  appears later.
- Coalesced heartbeat records carry a commit index too (`track-c.md:629-631`), with the same
  reader.
- A version-1 reply's `matched` means synced (today's semantics), so counting it for both cursors
  is right; what is wrong is only what the leader tells it.

What they gain: as a lever, one member offering N − 1 either switches a `written` group back to
`synced` semantics (if the design chooses the feature switch) or is itself left unsafe (if it does
not); as an upgrade hazard, a lagging node that cannot rejoin after a correlated loss.

Fix, in D7 and D16 before step 4's bodies land: an append or heartbeat record to a version-1
session carries `commit_durable` in `commit`, since that is what a version-1 reader's commit index
means, and the follower is counted for both cursors from its `matched`; a `Membership` with
`acks: written` is proposed only while every member's session is at version 2 or later, and every
replica refuses to apply one otherwise (D18's rule for membership changes, `pt:1191-1240`), so the
switch is as committed as any other configuration; T12 gains "a version-1 session in a `written`
group receives `commit_durable` and never a `commit` above it".

### DMS-7. Value rules for the new fields and records (low)

Attacker: a compromised leader or member for the wire fields; local file access for the journal's.
D9 of the peer transport asks every new decoder to bound lengths and counts before allocating
(`pt:666-668`) and, after SR8, to state value rules; D16 states sizes only.

Rules the design should state, each a protocol error or `Corruption` as the codec treats one
(`codec.h`: `boolean()` refuses a value above 1; `get64` throws on short input):

1. `AppendRequest`: `commit_durable ≤ commit`, `sync_through ≤ leader_last`, `leader_last ≥
   previous + entries`, `recovery_term ≤ term`; the follower clamps `commit_durable` to its
   verified prefix as it clamps `commit` (`core.h:914`), so a leader cannot set a floor over
   entries the follower has not verified.
2. `AppendResponse` and the reply record: `synced ≤ matched`; `forgetful` and `synced` only with
   `success`.
3. `VoteResponse`: `forgetful` only with `granted`; a grant from a voter whose own policy forbids
   it is the voter's lie (DMS-3), not a decode error.
4. `Recovery`: a payload of exactly 16 bytes; `last index` at or below the entry's own index.
5. `Image` (strata's command): the file count bounded by the remaining bytes over the smallest
   file record before allocation, as `files.hh:129` does; each size and the sum reserved before
   any fetch (`index.hh:794-798`), the entry refused and reported when the reservation fails;
   identities strictly increasing within the entry, so a duplicate is refused.
6. `RFTW0002`: the end-of-turn payload exactly 12 bytes and its synced turn at most the turn it
   ends; a boot identity of zero or one the platform cannot read reads as unknown, which recovery
   treats as unclean (forgetful), never as clean; the clean-close record is written only after a
   barrier covering every written turn, is itself synced, and counts only as the newest epoch's
   last record, so a later open record cancels it.

### DMS-8. One member's claimed segment formats hold the whole group's format back (low)

Attacker: a member holding one key.

Evidence: D8's "Mixed versions" states two rules: "the leader writes only segment formats every
member reads; a member that cannot read an installed segment applies the batches itself"
(`design:431-432`). Under the first, a member claiming it reads only an older format makes the
leader write that format for everyone, the lever SR4 found in the hello's limits and PT-R26 closed
by "limits hold back only their node" (`pt:565-575`). Under the second alone, the claim costs only
the claimant document mode's CPU.

Fix: keep the second rule only; a member's readable formats bind only itself, and the leader
writes its best format. Where the member announces them (its hello or its membership record) is
Track D8's to place; the design names which.

### DMS-9. Client-facing inputs without a bound or an authorization statement: session tokens, replicated settings, `manual` recovery (low)

Attacker: a client with valid credentials, through strato.

Evidence:

- A `session` token is `(term, index, position in batch)` and a replica serves the read once its
  visible position reaches it (`design:501`); a token whose entry D6 lost reports `lost`. A token
  naming an index no log holds (a forged or future one) must be refused at once, never waited on
  past the request's deadline, or each such read holds a read slot (`Limits::reads`, 64,
  `types.h:63`) for the deadline.
- `unclean_recovery`, the ack and read floors, and the replication mode are replicated index
  settings a request changes (`design:612-614`); `aggressive` loses `memory` writes within the
  model (`design:310-312`). The design should say these take the index's administrative privilege,
  not a writer's, as choosing the preset does.
- `manual` "lets one election run under `balanced`'s rule" through "an operator's command through
  the strata tool" (`design:313-314`; PLAN's "Recovery tooling", `PLAN.md:660-668`). The command's
  path and authorization are unstated: if it rides the HTTP API, a writer can run an unclean
  election; the peer transport's admin path (signed statements, `pt:1126-1190`) or a local socket
  under the key file's mode rule are the two fits.
- A `quorum` write in a `replicated` index "sets `sync_through`, so its barriers run at once"
  (`design:517-518`): a client streaming tiny `quorum` writes drives a shared core's barrier rate
  to the device's limit, which co-located `memory` indexes feel in write latency. It is today's
  cost (every write is a barrier today), so not a new lever; the design can say so in D14's
  barrier row.

Fix: a bound on tokens (refused above the replica's last index plus the leader's reported last,
counted), the privilege for the settings, the command's path, and one sentence in D14.

## Blocking items

1. DMS-2 blocks step 3's rewind code in `Kronuz/consensus` (`design:821`) until D5 states the
   `synced` floor, the once-per-session rule, and T6's mutant; the rest of step 3 (the `Written`
   event, the cursors, `Recovery`, `Repair{promotion}`, the ceiling, `Membership::acks`) may be
   drafted.
2. DMS-6 blocks step 4's wire bodies (`design:822`) until D7 and D16 state what a version-1 session
   receives in `commit` and who refuses `acks: written`, with T12's case.
3. DMS-1 blocks step 5's fetch protocol and incremental install (`design:823`) until D8 and the
   threat model carry the numbers and rules above; DMS-5's single hash rule and implementation
   land with them.
4. DMS-4 binds the full design review: T4 names an oracle for segment-mode groups that is not the
   leader's bytes, and the owner chooses among (a) to (c).

Steps 1 and 2 (`RFTW0002` in `Kronuz/journal`, the boot identity and directory barrier in
`Kronuz/completion`) may start after the architect's preliminary ruling, with DMS-7's item 6 as
step 1's rule. Step 6 (the simulator's fault classes) is unaffected, and gains DMS-3's mutant.

## Conditions

1. C1 (DMS-1): the fetch's limits table in D8 (fetches per requester and group, per requester
   node, work-conserving; `length` at most the announced host data bound; `offset + length` within
   the size without overflow; token and contiguity for chunks; credit returned only while a fetch
   is outstanding; fetch only for committed entries or one ahead; reservation before the first
   fetch; the removed-member sentence) and T14's row, before step 5.
2. C2 (DMS-2): the `synced` floor and the once-per-session rewind in D5, the outgoing slot rule
   routed with SR22's C16, the threat row corrected, and T6's mutant, before step 3's rewind code.
3. C3 (DMS-3): the row's cost named; the `Recovery` and replacement logs and counters in D6; the
   simulator's mutant with its verdict outside the model, before the full review.
4. C4 (DMS-4): T4's oracle for segment-mode groups and the owner's choice among (a) to (c); row
   652's persistence sentence; the install-entry fuzz row for Phase 3R's gate, before the full
   review.
5. C5 (DMS-5): one SHA-256 rule for every source; the accelerated implementation named and pinned
   by commit; T2 measured with it, before T2 runs as a gate row.
6. C6 (DMS-6): `commit_durable` in `commit` to a version-1 session; `acks: written` refused while
   any member's session is older, at proposal and at apply; T12's case, before step 4.
7. C7 (DMS-7 to DMS-9): the value rules in D16; the clean-close and boot-identity rules in D3 and
   D5 before step 1; the second mixed-version rule only in D8; the token bound, the settings'
   privilege, and the `manual` command's path in D10 and D15; in the next revision.

Not run: nothing compiled, started, fuzzed, or measured (`quiet check` exited 1: a strict hold by
`scale-threads-mac2` until 23:45:51, a light one by `nostalls-core-r3` until 23:53:44), and no
probe needed a build, since none of D1 to D16 exists as code; the branch's `count_syncs` and
`sizing` probes were read, not run, and their raw results read as the design's evidence; the
arithmetic above is by hand, shown inline, with no script; `Kronuz/sha256`'s speed is a public
typical figure, not a measurement; no socket crossed, so the fetch, the rewind, and the mixed
version paths are judged from the design's text and the code's current form; no Linux or FreeBSD;
Detent not read; Track C, the simulator, and the PKI designs read at the lines cited, not run.

## Re-check (revision 1)

- Date: 2026-10-19. Reviewed: `docs/design/durability-modes.md` at `fc30ed3` on
  `design/durability-modes` (revision 1; the ledger `docs/status/durability-modes.md` with the
  lead's calls on Q9 and Q10), in the worktree `~/code/Kronuz/strata-wt/review-durability-sec`.
  Scope: conditions C1 to C7 at `file:line`, then the new surface: the `Create` command (D1, D16,
  SI-R25, T16), the `forgetful` and `recovery` flags on kinds 1, 2, and 6 (D6, D16), and segment
  shipping with D18's sampled rebuild (D17, D18, Q9).
- Also read: the peer transport at `38a6c6f` (D5 rules 2.1 to 2.3, D6, D7, D8's hello table, D9's
  kinds and value rules, D17), format 13 at `43ee403` (F13-R41 `:470-482`, F21 `:1152-1170`,
  F13-R51 `:1116-1123`), small-indexes' re-check (SI-R25, `small-indexes-design.md:662-676`),
  Track C's checkpoint-record rule (`track-c.md:582-587`), consensus `61149a2` (`core.h:755-800`,
  `845-920`, `940-972`; `snapshot.h:35-60`; `snapshot_transfer.h:55-85`; `snapshot_channel.h:
  225-260`; `worker.h:294-320`; `codec.h:118-130`), strata `main` as merged at `1365cd1`
  (`index.hh:333-346`, `2250-2275`; `files.hh:63`), and the designer's model
  (`bench/durability/model/forgetful_model.py`).
- Checked by: reading, arithmetic, and three probes, all in `.scratch/durability-sec-r1/`
  (kept; `quiet check` exited 0, run at `nice -n 19` anyway):
  1. `liar_model.py` (a 44-line patch of the designer's model: `--liar K` makes voter K flag
     `forgetful` falsely on kind 6 probes and kind 2 answers and count itself when it starts a
     round; nothing else changes, and without `--liar` it reproduces the original's state space
     exactly, `baseline-orig.out` against `baseline-copy.out`, 1,432,552 states at depth 8) with
     `liar_schedules.py` (forced schedules, `liar_schedules.out`) and one bounded search
     (`liar-round-3v.out`: 5,667,532 states to depth 9, stopped by the state limit one step short
     of the trace below, so the trace was forced, not found).
  2. `d18_residual.py` (`d18_residual.out`): the verifier's detection probability against one
     planted segment, arithmetic.
  3. `create_replay.py` (`create_replay.out`): D1's creation-replay rule as literally written,
     against a retained log holding a second `Create` for the same UUID after a checkpoint record.
  Copies of the two cited designs are beside them (`peer-transport-38a6c6f.md`,
  `format-13-43ee403.md`). Not run: the last line of this section.
- Citations: `design:N` is line N of `docs/design/durability-modes.md` at `fc30ed3`; `pt:N` of
  the peer transport at `38a6c6f`; `f13:N` of format 13 at `43ee403`.

### Verdict

**APPROVE WITH CONDITIONS. C1 to C7 are closed: every number, rule, and gate row the preliminary
review asked for is in revision 1 at the lines below, and the two mechanisms it changed rather
than adopted (one byte mover on the snapshot transfer instead of a bounded fetch, DMS-1; a
per-session rule instead of refusing `acks: written` while a session lags, DMS-6) are stronger
than what was asked. The new surface holds in shape: a `Create` or an `Image` is a log entry, so
no peer but the term's leader can inject or replay one; a foreign UUID refuses with nothing
removed; every new field is fixed-size, a boolean is refused above 1, and the fields ride version
3 through the N and N − 1 range. Four new findings, one high: D1's creation-replay rule, read as
written, wipes an index on every replica that replays a log holding a duplicate `Create` after a
checkpoint record, and an honest client's retry after a lost reply is enough to put one there
(DMS-10, `create_replay.out`). Two medium: D18's one-in-64 sample is the honest-fault figure, not
the attacker's, since the leader chooses which segment to corrupt, drives the debt that skips
samples, and merges the evidence away under a merge check that re-runs over its own inputs
(DMS-11, `d18_residual.out`); and D5's rewind floor is the voter's own `synced` report, keyed by a
boot identity the voter declares, so a lying voter still buys a retained-log resend per
reconnect, the gap DMS-2 was meant to close (DMS-13). One low: D17's file list has no stated size
bound and its sum no overflow rule (DMS-12). The forged `forgetful` flag does what DMS-3 priced:
with one honest OS crash it turns a stall into a lost `memory` write after `recovery_timeout`
(`liar_schedules.out`, 10 steps, within the model), under the ruled rule as under revision 1's,
and never when every voter must answer; the row is right and gains one sentence.**

### Conditions C1 to C7

| Condition | Ruling | Where, at `fc30ed3` |
|---|---|---|
| C1 (DMS-1): the fetch's limits and rules, T14's row | **Closed.** The requester-driven fetch is withdrawn (`design:1015`); a receiver asks with `Request` and the source streams. Limits with mechanisms: one transfer per (group, receiver), 4 per receiver node, FIFO and work-conserving (`design:1083-1086`); a `Request` names a committed `Image` or the source's latest checkpoint, refused for an entry not committed in the source's log (`design:1083-1084`); chunks keep the snapshot channel's rules (token, offset equal to the bytes received, at most 65,536, final marker; `design:1067-1072`, against `snapshot_channel.h:241-242` and `worker.h:316-319`, which enforce exactly those), one changed with a reason (an unowned chunk drops, counted, where `snapshot_channel.h:240` throws); reservation of the missing files' bytes before the first chunk, a refusal reported (`design:1064-1066`); transfers only for committed entries (`design:1083`); the group's bulk window paces the bytes (`design:1050-1052`, `pt:840-843`); large transfers in `snapshot_concurrency`'s slots, at most one per destination while another waits, routed to Track C's D13 with SR22's C16 (`design:1087-1094`); the removed-member sentence (`design:1103-1105`); the threat row (`design:1146`); T14's one-peer-at-full-window row (`design:1293`) and T15 (`design:1294`). The `length`, `offset + length`, and credit-return items of C1 are moot: there is no requester-named range. DMS-12 below is what the new list adds |
| C2 (DMS-2): the `synced` floor, once per session, the outgoing slot rule, the row, T6's mutant | **Closed as asked.** The floor (`design:448-452`, `forgetful_below_synced`), once per restart keyed by the hello's boot identity through `PeerRestarted{incarnation}` (`design:453-458`, `1030`; `pt:550`, "fresh and nonzero at each process start"), lowering never lowers a counted commit (`design:459-460`), the outgoing rule routed (`design:1087-1090`), the row (`design:1149`), T6's "a rewind below the voter's reported `synced`, or twice in one restart" (`design:1285`), the Track C change (`design:1207`). DMS-13 reopens the floor's assumption: both inputs are the voter's own |
| C3 (DMS-3): the row's cost, the logs and counters, the simulator's verdict | **Closed.** "Loud, not covert": `recovery_grants` with incarnation, node, last index and term at warning level; `recovery_replaced` with the range (`design:566-571`); the row (`design:1149`: f honest forgetful voters plus one lie reach f + 1); T6's mutant counted outside the model, never a pass (`design:1285`). The probe confirms the pricing and adds the exact cost (below, "The forged flag") |
| C4 (DMS-4): T4's oracle, the owner's choice, row 652's persistence, the install-entry fuzz row | **Closed.** T4's oracle is a rebuild, not the leader's bytes, with a seeded leader-side mutant (`design:1283`); the choice is Q9 (`design:1307-1319`), the lead's call option 2 (`status:200-201`); the row carries the persistence sentence (`design:1148`: "a poisoned image outlives the leader's removal with no independent copy unless `verify: replica` keeps one"); T15 fuzzes the install's decoders with the network as the source, one CPU-hour per decoder (`design:1294`); REPLICATION's changes (`design:1193-1194`). DMS-11 is what the chosen option's residual costs against a leader that chooses |
| C5 (DMS-5): one SHA-256 rule, the accelerated implementation, T2 with it | **Closed.** One rule for every source, streamed, compared before the rename, CRC-32C kept per section (`design:1073-1077`); OpenSSL's EVP, already linked through `consensus::transport`, its version logged at startup under the transport's dependency row; Kronuz/sha256 not added (`design:1078-1082`); the cost redone at 0.5 and 1.5 GB/s, hashed once by the leader and once by every follower (`design:765-771`, `776-778`); T2 measures EVP with and without SHA extensions and on arm64, over loopback TLS, from both sources (`design:1281`). "Pinned by commit" does not apply to the platform's OpenSSL; the transport's row covers it (`pt:205-228`), which is the right place |
| C6 (DMS-6): `commit_durable` in `commit` to a version-1 session; `acks: written` refused while any member's session is older; T12's case | **Closed, by a stronger rule than asked.** `commit` carries `commit_durable` to every session, which is also the persisted commit index; a new field carries `commit_memory` (`design:662-665`, `691`, `1009`); heartbeat records the same (`design:1013`). Instead of refusing `acks: written` per session, D7 makes the lagging session safe on its own: a member on a version-2 session replies after its barrier, is counted for both cursors by `matched`, grants nothing as a forgetful voter over it, and is never rewound (`design:666-671`); `acks: written` is a log format that every replica refuses before activation (`design:635`, `1023`), and activation needs every node at the version (`pt:523`). T12's case (`design:1291`); the row (`design:1150`). Re-derived: a version-2 reader of kind 3 takes `commit` as its commit index (`core.h:914`), persists it (`core.h:785`), and under this rule never holds more than is durable, so a D6 truncation never conflicts with its committed prefix; the sizes decide control against data per session version (`design:1009`), so no version-3 field reaches a version-2 decoder |
| C7 (DMS-7 to DMS-9): value rules, clean-close and boot identity, the second mixed-version rule only, the token bound, the settings' privilege, `manual`'s path | **Closed.** D16's value-rule column (`design:1005-1014`): `commit ≤ commit_memory`, `sync_through ≤ leader_last`, `leader_last ≥ previous + entries`, `recovery_term ≤ term`, both commits clamped (`1009`); `synced` only with `success` and at most `matched` (`1010`); `forgetful` only with `granted` on kind 2 (`1008`); `Recovery` exactly 16 bytes, its last index at or below the entry's (`1024`); `Image`'s count bounded by the remaining bytes, identities strictly increasing, sizes reserved (`1033-1036`); `RFTW0002`'s 12-byte payload with S at most its turn, the 16-byte boot identity (`1026`). One deviation, argued and right: `forgetful` on a failed append reply too, since a forgetful voter's first reply is a failed consistency check the leader must see (`1010`); the floor uses the last `synced` the voter reported with a success. D3: the clean-close record only after a barrier covering every written turn, itself synced, the newest epoch's last record, cancelled by a later open record; an unreadable or zero boot identity reads as unclean (`design:382-386`, `1152`). D8: no member announces formats, the leader writes its best activated one, a member that cannot read an installed segment applies the batches itself (`design:758-761`). D10: tokens above the known last index refused at once and counted (`design:857`); presets, floors, `unclean_recovery`, `acks`, and the mode take the administrative privilege (`design:875-876`, `986-988`); `manual` through the admin path's signed statements (`design:547-549`; `pt:1126-1150`); D14's barrier sentence (`design:967`) |

### The new surface

- **`Create` (D1, D16, T16).** A `Create` is a strata command in the one log, carrying the UUID
  `create()` takes and every manifest stores (`design:298-300`; `index.hh:335, 344`; `files.hh:63`).
  Only the term's leader appends, a stale leader's append fails the term check
  (`core.h:846-848`), and entries ride the session's TLS, so no peer forges or replays one; the
  path an index lives at is the group's, never a field of the command, so there is no traversal
  and no adoption of another group's directory. A `Create` whose UUID differs from the manifest at
  the path refuses with a named error and removes nothing (`design:306-307`); every other `create()`
  and `open()` keep `clear_uncreated`'s refusal (`design:307-309`; `index.hh:2264-2275`). What the
  rule does not say is what a *second* `Create` for the *same* UUID means once the first has been
  superseded by a checkpoint record: DMS-10.
- **The forged flag (D6, D16).** Kinds 1, 2, and 6 gain one boolean each, refused above 1 as the
  codec reads one (`codec.h:124-127`), on version 3 only. A flagged kind 6 from a non-voter cannot
  count: the count is of "distinct voters of the membership in force" (`design:506`). A liar's
  flagged kind 6 counts once however often it is sent. Replay across sessions is the transport's
  (none); within a session a duplicate answer is the same voter's. What a single lie buys, with
  the designer's model extended by one liar (`liar_schedules.out`), at rf = 3 within the model
  (one honest OS crash, process crashes and partitions unbounded):
  1. Voter 0 leads at term 1 with votes from 1 and 2; `v0` is written and replicated to 1, so it is
     committed at the memory majority {0, 1}; voter 1 loses power and its unsynced `v0`.
  2. Voter 1's flagged probe reaches the liar 2, which counts itself: f + 1. It starts a recovery
     election at term 2 and asks only voter 1, which grants (its log is now empty) and is flagged.
     Voter 0, which holds `v0`, is never asked.
  3. `recovery_timeout` passes with no leader (voter 0 cannot win a normal election: 1 grants
     nothing while forgetful and the liar refuses it) and a majority answered, every answer a
     grant, two flagged: the liar leads at term 2 without `v0`. Voter 0 replaces it under
     `recovery_term`: `memory` lost inside the model, 10 steps.
  The same schedule under `round_heard` (every voter must answer) is blocked at step 9, since voter
  0 would refuse; without the liar the round never starts (step 7 not enabled); and the ruled rule
  (`fplus`, Q3 as the owner ruled it) loses the same write in 8 steps through a normal election, so
  the lever belongs to any count of self-declared flags, not to revision 1's correction. In the
  real system voter 0's heartbeats would re-learn voter 1 and the leader-contact guard
  (`design:513-514`; `core.h:770`) would make it ignore the liar's asks, so the attack needs voter 0
  unable to reach voter 1 for `recovery_timeout`: a partition, or a process crash of the leader
  (unbounded in the model; it restarts holding `v0` and cannot be re-elected). Under `balanced`
  that is 5 minutes of a stall the liar can cause anyway by refusing votes; under `aggressive`, one
  election timeout. This is DMS-3's finding with its cost now exact; the design's row and D6's
  "Loud, not covert" already name it. Condition: the row says "after `recovery_timeout`, with the
  write's holders unreachable; never when every voter answers", so an operator knows which policy
  resists a compromised voter at the price of availability (`manual`, or `balanced` with no
  timeout). No new finding.
- **Segment shipping and the sampled rebuild (D17, D18).** A corrupt or malicious leader's segment
  is bound by size, CRC-32C, and SHA-256 to the committed `Image` and by the list's equality with
  the entry's files (`design:1073-1078`); the install's decoders are fuzzed from the network (T15);
  so a crafted file either matches what the leader committed or is refused. What the binding
  cannot catch is a segment the leader built wrong on purpose, which is D18's job and DMS-11's
  subject.
- **Bounds and versions of the new fields.** Every size reproduces: kind 1 +8, kind 2 +8, kind 3
  +32 (four 8-byte fields), kind 4 +16, kind 6 +8, records +16; `Begin` 6,480 + 52 = 6,532 (the
  descriptor's 8 + 32 + 4 + 8); `Request` 168 + 16 = 184; `Missing` 4 + 795 × 8 = 6,364 within
  6,532; `control_frame_bound` 136 + 32·entries + bytes at version 3 (`design:1007-1014`). All ride
  version 3, offered through the range (`design:997-1001`; `pt:486-531`): a version-3 message on a
  version-2 session is a protocol error (`pt:494-496`), and `RFTXFR02` is a new magic a version-2
  reader refuses. The kind-3 control-or-data decision is per session version (`design:1009`).

### New findings

#### DMS-10. A duplicate `Create` after a checkpoint record wipes the index at the next replay, on every replica (high)

Attacker: a client with the privilege to create an index, through strato, or a compromised
leader; or no attacker: a client whose `Create` reply was lost and who retries, which strato's
idempotent "create if absent" would do on its own.

Evidence:

- D1: "Replay decides by the log: with a checkpoint record of the index after the creation record,
  the index opens at that record's generation; with none, the log holds every write since the
  creation, so replay removes the residue under strata's names ... and applies from the creation,
  provided every manifest present carries the record's UUID" (`design:302-306`); "the log keeps an
  index's creation record, and every record after it, until a checkpoint record of that index
  lands, which truncation by records gives" (`design:300-301`; SI-R25's invariant,
  `small-indexes-design.md:673-674`). A checkpoint record supersedes the group's earlier records
  (`track-c.md:586-587`), the first `Create` among them.
- D1's live refusal does not help: "every other `create()` and `open()` keeps today's narrow
  `uncreated()` and its refusal" (`design:307-309`) is apply's verdict for the second `Create`, but
  the entry is in the log whether its verdict was accepted or rejected, and nothing says the leader
  refuses to propose one for an index it holds open.
- So the retained log can be `[Checkpoint(U, g), Create(U), writes...]`. Read as written, "the
  creation record" is the second `Create`, no checkpoint record of the index follows it, every
  manifest carries U, so replay clears the index and applies the writes after the duplicate
  (`create_replay.out`: "index holds 2 documents at generation 0 (before: 100 at 7)"). Each
  replica that restarts does the same; a replica that has not yet restarted keeps 100 documents,
  so the group's digests diverge until the restarted ones win an election or the others restart.
  The writes between the first `Create` and the checkpoint record are gone from every replica's
  log, so nothing rebuilds them.

What they gain: an index's contents up to its last checkpoint, on every replica of the group,
removed by the group's own recovery; with the honest retry, no attacker is needed.

Fix, in D1 and T16 before step 5: a `Create` is a duplicate when the log holds any checkpoint
record or creation record of its UUID, before or after it, or a manifest with its UUID exists at
the path; the leader refuses to propose one for an index it holds (the retry gets its verdict with
no entry), a duplicate that reaches the log applies as a rejected verdict, and replay never takes
it as the creation record: the replay rule reads "with any checkpoint record of the index in the
log, open at the newest one's generation; a creation record counts only when no checkpoint record
of its UUID exists". T16 gains "a duplicate `Create` after a checkpoint record: the replay opens at
the checkpoint's generation with every document, on every replica, in the same boot and after a
power loss" (`create_replay.py`'s "fixed" arm is the oracle).

#### DMS-11. D18's one-in-64 sample is the honest-fault figure, not the attacker's: the leader chooses the segment, drives the debt that skips samples, and merges the evidence away (medium)

Attacker: a compromised leader, the row the design prices "D18 detects with its sampling
probability" (`design:1148`, `1163`).

Evidence:

- D18 states the honest figure: "a single corrupt segment with probability 1/64 per follower (about
  3.1% with two followers)" (`design:1128-1129`; `d18_residual.out`: 0.031 at rf = 3, 0.061 at
  rf = 5). It does not say how a follower chooses its samples. If it is every 64th flush, or any
  rule the leader can compute, a leader that plants one corrupt segment picks an unsampled one and
  the probability is 0; the design's "systematic fault within tens of flushes" (`design:1128`)
  holds only for a fault that cannot choose.
- Under debt "it skips samples and counts them" (`design:970`, `1124`). The leader admits the
  writes, so it sets the debt; at a skip rate of 0.9 the two-follower detection falls to 0.3%, at
  1.0 to 0 (`d18_residual.out`).
- The merge check "re-runs a sampled merge over the inputs it holds and compares outputs"
  (`design:1119-1120`). The inputs are the leader's segments, so a corrupt flush that reaches a
  merge passes a correct merge: after its first merge the planted segment is undetectable, and
  with the 16 MiB cadence of D8's table a flush is merged within minutes. The window in which the
  1/64 applies is one flush's lifetime, once.
- The digest compared is F21's over the segment's documents: `D`, the length record, `P` records,
  `stored`, `V`, the slot table (`f13:1154-1158`), so a dropped posting changes it (T4's mutant,
  `design:1283`); a planted segment whose records are consistent but whose documents differ from
  the log is caught the same way. The oracle is right; the sampling is what the attacker defeats.

What they gain: a segment of their choosing, with documents the log never carried or without ones
it did, on every segment-mode follower, with probability about 1 of never being checked, outliving
their removal (`design:1114`), where the design's row prices the attempt at 3%.

Fix, in D18, the row, and T4, before the full design review: samples are chosen by each follower
from its own randomness (a per-process seed the leader never learns), so every flush is sampled
with probability 1/64 whatever the leader computes; under debt a sample is deferred, not skipped
(a bounded queue of flush identities, verified when the debt clears, since the segments persist
until merged), and a sample whose inputs were merged away before it ran is counted as `verify_lost`
so the operator sees the rate, not only `verify_skipped`; the merge check, where it runs, rebuilds
the output from the log range the inputs cover (the union of their ranges, which `Image` names
per segment, `design:712`) rather than from the leader's inputs, or the design states that merges
are checked for merge faults only and a planted flush survives its first merge; the row prices a
compromised leader at "chooses what is sampled unless the sample is the follower's secret", and T4
gains a mutant leader that corrupts one flush in 64 under sustained ingest, caught within the run.
`verify: replica` stays the answer for an index that needs an independent copy; this finding is
about the default being priced as it performs.

#### DMS-12. D17's file list has no stated size bound and its sum no overflow rule (low)

Attacker: a compromised member serving files, or the leader of a checkpoint transfer.

Evidence: the `RFTSNP03` descriptor carries "the file list's size and SHA-256, the file count, and
the total bytes" (`design:1056-1057`), and the stream's list is "at most the descriptor's list
size and checked against its SHA-256 before anything in it is decoded" (`design:1059-1060`). The
list size is the source's number; nothing caps it, so a receiver buffers whatever the descriptor
names before the hash can refuse it. Today's descriptor is bounded by `maximum_application_bytes`
(`snapshot.h:55-63`, checked in `reserve_snapshot`, `worker.h:296`), which `RFTSNP03` extends
without saying which of its three sizes that policy bounds. The reservation "of the missing files'
bytes" (`design:1064-1065`) sums 8-byte sizes the source wrote, with no "checked without overflow"
sentence, which the peer transport's D9 asks of every new decoder (`pt:666-668`).

Fix, in D17 before step 8: the list size is at most the file count × 60 plus the manifest's bound,
the file count at most the `Image` entry's (for an `Image` transfer the receiver knows both and
refuses a `Begin` that differs) or `limits.md`'s segment count for a checkpoint transfer, checked
from the `Begin` before any list byte is buffered; the sum of the sizes is computed without
overflow and must equal the descriptor's total bytes, else `Result{Rejected}`, counted; T15 gains
a `Begin` whose list size, count, or total disagrees with the committed entry, refused before the
first chunk.

#### DMS-13. The rewind floor is the voter's own report, once per a restart the voter declares (medium)

Attacker: a member holding one key, as DMS-2's.

Evidence:

- D5: "A forgetful reply lowers `matched` to no less than the highest `synced` that voter reported
  to this leader" (`design:448-449`), and "at most once per restart of that peer's node: the hello
  carries a boot identity fresh at each process start" (`design:453-455`; `pt:550`). Both are the
  voter's: a voter that reports `synced` 0 on every reply (a lie "nothing in consensus" stops,
  `design:1149`) has a floor of 0, and a voter that reconnects with a fresh boot identity has
  restarted as far as `PeerRestarted` can tell. The resend is then the retained log for the group,
  or a snapshot below its base (`design:457-458`), once per reconnect; the transport caps
  handshakes in flight (2 per source, 2 s each, `pt:209`) but not reconnects over time, about one
  a second per source by its own arithmetic ("8 sources and 8 new connections a second").
- The honest bound the design already has and does not use: D14 caps a core's unsynced
  acknowledged bytes at `max_unsynced` (64 MiB) and 4 × `sync_interval` (`design:331-334`), so an
  honest forgetful voter has lost at most that much above its last barrier, whatever it reports.

What they gain: DMS-2's gain again, metered by reconnects instead of replies: the leader's disk
and egress for the retained log of every shared group about once a second, and the laggards'
installs behind it; the row's "bounded by the voter's reported `synced`" (`design:1149`) is a
bound the voter sets.

Fix, in D5 and the row, before step 3's rewind code: the floor is the larger of the reported
`synced` and `matched` less the entries that fit `max_unsynced` (the leader's own figure, from the
entries it sent that voter), so the honest cost stays one unsynced window and a liar's is the same;
a reply below that floor is counted as `forgetful_below_synced` already is; and the once-per-restart
allowance is also at most once per `recovery_timeout` per peer, so a stream of fresh boot identities
buys one window per timeout. T6's mutant "a rewind below the voter's reported `synced`" becomes "a
rewind below the leader's floor".

### Blocking items

1. DMS-10 blocks step 5's `Create` replay (`design:1345`) and T16's row until D1 states the
   duplicate rule above; steps 1 to 4 and 6 to 8 are unaffected by it.
2. DMS-13 binds step 3's rewind code (`design:1343`) with DMS-2's condition: the floor from the
   leader's figure, in D5, before the rewind lands.
3. DMS-11 binds the full design review: D18's sampling rule, the deferral, and the merge check's
   honest statement, with the row re-priced, before Q9's default is final.
4. DMS-12 binds step 8 (`design:1348`): the list's bounds in D17 before `RFTXFR02` is encoded.

### Conditions

1. C8 (DMS-10): the duplicate-`Create` rule in D1 (refused at proposal, a rejected verdict in the
   log, never the creation record at replay; replay opens at any checkpoint record of the UUID) and
   T16's case, before step 5.
2. C9 (DMS-11): follower-secret sampling, deferral under debt with `verify_lost`, the merge check
   either from the log range or stated as merge-fault-only, the row re-priced, T4's choosing
   mutant, before the full design review.
3. C10 (DMS-12): the list size, count, and total bounds in D17, checked from `Begin` before
   buffering, the sum without overflow, T15's case, before step 8.
4. C11 (DMS-13): the floor as the larger of the report and `matched` less `max_unsynced`, the
   allowance at most once per `recovery_timeout` per peer, the row's sentence, T6's mutant
   reworded, before step 3's rewind code.
5. C12 (DMS-3's cost): one sentence in the row (`design:1149`): the forged flag loses a `memory`
   write only after `recovery_timeout` with the write's holders unreachable, never when every voter
   answers; in the next revision.

C1 to C7: closed as above; nothing of them stays open.

Not run: nothing compiled (the three probes are Python under `python3 -I` at `nice -n 19`,
`quiet check` 0); no socket crossed, so the transfer, the version negotiation, and the kind sizes
are judged from the designs' text and the consensus code at `61149a2`; the liar search stopped at
depth 9 by its state limit (5,667,532 states), one step short of the trace, which was forced
instead, so no claim is made about other liar traces or about five voters; the C++ port
(`forgetful_check.cc`) was not patched or run; the designer's exhaustive and swarm results were not
re-run (the architect's re-check does); T2 and T1 not re-measured; no Linux or FreeBSD; Detent not
read; format 13's digest code not run against a planted segment (the T4 mutant is the design's to
build).

## D18 review (revision 4)

- Date: 2026-10-19. Reviewed: `docs/design/durability-modes.md` at `b7c6c78` on
  `design/durability-modes` (revision 4), in the worktree `~/code/Kronuz/strata-wt/review-durability-sec`:
  D18 (`design:1767-2044`), D16's wire row for `Vouch` (`design:1642`), D17's `Vouch` and
  `Request` (`design:1737-1746`), D8's install and catch-up (`design:1338-1372`), D14's verifier row
  (`design:1598`), the threat model's leader and voter rows (`design:2058-2059`), the fault table
  (`design:2081`), T2 and T4 (`design:2201`, `2203`), Q9 (`design:2227-2297`), and the summary
  (`design:310-313`). Also read: the architect's re-check of revision 3 (DM-R25, DM-R31, DM-R32,
  C25) and its `r3/plant/term_index_plant.cc` with its output; `bench/durability/keyed_digest.py`
  and `records.py` with `results/r3/keyed_digest.out`; strata `main` `d058cd9`: `index.hh:644-700`
  (F21's digest), `reader.hh:60-100` (`digest_record`), `380-540` (the inverted reader's open,
  `find_term`, `scan_terms`, `term_cursor`), `1160-1300` (the postings skip levels), `1732-1736`
  (`tail_impacts`), `1893-1918` (the record emitters), `300-378` (`validate`), `895-915` (stored
  records), `keys.hh:60-185`, `format.hh:110-200` (the section table), `420-660` (term info,
  `encode_term_info`, `decode_term_info`, `competitive`), `search.hh:310-330` (`term_scorer`'s
  whole-list bound), `writer.hh:108-122` (`stats`), `docids.hh:96-106`, `270-278`;
  Kronuz/termdict at strata's pin `eaf83b3` (`builder` `:78-175`, `block` `:220-240`, `reader`
  `:455-530`); Kronuz/fusefilter `28f4241` (`filter8`, `filter8_view::parse`); REPLICATION.md's
  guarantee. The ledger's routing (`status:227-232`). Not read: any Xapian or Xapiand GPL source.
- Threat model applied: a compromised leader, and separately a compromised non-leader voter,
  trying to get a segment-mode follower to install a segment whose content or lookup structures
  differ from what the log implies, without detection; a client with valid credentials where the
  verifier's own rules give it a lever.
- Checked by: reading, arithmetic, and three probes in `.scratch/durability-sec-r4/` (kept; `quiet
  check` exited 2 throughout, a light hold, so the one build and every run went at `nice -n 19`,
  one job):
  1. `impacts_roundtrip.cc` (`impacts_roundtrip.out`; built with Homebrew clang 23.1.2 against
     `include/`, whose `PROVENANCE` names each header's commit: termdict, fst, and varint at
     strata's pins as the architect copied them, `crc32c.hh` from Kronuz/hashes `7ab0c23`,
     `format.hh` from strata `main` `831459e`): a term's dictionary value with planted whole-list
     competitive pairs and a planted `cf` decodes and re-encodes to the planted bytes, so check 5's
     byte equality holds against them while `format::competitive()` over the walked (wdf, norm)
     pairs gives the honest frontier.
  2. `vouch_position.py` (`vouch_position.out`): D17's `Vouch` rule as written, run against the
     schedule in DMS-15, under the rule's wording ("at or below the position through which the
     member's checks passed") and under a per-entry rule.
  3. `verifier_bounds.py` (`verifier_bounds.out`): the arithmetic of check 1's table and a merge
     flood against D9's replay bound and D17's window.
- Citations: `design:N` is line N of `docs/design/durability-modes.md` at `b7c6c78`; strata code
  is `main` `d058cd9` unless a commit is named; `termdict.hh:N` is Kronuz/termdict `eaf83b3`.

### Verdict

**APPROVE WITH CONDITIONS. The keyed comparison is sound for what it compares: SipHash-2-4 under a
per-process key from `getentropy`, summed modulo 2⁶⁴ − 59, over injective record encodings, gives
a leader without the key a 2⁻⁶⁴ chance per comparison and one comparison per follower per group;
sampling by (group, index, term, position) under a second key is a fresh draw the leader cannot
steer or learn ahead; the verifier's work is bounded by D9's replay bound and D17's window, not by
the leader; and a compromised voter gains nothing from a false `yes` without a compromised leader
and only a bounded re-fetch from a false `no`. Check 5 closes DM-R31 for the dictionaries, the key
range and filter, norms, the statistics, df, and the skip levels. Two findings keep it from
"detected at its first install" as the threat row and Q9 state it: a term's whole-list competitive
pairs, stored in its dictionary value for df ≥ 128 and read by `term_scorer` as the term's score
upper bound, survive check 5's byte-for-byte re-encoding (the re-encoding is of the decoded value,
so it verifies canonical encoding, not the pairs; `impacts_roundtrip.out`), which lets a leader
lower a term's bound and have MaxScore prune documents a scan sees (DMS-16, medium); and `Vouch`
answers by position, so a follower that skipped an `Image` through D8's catch-up and passed a
later one answers `yes` for an entry it never compared against the log, which the leader can
arrange with the ingest flood the design already names, so a planted whole image on a new member
is vouched for instead of detected (DMS-15, medium; `vouch_position.out`). Two low findings on
`Vouch`'s wire and liveness rules (DMS-17) and the key's hygiene in D18 (DMS-18), and one on check
1's dependence on F21's layout independence as an availability lever for an honest client (DMS-19).
Q9's recommendation stands, with its residual restated (below).**

### The five attacks, as checked

1. **The keyed sums.** Key leakage: SipHash-2-4 has no key-dependent branch or address, and the
   only outputs that leave a comparison are one bit, pass or fail, per follower per group
   (`design:1788-1793`); a leader that writes a one-record segment and sees the verdict learns
   whether SipHash_k(r) summed with its guess matched, nothing of k. The sum's `% p` is a division
   over PRF outputs, not over the key. Logs: D18 does not say whether a mismatch logs the two sums;
   leaking them is harmless to SipHash as a PRF but there is no reason to send 64 bits of keyed
   output to a log a leader may read: DMS-18 asks for the sentence. Crash dumps and swap: the key
   dies with the process (`design:1916-1917`), so a dump of a dead process is useless; a dump or a
   swapped page of a live one is the local-file row, outside the model, but costs one `mlock` and
   one `madvise` to close (DMS-18). `getentropy` failing must refuse to start the verifier, not
   seed from the clock (DMS-18). Multiset and ordering: the sum is over a multiset, so a leader may
   reorder documents within a segment or across segments of one `Image` freely, which changes no
   query result (ordinals are not in the records; docids are); the prime modulus keeps every
   multiplicity (`design:1789-1791`, the architect's check); a record moved between a merge's inputs
   and its output is check 3's multiset either way. Decode differentials: the walk's `P` records
   come from `postings_iterator::next()` and `positions_reader`, the same decoders `advance()` and
   the scorers use for a block's contents; the skip levels, which `advance()` alone reads, are what
   check 5 names (`design:1896-1900`), both levels (level 0's `last`, offsets, pairs,
   `reader.hh:1243-1258`; level 1's `group_last` and pair offsets, `:1281-1285`), which the text
   should say in those words. `D` records are built from the keys walk (`index.hh:655-659`), so a
   keys dictionary that maps a key to another ordinal changes a `D` record and fails check 1. A
   term with df 1 holds its posting inline in the value (`format.hh:598-600`); the walk's `P`
   record covers it. What the walk does not reproduce from the value: `cf` (nothing in `scorer.hh`,
   `search.hh`, or `exact.hh` reads it; a merge recomputes it, `merge.hh:148`) and the whole-list
   pairs (DMS-16).
2. **Check 5's coverage.** Confirmed against the readers: both dictionaries re-encoded from the
   cursor (`termdict.hh` `reader::cursor` from offset 0, `:500-516`), the builder a function of its
   entries and alphabet (`:95-131`); the key range and filter probed per walked key (false
   negatives are the only way a filter hides a key, `keys.hh:165-168`); norms against lengths;
   the `term_index` head against the walk; df against decoded postings; skip entries and block
   pairs. `stats` is the document count (`writer.hh:118-120`), which the bitmap length and the walk
   fix; `docids` must increase or the reader refuses it at open (`docids.hh:96-106`, `270-278`);
   `versions` ride the `D` records. The alphabet: check 5 re-encodes "under the alphabet the
   received index names" (`design:1875`); an honest writer derives it from the bytes its keys use
   (`writer.hh:241-243`, `keys.hh:84-91`, `merge.hh:92-99`), so a received superset alphabet
   re-encodes equal to itself and not to an honest writer's bytes. No query reads the alphabet
   except to code keys, so nothing is gained; the design should say "the alphabet the writer
   derives from the walked keys", which also makes the check a function of the walk alone (a
   precision item under C14). The gap: the term-level pairs, DMS-16. `cf` is unread today; the
   design should say so and add it to the recomputed set when a scorer reads it.
3. **Whole images and `Vouch`.** A false `yes` from a compromised voter vouches for what an honest
   leader built: nothing gained without a second compromised member, outside the model. A false
   `no` costs the receiver a re-fetch "from that member" and an error log, at most one answer per
   (group, receiver) per `recovery_timeout` (`design:1642`, `1741`): bounded, but routed to the liar
   (DMS-17). Replay: an answer is one byte with no entry in it (`design:1642`); within a session the
   receiver has at most one ask outstanding per member, so a stale answer can only be mistaken for
   the next ask's after a timeout, which an echoed (index, term) closes (DMS-17). Forcing
   `install_unvouched`: the leader cannot make members unreachable, but it does not need to: a
   member answers `not yet` while its checks have not reached the entry, and `yes` once a later
   entry passed, so the flood that pushes honest followers into D8's catch-up (`design:1366-1372`,
   `1940`) turns the whole-image residual into a vouched install: DMS-15. The residual as stated
   ("detected when a member answers no, and never if no member ever answers", `design:1979-1982`)
   lacks the case "a member answers yes for an entry it never checked".
4. **Resource exhaustion.** Check 1's table is 16 bytes per document of the range
   (`design:1917-1923`); the range is the leader's to name, but a follower whose unchecked installs
   pass D9's replay bound takes the whole-image comparison, two sums and no table
   (`design:1937-1941`, `1959-1962`), so the table is at most 16 × the replay bound, 1.6 MB at the
   no-stalls estimate and 0.1 to 0.4 MB at T2's costs, where the retained share would have allowed
   233 MB (`verifier_bounds.out`). A merge flood (the whole index merged every refresh) makes each
   follower walk the index per merge, about 0.9 s of one core per GiB at the assumed decode, paced
   by the bytes D17's window delivers and symmetric with the leader's own merge and shipping cost: no
   amplification. The dictionary walk cannot loop: a block's next offset is its own end
   (`termdict.hh:220-240`, `next_block()`), and a zero-entry block is refused (`:229`). The filter
   probe and the re-encoding are linear in the walk; the index's builder holds one key per block.
   Check 3 holds the inputs' bitmaps it already holds. Nothing here allocates on the leader's
   numbers before a bound.
5. **Q9 as a security control.** Below, after the findings.

### Findings

#### DMS-15. `Vouch` answers by position, so a follower that skipped an `Image` through catch-up vouches for content it never compared against the log (medium)

Attacker: a compromised leader, with a new member or a repair in its group.

Evidence:

- D17: a member answers `yes` when the entry "is at or below the position through which the
  member's own install checks passed" (`design:1738-1739`). D8: a follower whose install lag
  passes D9's bound applies the tail itself "until an installed `Image` covers its applied
  position" (`design:1366-1369`), skipping the incremental installs between; D18: that reinstall is
  checked whole against the follower's own image (`design:1959-1962`), so its checks pass through
  the later entry E′ without a walk of any segment the skipped entry E named. The design names the
  lever: "a leader that floods ingest buys a follower document-mode work" (`design:1940`).
- The schedule (`vouch_position.out`): the flood puts F1 and F2 in catch-up; the leader flushes S
  with a document X under a key K the log never carried and commits E naming S; F1 and F2 skip E;
  the new member N installs E whole from the leader (only the leader holds S), passes check 5, asks
  F1: `not yet`, counts `install_unvouched`, and serves X; the leader appends `Delete(K)`, which
  N's check 2 agrees with (X's `D` record names K, so N expects X's ordinal deleted) and F1 and F2
  apply as not found; the leader merges S into M dropping the dead X, which N's check 3 passes
  (live inputs equal the output); E′ names M; F1 and F2 reinstall E′ whole against their own
  images, equal, checks passed through E′; N asks again: E ≤ E′, `yes`. N's warning clears. No
  member ever compared S with the log. Hiding works the same way: S omits a document the log
  holds, a later client-visible re-index restores it before E′.
- Under a per-entry rule (`yes` only for an entry the member installed with its checks or compared
  whole itself) the same schedule answers `not verified` and N stays counted (`vouch_position.out`,
  second run).

What they gain: a document the log never carried in one replica's query results, or one it did
carry missing from them, for a window the leader chooses, on a new member or a repaired one, with
the design's residual counter cleared by an honest member's `yes`. Bounded to one replica and
loud only while unvouched.

Fix, in D17's `Vouch`, D18's whole images, the threat row, the fault table, and T4, before step 5's
verifier: a member answers `yes` only for an entry it installed with checks 1 to 5 passing at that
entry, or compared whole against its own image at that entry; `not yet` while its checks have not
reached the entry; and a third answer, `skipped` (or `not verified`), for an entry below its
checked position that it never checked, which the receiver treats as `not yet` and keeps asking
every member for. A receiver that gets no `yes` within k × `recovery_timeout` (k a Detent knob)
re-fetches from a member that is not the leader if one now holds the entry, else stays counted.
T4 gains the schedule above as a case: the planted flush on a new member while both followers
catch up, the delete and the merge, then E′; the new member stays `install_unvouched` under the
fixed rule and is vouched for under the written one (the mutant). The residual sentence gains "a
member vouches only for an entry it checked itself".

#### DMS-16. A term's whole-list competitive pairs, read as its score upper bound, survive check 5's byte-for-byte re-encoding (medium)

Attacker: a compromised leader.

Evidence:

- A term with skip blocks (df ≥ 128) stores its whole-list competitive pairs in its dictionary
  value (`format.hh:442-454`, `encode_term_info` at `:595`); `term_scorer` takes them as the
  term's `max_score_total` (`search.hh:316-317`, `329`), which MaxScore uses to order terms
  (`search.hh:707`), to bound a disjunction (`:458`), and for the tail block's pruning
  (`reader.hh:1248-1251`, `search.hh:392`, `422`).
- Check 5 re-encodes each dictionary "from the cursor's walk" and compares bytes
  (`design:1874-1876`); the walk yields the value's bytes, so the re-encoding is of the decoded
  value, and a planted value re-encodes to itself: `impacts_roundtrip.out` shows a value with df
  200, pairs lowered to (wdf 1, length 20) and `cf` 999999 decoding and re-encoding to the same
  bytes, df passing the postings count, while `competitive()` over the walked pairs gives
  (wdf 7, length 20). Check 5's pairs rule names skip entries' pairs and "a term without [skip
  blocks], its pairs computed from its postings already" (`design:1896-1900`), which is the
  tail-impacts path (`reader.hh:1732-1736`); the whole-list pairs of a term with skip blocks are
  in neither.
- `cf` is in the same value and unread by any scorer today (`scorer.hh`, `search.hh`,
  `exact.hh`; a merge recomputes it, `merge.hh:148`), so a planted `cf` is a latent gap, not a
  present one.

What they gain: a term's bound set below its true best score makes a top-k query under MaxScore
skip that term's documents while a scan sees them, on every segment-mode follower, with every
check passing: the DM-R31 class of fault (hidden from lookups, present in the walk), through a
structure check 5 does not recompute.

Fix, in D18's check 5 before step 5's verifier: "the whole-list competitive pairs of every term
with skip blocks equal `competitive()` over the (wdf, norm) pairs the walk decoded for it, and
`cf` equals the sum of its walked wdf", stated beside the skip-entry rule; T4's planted-structure
mutant gains "a term's whole-list pairs lowered". Precision in the same place: the skip rule names
both levels (level 0's last ordinal, offsets, and pairs; level 1's group last and pair offsets),
and the dictionaries' alphabet is the one the writer derives from the walked keys, not the received
index's.

#### DMS-17. `Vouch`'s wire and liveness rules: an answer carries no entry, a `no` routes the re-fetch to its sender, and a follower's `Request` has no second source (low)

Attacker: a compromised non-leader voter; a compromised leader for the third item.

Evidence: a `Vouch` answer "is one byte, yes, no, or not yet" (`design:1642`) where the ask carries
the entry's index and term; the receiver "asks again each `recovery_timeout`" (`design:1977`), so
after a timeout a late answer to the earlier ask is the next ask's by session order. On a `no`
"the receiver then fetches the image again from that member" (`design:1980-1981`): a lying voter's
`no` sends the re-fetch to the liar, which may refuse (`Request` refuses an entry "not committed in
its own log", `design:1744`) or stall, with no rule for what the receiver does then. D8's
follower "asks a member for the files it lacks" (`design:1338`) and D17's uses say "the leader's
push, as today" (`design:1755`); nothing says when a follower whose `Request` the leader does not
serve asks another member that holds the entry, which is the starvation DMS-15's schedule can also
use instead of the flood. And "it serves no whole image to another receiver meanwhile"
(`design:1977-1978`) leaves whether an unvouched member serves single files of that image.

What they gain: a liar's `no` costs the receiver one error log and one re-fetch per
`recovery_timeout`, bounded; a stale answer mis-attributed after a timeout is a one-byte race
whose worst case is a `yes` for the wrong entry; a leader that is a follower's only file source
chooses when that follower's checks reach an entry.

Fix, in D16 and D17 before step 8 encodes `RFTXFR02`'s controls: the answer echoes the entry's
index and term (17 bytes), refused when they differ from the outstanding ask; on a `no` the
receiver re-fetches from any member that is not the leader and not the `no`'s sender when one holds
the entry, and counts a sender that answers `no` and then refuses to serve; a follower whose
`Request` is unserved for one `recovery_timeout` asks another member that holds the entry; an
unvouched member serves no file of its unvouched image.

#### DMS-18. The key's hygiene is argued, not stated: dumps, swap, `getentropy` failure, and the sums in logs (low)

Attacker: local access on the node (outside the model, the local-file row) for the first two; a
compromised leader reading logs for the last.

Evidence: "never sends, and never stores" (`design:1787`) and "the key dies with the process"
(`design:1916-1917`) hold for the design's own writes; a core dump, a diagnostic dump of a live
process, or a page of the key swapped out (this host's swap was nearly full while the architect
ran, `durability-modes-design.md:1477`) stores it outside them. Nothing says what a follower does
when `getentropy` fails, or whether a mismatch logs the sums.

What they gain: with a live process's key, a leader could forge any sum for that follower until it
restarts; without it, nothing. The sums in a log give a leader 64 bits of keyed output per
mismatch, which SipHash-2-4 as a PRF withstands, and which a one-bit verdict already leaks less of.

Fix, in D18, next revision: the two keys live in one `mlock`ed page marked `MADV_DONTDUMP` on Linux
and `MADV_NOCORE` on FreeBSD (macOS has neither; `mlock` alone), zeroed at exit; a `getentropy`
failure refuses to start the verifier and the follower applies the batches itself, logged at error
level; a mismatch logs the entry, the segment, and the check that failed, never a sum.

#### DMS-19. Check 1 inherits F21's layout independence, so any document shape where the log-built records and the segment walk differ is an availability lever for a client (low)

Attacker: a client with valid credentials; or no attacker.

Evidence: check 1 compares records built from the prepared batch (`inverted_plugin::records(docid,
part)`, `reader.hh:1908-1918`: `p.terms` then the tokens grouped by term) with records walked
from the segment (`records(reader, core, live)`, `:1893-1904`); the two agree when the writer's
output is a function of the part as F21 states, which `validate` protects (terms unique, positions
increasing, no token equal to a term, `reader.hh:319-371`). A shape where they differ (a future
plugin, a token shape `validate` admits and the writer normalizes) is not a plant: it is an honest
install that fails check 1 on every segment-mode follower at once, each of which "stops installing
that group's images and applies the batches itself" (`design:1933-1934`): the group leaves segment
mode until an operator acts, on one client's document.

What they gain: a segment-mode group's followers at document mode's cost, by one write; loud (error
logs on every follower) and reversible, but a lever that does not need the leader.

Fix, in T4 and D18, before step 5: T4's honest control runs over a corpus that exercises every part
shape the plugins admit (tokens with terms, `split_text`, `no_positions`, wdf 0, the key as a term,
empty and maximal stored values, a keyless index) with zero check failures as the pass condition;
D18 says a check-1 failure on an honest leader is a strata bug that T4's control is the gate for,
and that the follower's fallback is counted per check (`verify_failed{check}`) so an operator can
tell a plant from a shape.

### Q9's recommendation

Option 3 stands as the only option that detects a planted segment, with the residual restated:

1. What it verifies is the segment against the *log*; a compromised leader writes the log, so a
   document it alters before proposing is "what the log implies" and no option here sees it
   (REPLICATION.md's "nodes are not Byzantine" is the boundary, and the threat row should say so
   in one clause).
2. Until DMS-16's recomputation, a term's whole-list bound is a structure a leader can plant
   undetected; after it, check 5 recomputes every structure a reader consults from the walk,
   which is the right shape: the only values the verifier trusts from the leader's bytes are the
   ones it decodes the postings from, and those feed the sums.
3. Until DMS-15's per-entry vouch, a whole image on a new member or a repair is vouched for by
   position, which the leader can make true without a check; after it, the residual is as the
   design states it: counted and logged until a member that checked the entry answers.
4. The per-follower cost is the architect's (DM-R32 item 6, T2); nothing in this review changes
   it except that the recomputation in DMS-16 is one `competitive()` per term with skip blocks,
   which `tail_impacts` already pays for the others.

Option 1's "detects nothing against a forger" and option 2's 3.1% are stated as they perform; option
2's samples run check 5 (`design:1995-1999`), so they inherit DMS-16 the same way.

### Blocking items

1. DMS-15 and DMS-16 block step 5's verifier from landing (not from starting), and Q9's residual
   sentences and the threat row from being final, until D17's `Vouch` is per entry and check 5
   recomputes the whole-list pairs.
2. DMS-17 binds step 8's `RFTXFR02` controls: the answer's echo and the `no` routing before the
   kind is encoded.

### Conditions

1. C13 (DMS-15): `Vouch` answers `yes` only for an entry the member checked itself, with
   `skipped`/`not verified` as a third answer; the whole-image residual and the threat row say so;
   T4's new-member case runs the schedule in `vouch_position.py`; before step 5.
2. C14 (DMS-16): check 5 recomputes every term's whole-list pairs and `cf` from the walk, names
   both skip levels, and takes the alphabet from the walked keys; T4's mutant; before step 5.
3. C15 (DMS-17): the echoed entry in the answer, the `no` routing, the second source for an
   unserved `Request`, and the unvouched member's file service; in D16 and D17 before step 8.
4. C16 (DMS-18): the key page's `mlock`/`madvise`, `getentropy` failure fail-closed, and no sums in
   logs; in D18, next revision.
5. C17 (DMS-19): T4's honest control over every part shape, and `verify_failed{check}` per check;
   before step 5.

Of the re-check of revision 1's conditions, C9 (DMS-11) is closed by the keyed decisions, the
deferral, and the merge statement (`design:1810-1822`, `2000-2010`); C8, C10, C11, and C12 are the
architect's re-check to confirm and are not re-ruled here.

Not run: no strata, consensus, or journal build or suite (the branch changes no library code; the
one build is a 90-line probe against five headers); no network, TLS, or transfer exercised (`Vouch`
and `Request` are judged from D16 and D17's text); no fuzzing; no timing (the walk's and
`competitive()`'s costs are the architect's and T2's); SipHash's reference vectors not re-run (the
architect's and the designer's runs agree); the vouch schedule is a forced trace in a 70-line
model, not a search, so no claim about other vouch traces; the plant shown is at the value level
against `format.hh`, with no strata segment written and no query run over it (the pruning
consequence is read from `search.hh:316-329`, `392`, `422`); `cf`'s consumers were grepped, not
traced through every scorer plugin; no Linux or FreeBSD; no Xapian or Xapiand GPL source opened.

## Full design review (revision 6)

- Date: 2026-10-19. Reviewed: `docs/design/durability-modes.md` at `4d5acb8` on
  `design/durability-modes` (revision 6), in the worktree `~/code/Kronuz/strata-wt/review-durability-sec`:
  "Revision 6" and "Changes in revision 6" (`design:70-120`), "Revision 5" and "Changes in revision 5"
  (`design:121-171`), D8's catch-up and switch bullets (`design:1495-1515`), D14's and D15's rows
  (`design:1734-1738`, `1756`), D16's `Vouch` row (`design:1783`), D17's `Vouch` and `Request`
  (`design:1878-1895`), D18 whole (`design:1916-2365`: the hygiene at `1959-1970`, check 3 at
  `2024-2050`, check 5 at `2066-2159`, the counters at `2160-2173`, whole images at `2224-2300`), the
  threat rows (`design:2376-2380`), the fault table (`design:2400`), T4 (`design:2522`), Q9's
  recommendation (`design:2613-2632`), and the first steps' step 5 and step 8 rows (`design:2697`,
  `2700`). Also read: the architect's "Re-check (revision 5)" (DM-R36 to DM-R38, C30 to C32) and its
  `r5/plant/check35_real.cc` and `.out` and `r4/plant/term_index_check5.cc` and `.out` (read only);
  the ledger's record of the choice (`docs/status/durability-modes.md:274-281`, `299-300`);
  `calibration.md`'s eighteen rows; Kronuz/termdict at strata's pin `eaf83b3` (`alphabet::parse`
  `:193-212`, `builder` `:78-135`, `block::suffixes` `:271-323`, `find_from` `:380-420`, `reader`
  `:443-540`); strata at `4d5acb8` (`keys.hh:84-97`, `tuple_buffer.hh:394-405`, `writer.hh:236-246`,
  `merge.hh:92-99`); `track-c.md:523-526`, `671-673`, `712-714`; consensus `core.h:725-730` and
  `snapshot.h:45-60`. Not read: any Xapian or Xapiand GPL source.
- Threat model applied: a compromised leader choosing a whole image's term alphabet, or driving a
  mode switch; a compromised non-leader voter answering `Vouch`; a client with administrative
  credentials flipping the replication mode; the local-file row stays outside.
- Checked by: reading, arithmetic, and two probes in `.scratch/durability-sec-r6/` (kept; `quiet
  check` exited 0 at every launch, no hold):
  1. `alphabet_rules.cc` (`alphabet_rules.out`, `build.log`; Homebrew clang 23.1.2, built against
     `../durability-sec-r4/include`, whose `PROVENANCE` names each header's commit): revision 6's
     whole-image alphabet rule over 2,000 terms under six alphabets, every read path compared with
     the canonical dictionary's (find over the terms and 2,000 absent keys with bytes inside and
     outside each alphabet, 60 prefix scans, the cursor walk, and `find_run`'s visited run), seven
     spliced or malformed headers, and two degenerate dictionaries.
  2. `switch_model.py` (`switch_model.out`; Python 3.14, `-I`): DM-R37's dead-ordinal rule over
     every combination of up to three documents' write and delete positions around the command,
     the follower's pre-command merge choices and buffer state, the leader's per-document choice
     (omit, honest, altered) and a phantom, 12,984 cases at three documents, with three controls
     (the leader skips the expunge, the follower merges the segments it held, revision 5's
     "counted, not refused").
- Citations: `design:N` is line N of `docs/design/durability-modes.md` at `4d5acb8`; `termdict.hh:N`
  is Kronuz/termdict `eaf83b3`; strata files are `include/strata/` at `4d5acb8`.

### Verdict

**APPROVE WITH CONDITIONS. Revision 6 closes C13, C15, C16, and C17 as written, and C14 for every
field but the alphabet, where I rule as the lead asked: the received-alphabet rule is sound, and I
release C14's "function of the walk alone" clause for every install, not only whole images, since
the probe shows the alphabet buys a leader nothing a read can see and the exact incremental rule
buys the follower nothing but an honest-refusal path (DMS-20). DM-R37's switch rule is sound
against dead content: over every schedule the model enumerates no altered or phantom dead ordinal
passes and no expunged honest leader is refused, while revision 5's "counted" rule passes 2,932 of
them (`switch_model.out`). What the switch does open is not content but time: from the command
until the first whole install every follower's checkpoint ceiling is pinned, the leader's own log
is not, and the design names no release, so a leader that proposes a switch and expunges slowly or
never (or that stops proposing `Image`s in steady segment mode, or that floods a follower into
catch-up) grows every follower's retained log at ingest rate against a budget that is a node pool,
which is a wider blast radius than "stall its group" (DMS-22, medium). Two low findings on bounds
(check 3's table allocates on the leader's dead count, DMS-21; a mode switch costs every follower
two whole-index keyed walks per flip at no cost to a leader that flips, with no rate floor, DMS-23)
and one on precision (DMS-24). Nothing here blocks the design's approval; DMS-22 binds step 5's
verifier as C30's items do.**

### Ruling 1: the whole-image alphabet (DMS-16's remaining half, DM-R36)

What the leader controls, exactly: the index's alphabet header, which `alphabet::parse` reads at
every open and refuses unless its bytes are strictly ascending and at most 255 (`termdict.hh:197-212`;
`n = 0` means every byte, and an explicit 256 is refused). Under the rule the blocks and the FST
must then equal the builder's output for the walked entries under that header, so the header is the
leader's only free input, and the builder is a function of (entries, header): it sets each byte's
code to its rank and the code width to `bit_width(size − 1)` (`termdict.hh:82-90`), cuts blocks at
64 entries or past 1,024 bytes after 32, and indexes each block's first key by its raw bytes
(`:125-131`, `:172`). Every read decodes symbols through the same parsed header the walk used, and a
code at or past the alphabet's size throws in both (`:296`, `:317`): there is one decoder, so no
decode differential between the walk and a lookup exists. `find` and `find_run` compare decoded
symbols with the query's bytes (`:375-385`, `:397-423`); a query byte outside the alphabet simply
never matches, as it never matches in a canonical dictionary that holds no such byte. The key range
and filter are the keys dictionary's, whose rule revision 6 leaves as the walked keys' bytes
(`design:2073-2074`, `keys.hh:84-97`), so the term alphabet never reaches a key lookup.

`alphabet_rules.out`, part A: under the canonical alphabet, one unused byte above every used one,
one unused byte between used ones, NUL plus every high byte, every byte (header 0), and 254 bytes,
check 5 passes each, and `find` over 4,000 keys, 60 prefix scans, and the cursor walk answer
exactly as the canonical dictionary does. What does move is where blocks are cut once the width
grows (50 blocks at width 6, 51 at width 8), so `find_run`'s visited run differs, which is what an
honest writer under that alphabet would also produce and what the verifier's own re-encoding
reproduces; it matters only for DMS-24. The cost of a widened alphabet is the suffix bits' width,
1.03× the blocks here, at most 8 ÷ the canonical width, and it persists through later merges
(`merge.hh:92-99`), which the design states (`design:2088-2089`). Part B: a canonical block set
under an every-byte header, a narrowed header, an unordered one, a repeated one, and an explicit
256 are each refused, four of them by `parse` or the block decoder before the comparison; a header
with a same-width byte spliced between used ones shifts every code and fails on the index. Two
spliced headers pass, and should: a same-width byte above every used one leaves every code and
every block byte unchanged, so the result is byte for byte what an honest writer given that
alphabet writes. Part C: a one-byte alphabet (width 1) and an empty dictionary pass as argued.

So a leader gains nothing from the extra bytes on any lookup path, key filter, prefix scan, or
decode, and the "function of the walk alone" property was defense in depth against a differential
that the one-decoder structure rules out. Between the two rules the lead offered, I require one rule
everywhere (DMS-20): the exact incremental rule refuses only alphabets that read the same, and its
cost is an honest-refusal path coupled to `merge.hh:92-99`'s exact policy and to an induction over
the inputs the follower happens to hold, which the architect's own probe showed fails under one
reading for a merge of a merge (`check35_real.out`, M2), and under which the same bytes pass on a
new member and fail on a follower that installs them incrementally. That is DMS-19's class of lever
with no security on the other side of the scale.

### Ruling 2: the mode switch (DM-R37)

The rule as written (`design:2238-2248`, D8 `design:1507-1515`): from the command the follower
merges none of the segments it then holds and keeps its log from the command; the leader's first
`Image` after the command carries no dead ordinal of a document deleted before it; every dead
ordinal must name a document dead at the `Image`'s position, at most once, with records equal to
the follower's own image's or to those built from its log while the log holds the write, else the
comparison fails. The model's follower holds a document's records when a held segment still
carries it (live, or dead by a post-command delete, or dead by a pre-command delete its own merges
carried), when a post-command flush holds it (a document deleted while buffered is dropped, as
`flush_active` drops it), or when its log holds the write (every write above its checkpoint, which
every document still buffered at the command is, and every write after the command). Over 96, 1,296,
and 12,984 cases: no dead ordinal with altered records and no phantom passes; no expunged honest
leader is refused; a leader that skips the expunge is refused exactly when it carries a pre-command
delete the follower's merges dropped (218 of 1,498 honest-record cases at three documents) and
passes when the follower also carried it, which is honest content the log wrote; a follower that
merges its held segments after the command refuses 158 of 906 honest switches, T4's mutant; and
revision 5's rule passes 2,932 planted ordinals (`switch_model.out`). The residual that remains is
check 3's: which post-command deletes the leader's output carries, each a document the log wrote,
never above the follower's own statistics. So no unchecked dead content passes the switch.

The denial-of-service half is where the switch is open, and it is not the expunge. The expunge is
the leader's own cost, triggered by an administrative command it (or an administrator) issues, and
skipping it is loud and gains nothing. What the switch adds is a window in which every follower's
checkpoint ceiling sits at the command "until its first whole install" (`design:1510-1511`) while
the leader's own retention is unpinned: DMS-22. And the switch itself is a lever with no floor:
DMS-23.

### Ruling 3: C13 to C17 at `4d5acb8`

| Condition | Status | Where |
|---|---|---|
| C13 (DMS-15) `Vouch` per entry | **Closed** | D17 `design:1878-1886`; D18 `design:2256-2283` (`yes` 2261, `not yet` 2267, `skipped` 2270, `no` 2273, the echo 2276); the residual as asked, "a member vouches only for an entry it checked itself" `design:2291-2293`; the threat row `design:2376-2377`; T4 runs `vouch_position.py`'s schedule with the position rule as the mutant `design:2522`; the architect's `vouch_position_r5.out` answers `skipped` |
| C14 (DMS-16) rebuilt values, both levels, alphabet | **Closed**, the alphabet under DMS-20's ruling | Values rebuilt from the walk `design:2112-2135` (`cf` at 2131-2133); both skip levels with their fields `design:2136-2147`; the alphabet `design:2070-2094`; T4's plants `design:2522`; `check35_real.out` part A (0 of 20 values differ on strata's own segments). The "from the walked keys" clause I asked for is withdrawn by DMS-20 |
| C15 (DMS-17) wire and liveness | **Closed** in the design; binds step 8 | The echo, the byte's range, the drop counted, one answer per (group, receiver) per `recovery_timeout` `design:1783`; a `no` never routes to its sender `design:1885-1886`, `2276-2283`; an unvouched member refuses every `Request` `design:1891-1892`; an unserved `Request` asks another member, `request_unserved` `design:1892-1895`; step 8 carries them `design:2700` |
| C16 (DMS-18) key hygiene | **Closed** | `design:1959-1970`: one `mlock`ed page, `MADV_DONTDUMP`/`MADV_NOCORE`, zeroed at exit; `getentropy` failure refuses the verifier, never the clock; `mlock` failure logged and counted; no sum logged |
| C17 (DMS-19) honest control, per-check counters | **Closed** | `verify_failed{check}` with the whole-image series `design:2160-2173`; T4's honest control over every part shape, as whole images and through a switch `design:2522`; the client row `design:2380` |

Nothing else in revisions 5 and 6 changes DMS-1 to DMS-14's rulings. Revision 5's check 3 adds one
allocation on a leader number (DMS-21); revision 6's switch adds the pinned window (DMS-22).

### Findings

#### DMS-20. One alphabet rule everywhere: the received alphabet, ascending, containing the walked bytes, re-encoded byte for byte (low; a ruling)

Attacker: none for security (ruling 1); for availability, an honest writer whose alphabet policy
differs from `merge.hh:92-99`'s exact union.

Evidence: ruling 1 and `alphabet_rules.out`. The incremental rule (`design:2074-2081`) refuses
exactly the alphabets that read the same; the architect's M1 and M2 show how narrowly the honest
policy has to be matched (`check35_real.out`, part A); the design already says a change to the
re-encoding's rules ships with a format version (`design:2098-2102`), which this rule would widen
to the alphabet policy as well.

What they gain: nothing; what the follower loses with one rule: nothing a read can see.

Fix, in D18's check 5 and T4, next revision, text only: one rule for every term dictionary, flush,
merge, or whole image: the received alphabet, which `parse` already requires ascending and at most
255 (`termdict.hh:197-212`, worth citing so the "strictly ascending" check is seen to be free),
containing every walked term's byte, with the re-encoding under it equal byte for byte. Drop
"never the one the received index names" and the stored-alphabet union; keep the keys' rule as it
is. T4's "widened or narrowed alphabet in an incremental install fails" becomes "a narrowed one
fails, a widened one passes and reads the same". Security C14's "from the walked keys" clause is
withdrawn.

#### DMS-21. Check 3's dead-ordinal table allocates on the leader's output count before any bound (low)

Attacker: a compromised leader.

Evidence: check 3 walks the output first, "keeping each dead ordinal's docid in a flat table with
slots keyed by its PRF (8 bytes per carried document, released at the check's end)", then walks
the inputs (`design:2033-2036`, `2184-2186`). The output's ordinal count is the leader's number; the
rule that bounds it (every output ordinal names an input document at most once) is applied during
the inputs' walk, after the table is filled. Check 1's table is bounded by the log range
(`design:2176-2183`); this one is not. The transfer bounds the bytes on disk through the storage
permit, not the table in memory, and each dead ordinal costs the leader a few bytes of `docids`
and keys against 8 bytes of the follower's memory.

What they gain: a follower's memory in proportion to a segment it shipped, about 2× the bytes, up
to the permit; one process per follower, repeatable.

Fix, in D18's check 3 before step 5: before the output's walk, its ordinal count (the `stats`
document count, which check 5 verifies against the walk) must be at most the sum of its inputs'
ordinal counts, which the follower already verified at their installs; a larger output fails check 3
without a walk, and the table is at most 8 bytes × that sum. State the same bound for the
whole-image comparison's dead ordinals (`design:2231-2235`) against the follower's own ordinal
count.

#### DMS-22. A pinned checkpoint ceiling has no release, and the retained-log budget it grows against is a node pool (medium)

Attacker: a compromised leader; or no attacker (an honest leader's long expunge, or a leader
change during a switch, which restarts it).

Evidence:

- Three paths pin a segment-mode follower's checkpoint ceiling: catch-up, "until an installed
  `Image` covers its applied position" (`design:1495-1506`); the switch, "until its first whole
  install" (`design:1510-1511`); and steady segment mode, where a follower "checkpoints only images
  whose checks passed" (`design:2204-2206`), so a leader that proposes no `Image` pins every
  follower at its last checked image. In each, the follower acknowledges every delivery, so from
  consensus's view it is not lagging and D13's forced checkpoint for a slow follower never fires
  (`track-c.md:671-673`); the retained-log refusal is the leader's own `charged_bytes()`
  (`core.h:725-730`), and the leader's retention is unpinned since its own state is the image.
- Track C's reclamation "forces its checkpoint when its retained log passes its share"
  (`track-c.md:523-526`, `712-714`), which D8's ceiling bars. The design does not say which wins. If
  the ceiling wins, the group's pinned segments accumulate past its share toward the node budget
  (16 GiB of retained bytes, 8 Mi entries, "node pools"), and every group on that node shares the
  consequence; if reclamation wins, the log range the whole comparison needs is truncated, which is
  T4's own mutant for an honest reinstall (`design:2522`, "a catch-up follower whose checkpoint
  ceiling is lifted so its log range is truncated").
- The switch window also holds two images on every follower at once: its own, which it keeps
  applying into, and the leader's whole image, reserved from the storage permit before the first
  chunk (`design:1859-1861`); a refused reservation is retried "only after space frees", with the
  ceiling still pinned.

What they gain: with one log entry (the switch, a replicated setting a leader writes to its own
log) and ordinary ingest, every follower's retained log grows at the ingest rate with no bound
the design names, on a budget shared by every group on the node; the honest trigger is a large
index's expunge, one rewrite of every segment holding a dead ordinal, during which the same holds.
"A compromised leader can already stall its group" (`design:2377`) covers the group, not the node.

Fix, in D8, D14's verification row, D15's switch row, and D18, before step 5's verifier: state the
release. A segment-mode follower whose retained log passes its share while its ceiling is pinned,
whatever pinned it, leaves segment mode for that group as it does after a failed check: it applies
the batches itself from its installed position, checkpoints its own state, counts it
(`segment_mode_left{reason}`: `no_image`, `switch`, `catch_up`), and logs it at error level; the
switch completes for it at a later `Image` through the same whole comparison. Say that this release,
not Track C's forced checkpoint, is what runs at the share for a pinned group, so a comparison's log
range is never truncated under it. T4 gains the case: a leader that proposes a switch and never
writes the `Image` under sustained ingest, with every follower's retained log within its share at
the end and the group still serving; T14 reports the retained log per group across a switch.

#### DMS-23. A mode switch costs every follower two whole-index keyed walks and a whole install per flip, costs a leader that flips nothing, and has no rate floor (low)

Attacker: a compromised leader; a client with administrative credentials.

Evidence: the replication mode is a replicated index setting (`design:1756`), applied at a log
position; a switch to segment mode ends in a whole comparison, the keyed sum of every live record
of both images plus check 5 on every segment (`design:2231-2237`), and a switch back is a replay
from the installed position (`design:1756`). A leader whose segments hold no dead ordinal has no
expunge to pay; the follower holds the leader's files from the previous segment-mode period, so
little is shipped; the follower's walk is the whole index each time. D15 marks the switch heavy
and names no minimum interval; `recovery_timeout` bounds `Vouch` answers but not switches.

What they gain: a compromised leader, two log entries per cycle against two keyed walks of the
whole index on every follower (at the design's 2.0 to 5.3 µs per record pass, tens of seconds of
one core per million documents per flip) plus a replay; within the "can already stall its group"
residual, but asymmetric where the flood lever is symmetric. An administrator can do the same with
the leader's expunge as its own cost.

Fix, in D15, before step 5: a floor on switches per index (a Detent knob, one per
`switch_min_interval`, default on the order of minutes), a switch within it refused naming the
floor; `mode_switches{index}` counted and logged at warning level either way.

#### DMS-24. Precision: the restart flags under the alphabet in force, and the merge that straddles the command (low)

Attacker: none; honest refusals.

Evidence: (1) check 5 rebuilds each term's value and says its `absolute` and restart flags "follow
from those values" (`design:2126-2131`), and the architect read them as following the builder's
`next_is_restart()` (its revision-5 re-check); the probe shows the alphabet's width moves the block
cuts (50 to 51 blocks, `alphabet_rules.out` part A), so the restart points, and with them the
flags, are a function of the alphabet in force for that segment, not of the entries alone. The
verifier's re-encoding builder under the received alphabet gives the right answer; the text should
say so. (2) D8's switch says "a merge that runs after the command carries only the documents
deleted while it runs, which were live at the command" (`design:1513-1515`); a merge that started
before the command and finishes after it carries pre-command deletes, so the expunge must cover
every output that holds a dead ordinal of a document deleted before the command, in-flight
merges' outputs included, or wait for them. An honest leader that misses one is refused by every
follower (the model's skip-expunge control), a loud honest refusal.

Fix, text only, next revision: in check 5, "the restart and `absolute` flags are those the
re-encoding builder assigns under the segment's alphabet"; in D8, "every output that holds such a
dead ordinal when the `Image` is written, a merge that straddled the command included".

### Blocking items

None blocks the design's approval or step 5's start. For step 5's verifier landing, beside C30's
items: DMS-22's release rule (a pinned ceiling at the share) and DMS-21's bound, since both are
limits that must be load-bearing before segment-mode followers run in production. DMS-20's one rule
is text before step 5 writes the alphabet check. DMS-23's floor lands with D15's knobs.

### Conditions

1. C18 (DMS-20): one alphabet rule everywhere in D18's check 5 and T4, text only, next revision;
   the "from the walked keys" clause of security C14 is withdrawn.
2. C19 (DMS-21): check 3's and the whole comparison's ordinal-count bound before the table, in D18
   before step 5.
3. C20 (DMS-22): the pinned ceiling's release at the share, which path wins over Track C's forced
   checkpoint, the counter, T4's never-written `Image` case, and T14's reported retention across a
   switch; in D8, D14, D15, and D18 before step 5's verifier lands.
4. C21 (DMS-23): a switch rate floor and `mode_switches` in D15, before step 5.
5. C22 (DMS-24): the two sentences, next revision.

C13, C15, C16, and C17 are closed as the table above states; C15 still binds step 8's encoding.
C14 is closed under C18.

Not run: no strata, consensus, or journal build or suite (the branch changes no library code; the
one build is a 300-line probe against four headers at strata's pins); no model of the pinned
ceiling or the retained-log budget (DMS-22 is argued from D8, D14, D18, `track-c.md`, and
`core.h:725-730`, not simulated); no measurement of a whole comparison, a switch, or an expunge
(DMS-23's cost is the design's own per-record figure times the record count); the switch model is
exhaustive to three documents with one follower and no leader change, so it says nothing about a
leader change mid-switch or about live-record sums (it models dead ordinals only); the alphabet
probe runs termdict's reader and builder, not strata's segment reader, so the rebuilt term values'
restart flags under a widened alphabet (DMS-24 item 1) are argued from `termdict.hh:141`, not run;
no fuzzing; no Linux or FreeBSD; no Xapian or Xapiand GPL source opened.

## Re-check (revision 8)

- Date: 2026-10-19 (runs 02:00 to 02:40 UTC on 2026-10-20). The security re-check the architect's
  C47 asked for, scoped to DM-R40's rules with DM-R48's fix, DM-R41 with DM-R49's and DM-R50's,
  and security C18 to C22 as written; it decides whether `Vouch` answering and the `verify` switch
  may land. Reviewed: `design/durability-modes` at `a73f6a3` (`git log ca70579..a73f6a3`: the
  architect's re-check of revision 7 at `5ee34e4`, the models at `52a4517`, the design at
  `d341d60`, the ledger at `a73f6a3`), in `~/code/Kronuz/strata-wt/review-durability-sec`.
- Read: the architect's "Re-check (revision 7)" (DM-R48 to DM-R55, C41 to C47) and this file's
  full design review of revision 6; "Revision 8" and its changes table (`design:92-138`); D6's
  repair (`design:1345-1362`); D8's release and return (`design:1772-1827`); D15's replication-mode
  and `verify` rows (`design:2122-2123`); D16's standing attribute and `verify` history
  (`design:2180-2190`); D17's hash rule, `Vouch`, and `Request` (`design:2244-2290`); D18 whole,
  in particular the grounding rules (`design:2685-2700`), the standing (`design:2711-2720`), the
  comparison at p and its bound (`design:2721-2748`), the switch to `install` (`design:2774-2839`),
  new members and `Vouch` (`design:2840-2909`); the threat rows (`design:2985-2989`); the fault row
  (`design:3009`); Q9's options and (a) to (c) (`design:3187-3265`); the gate rows T2 and T4
  (`design:3131`, `3133`); first steps 5 and 8 (`design:3370`, `3373`); the changes to
  `peer-transport.md` and the Kronuz/sha256 row (`design:3051`, `3058`); the ledger (`ST:1-420`);
  `calibration.md`'s 19 rows. Also: `peer-transport.md` D17 (`PT:1126-1190`, read in the
  `peer-transport` worktree at `d8384c8`, whose D17 is the text the design cites at `38a6c6f`);
  format 13's index-level attribute rule (`format-13.md:1116-1123` at `main` `af37f69`);
  Kronuz/sha256 at `45b7123` whole (`sha256.h`, `sha256.cc`, `sha256_x86.cc`, `sha256_arm.cc`,
  `sha256_kernels.hh`, `CMakeLists.txt`, `test/kernels.cc`, `test/openssl.cc`,
  `fuzz/fuzz_stream.cc`, `README.md`, `.github/workflows/ci.yml`); the branch's three models and
  `results/r8/` (33 outputs). Not read: any Xapian or Xapiand GPL source; Detent.
- Threat model applied: a compromised leader (writes the log and every index setting, proposes
  `Image`s or withholds them, times every entry); a compromised non-leader voter (answers `Vouch`,
  serves files and manifests); an administrator's key in an attacker's hands; a client with
  administrative credentials; an honest follower whose clock or directory lags. The local-file row
  stays outside.
- Checked by: reading, arithmetic, and the probes below, each kept with its output in
  `.scratch/durability-sec-r8/` (`quiet check` exited 2 at every launch, light holds
  `f13g-servemem` and `f13g-am` elsewhere, never 1, `quiet.log`; every run at `nice -n 19`, at
  most four at once, each model under the branch's `run_capped.sh` at 1,800 MB, the two
  every-reachable-state runs at 2,200 MB):
  1. `runs_sec_r8.sh` and `runs/`: the branch's `vouch_model_rewind.py` and `vouch_model_cycle.py`
     at `a73f6a3`, unedited, in the 18 configurations `results/r8/` reports (the table below).
  2. `switch_consumed.py` (`switch_consumed.out`): the branch's cycle model imported unedited and
     driven along a fixed schedule, then searched from there (DMS-25).
  3. `vouch_model_cycle_deferred.py` (its diff from the branch's model in
     `vouch_model_cycle_deferred.diff`, 60 lines) and `runs-deferred/` (six runs): the cycle model
     with the grounding DMS-25 proposes, and `switch_consumed_deferred.out`.
  4. `sha256-build/` and `sha256-build.log`: Kronuz/sha256 at `45b7123` built out of tree (Apple
     clang, Release, OpenSSL 3 from Homebrew) and its three tests run; this host is x86-64 without
     SHA-NI, so only the portable kernel ran here (DMS-28 has the CI evidence for the other two).
- Citations: `design:N` is `docs/design/durability-modes.md` at `a73f6a3`; `ST:N` the ledger
  there; `PT:N` `peer-transport.md` as above; `sha256.cc:N` Kronuz/sha256 at `45b7123`.

### Verdict

**`Vouch` answering: APPROVE WITH CONDITIONS. DM-R48's fix is sound and reproduces: a standing that
belongs to the image and is taken back with it at a repair, a restart, and a restore cannot be
rolled forward by a leader, and the two mutants (the interval kept, the grounding kept) lose in 6
and 9 steps while the fix holds over 859,430 and 654,736 states. One sentence is owed before it
lands (DMS-29): the manifest a whole install ships carries the source's index-level attributes,
and nothing yet says the receiver discards the standing in it, so a compromised member serving
files could hand a new member an interval it never checked. The `verify` switch: REJECT as
written. DM-R49's three rules close the architect's cycle, and the signed statement is the right
mechanism, but two things remain. The once-only `grounded_by_switch` is spent by a switch the
leader times while no follower is "current in segment mode", which the leader arranges by
proposing no `Image` until the share releases every follower, or by flooding them into catch-up:
the group then has no grounded member in any reachable state (DMS-25, medium, `switch_consumed.out`),
which is the guarantee (a) exists to keep. And the statement's verdict depends on the follower's
clock at apply time and on its node's directory, while the design keeps the setting's
consequences (the two history bits, the admin's last sequence) as replicated state: read as
replicated, a leader-written unsigned entry sets the bits and spends or misroutes the switch
without any key; read as per replica, an honest restart or a new member replaying the entry more
than 10 minutes after its signed time refuses it and the setting diverges across the group
(DMS-26, medium). DM-R50's re-grounding is sound where its premise holds and the premise is not
stated: nothing says which voter is the document-mode member, and a group that reached `replica`
from `sample` has one whose image is unvouched, so the path never completes (DMS-27, low). The
move to Kronuz/sha256 raises no security concern (DMS-28, low: pin it by commit, as every other
library is pinned). C18 to C22 are closed at the lines below.**

### Ruling 1: the standing as an index-level attribute (DM-R48)

What a leader can reach: the standing is an index-level attribute of the follower's own checkpoint
(`design:2182-2188`), per replica under F13-R51 ("this replica's index state, which is neither
replicated nor digested; `install` keeps the target's own list", `format-13.md:1120-1123`), so no
log entry, `Image`, or install writes it; a leader cannot forge a `grounded` bit or an interval.
What a leader can roll back: nothing directly. A repair after D6's replacement takes the
checkpoint's standing (`design:1345-1362`), which is what the fix needs: the interval ends at most
at the checkpoint (`design:2185-2186`), so an entry the recovery leader replaces at a later term is
above every repaired member's interval, and the ask that echoes (index, new term) is answered
`not yet`, never `yes` (`design:2851-2858`); the grounding is the checkpoint's, so a `yes` earned for
a later clean entry does not survive a repair below it (`rw-clean-standing_kept`, 9 steps, the
mutant). A restart and D1's restore are the same rule with nothing replaced: the member re-applies
the log from the checkpoint and re-checks each install, so it can only re-earn what it had; a
failure its checks found above the checkpoint is found again from the same log. A member with no
checkpoint at or below `commit_durable` discards its image and fetches again (`design:1353-1355`),
which is `ABSENT` in the model. Every rerun reproduces the branch's (the table below). The model's
repair is at `commit_durable` exactly; the design's is at the newest checkpoint at or below it,
which only shortens the interval. A compromised non-leader voter gains nothing new: it can lie in
its own answers (DMS-15's row), not in another member's standing, except through the shipped
manifest (DMS-29).

### Ruling 2: signed `DTADMS01` statements for the `verify` switch (DM-R49)

The mechanism is right: a switch that removes checks needs an authority a leader does not hold,
and the peer transport's statement (`PT:1136-1160`, `1188`) is the one strata already verifies for
`manual`. Checked against the questions asked:

- **Replay** within a group: the admin's sequence must exceed the last the group applied
  (`design:2123`), so an applied statement is dead; across groups: the command names the group;
  of a `manual` statement as a `verify` one, or the reverse: each body is exactly its op's layout
  (`PT:1188`), so a statement signed for one op does not decode as another. A `verify: install`
  entry needs no statement and advances no sequence (`design:2123`), so it cannot be used to
  replay anything.
- **Reordering**: a leader holding two statements from one admin applies the higher sequence
  first and the lower is then refused, a no-op; applying the lower late, within its 10 minutes,
  after the admin's `install` undo, returns the group to `sample` for a window, which gains the
  leader nothing it does not have by not proposing the undo at all.
- **Clock skew**: a follower more than 10 minutes from the signed time refuses, keeps its setting,
  and counts `verify_unsigned`: conservative for `install` to `sample`, and the source of DMS-26's
  divergence for every other switch and for replay after a restart.
- **A compromised admin key**: it switches every group the admin may command away from `install`
  (one admin suffices; D17's two-admin rule covers `AddPin`, `Activate`, `Remove`, and the like,
  `PT:1158-1172`, not `verify`), at D17's rate limits (1,000 commands a minute, 256 commands a
  statement). With a compromised leader beside it, plants under `sample` are missed 96.9% of the
  time at rf = 3 and the switch back grounds nothing, so the whole-image path is lost until (c).
  That is the design's stated cost of the statement ("what it costs", `design:2123`); the threat
  model's client row should say it in these words, and whether `verify` joins the two-admin
  class is the lead's call (I would put a switch to `replica` or `sample` of more than one group
  per statement there, since it is the one statement that turns one key into an integrity
  downgrade across a cluster).
- **`grounded_by_switch` once, `sample`-only groups, and a `yes` only after checked installs**:
  each reproduces in the branch's model and in the architect's configurations (the table). The
  model lets the leader write `down` at will, which is stronger than the design's rule 1, so
  rules 2 and 3 hold even with the statement forged, which covers the compromised-admin case for
  `sample`.

What the model does not cover and the text leaves open is DMS-25 (who is "current" at the switch)
and DMS-26 (where the verdict and the bits live). The admin set a non-root node holds is named as
owed to the peer transport (`design:3051`); until it exists every switch away from `install` is a
no-op everywhere, which fails closed.

### Ruling 3: re-grounding from a document-mode member; leader-alone images (DM-R50)

The leader-alone bit is sound: an image that came whole from the leader alone carries no content
check, the switch leaves it unvouched, and it re-fetches from a member the switch grounded
(`design:2792-2797`); `cyb-switch_ground_all` loses in 4 steps without it and the fix holds over
922,928 states with two entries. The re-grounding from `replica` (`design:2800-2809`) is sound as a
rule: rule 4 grounds the fetch, the catch-up's whole comparison against the leader's image then
refuses any plant the leader holds that the document-mode member does not, and the detecting
follower keeps the fetched grounded image. Its premise, "the document-mode member is grounded by
construction (rule 1)", holds only for a member in document mode from the creation or on a
grounded image; the design never says which voter is the document-mode member, how the choice is
replicated, or what happens when that voter becomes leader (rule 4 excludes the leader), and a
group that reached `replica` from `sample` by a signed switch has a document-mode member whose
image rests on its sampled installs, unvouched by the rules at `design:2697-2702`: it serves no
file, every fetch is refused, and "stays in document mode until every segment-mode follower has
grounded" never ends (DMS-27). Nothing false is grounded on that path; the text's "no residual"
claims a completion it cannot reach. The leader-alone bit's own lifecycle (set by a whole install
from the leader alone, cleared by any grounding, `design:2713-2714`) is right.

### Ruling 4: the release mark and the deferral (DM-R53)

Sound against a compromised leader, and the bound is load-bearing. Below the mark the frozen
segments hold every ordinal the follower installed or carried, so a dead ordinal it cannot match
is a plant or an honest fault and fails (`design:1800-1803`); at or above it the follower cannot
tell its own dropped document from a plant and defers without installing, so nothing planted
reaches anyone through a deferral (`design:1803-1811`); the fourth deferral counts a failure and
stops comparing (`design:1812-1821`). Can a leader keep a follower deferring forever? It can keep
it out of segment mode for good at no cost: one dead ordinal at or above the follower's mark, in
a segment the leader never merges, defers three comparisons at least 20 minutes apart and then
ends the follower's return for that release; the follower is then in document mode with its
standing intact (`design:1787-1788`), grounded if it was, serving and vouching as before, and
returns only by rule 4 from a grounded member that is not the leader or by an operator's switch.
With rf = 3 and both followers released by the leader's silence, both can be held in document
mode: the group loses segment mode's cost advantage, never a check, which is within "can already
stall its group", and the failure line names the reason. Two notes, neither blocking: an honest
leader reaches four deferrals routinely (a large segment with one such dead ordinal that no merge
touches for 30 minutes is a normal merge policy's steady state), after which the follower's
return is a whole transfer by rule 4, which the text should state as the expected honest path
rather than only "which an honest leader reaches only when" (`design:1819-1821`); and the mark is
8 bytes "while released", so it joins DM-R54's per-group accounting only for released groups,
which is fine.

### Ruling 5: Kronuz/sha256 for file hashes (CL-10)

No security concern with the choice; DMS-28 states the one condition. What the hash buys here is
collision and preimage resistance for bytes the leader committed in an `Image` entry and a
follower reads back from any member (`design:2244-2262`): both rest on SHA-256 the function, not
on which implementation computes it, so a mixed fleet (an older binary still on EVP during a
rolling upgrade, the portable kernel on one host and SHA-NI on another) agrees as long as each
implementation is correct. That correctness is what I checked:

1. The two hardware kernels and the portable one compute the same function: `test/kernels.cc`
   forces each kernel the host has and checks the NIST vectors, every two- and three-way split of
   the short vectors, every split of 63- to 193-byte buffers at an unaligned offset, the million
   `a`s in mixed chunk sizes, and 3,000 random cases against the portable kernel's digests;
   `test/openssl.cc` checks each kernel against OpenSSL's `SHA256()` at 0 to 70,000 bytes;
   `fuzz/fuzz_stream.cc` streams any input through every kernel with mid-stream `final()` calls.
   CI at `45b7123` on `main` (run 39357682276, every job green: `linux-x86_64-gcc`,
   `linux-x86_64-clang`, `linux-arm64-gcc`, `linux-asan-ubsan`, `linux-tsan`, `freebsd-x86_64`,
   `macos-arm64`, `macos-x86_64`, and the five bench jobs) ran the kernel tests with `sha-ni`
   detected and OK on `bench-linux-x86_64`, `armv8` on `bench-linux-arm64`,
   `bench-linux-arm64-clang`, and `bench-macos-arm64`, and `portable` alone on `bench-macos-x86_64`
   (the step logs, read with `gh run view --log`). Locally (`sha256-build.log`): the portable kernel,
   NIST vectors, every split, 3,000 random cases, and the OpenSSL test, all passing.
2. Detection cannot select a kernel the CPU lacks: SHA-NI needs CPUID leaf 1's SSSE3 and SSE4.1
   bits and leaf 7's SHA bit (`sha256_x86.cc:27-45`), which are exactly the instruction sets the
   kernel uses; ARMv8 asks `getauxval` on Linux, `elf_aux_info` on FreeBSD, `sysctl` on macOS when
   the build does not assume it (`sha256_arm.cc:40-62`). A wrong positive would be a SIGILL at the
   first hash, a crash at process start, which the CI matrix exercises per platform. The test hook
   `force_kernel` refuses a kernel the CPU lacks (`sha256.cc` `kernel_fn` returns null), so even a
   knob wired to it could not select an unsupported one; strata should not wire one.
3. Streaming and padding are right for the sizes here: `absorb` buffers to 64 bytes and hands whole
   blocks to the kernel, `finish` pads to one or two blocks with the bit length big-endian, and
   the 64-bit byte counter overflows at 2^61 bytes, beyond any 32 GiB volume. `final()` is `const`
   and repeatable, which the receiver's stream-then-compare needs. Unaligned input is read with
   `memcpy` and unaligned loads; the round constants are 16-byte aligned for the aligned load the
   SHA-NI kernel makes.
4. Cost as a lever: the portable kernel's 0.24 GB/s on x86-64 without SHA-NI is the receiver's own
   cost per byte it accepted under its bulk credit (D17), so a leader cannot make a follower hash
   more than it shipped; T2 gates only on hosts with SHA instructions and reports the floor
   (`design:3131`), which is a performance matter, not a security one.
5. Side channels: the README's "not hardened" (README.md:79-81) is about secret-dependent timing,
   and file bytes every member holds are not secret; the keyed comparison stays on SipHash
   (`design:2347-2351`), which is where a key lives.
6. The pin: strata pins every library by commit in `CMakeLists.txt:10-32`; the Kronuz/sha256 row
   says "`main` `45b7123` or later" (`design:3058`) and the library's own README suggests
   `GIT_TAG main`, which a step-8 implementer could copy. Every pin is a commit hash (DMS-28).

One precision for D17: "so the leader and its followers hash with one implementation"
(`design:2248`) is not what makes them agree; SHA-256 is, which is why the EVP cross-test is the
evidence that matters and why a rolling upgrade across the two implementations is safe.

### Ruling 6: C18 to C22 at `a73f6a3`

| Condition | Status | Where |
|---|---|---|
| C18 (DMS-20) one alphabet rule | **Closed** | `design:2475-2509`: the received alphabet, parsed ascending and at most 255, containing every walked byte, the re-encoding under it equal byte for byte; revision 6's rules withdrawn with security C14's clause; T4 `design:3133` ("a widened alphabet passes and reads the same") |
| C19 (DMS-21) bounds before the tables | **Closed** | check 3 `design:2432-2438`; the whole comparison `design:2741-2748`; T4's two refusals before allocation `design:3133` |
| C20 (DMS-22) the release at the share | **Closed** | `design:1772-1788` (the three pins, the release, `segment_mode_left{reason}`, precedence over Track C's forced checkpoint); D14 `design:2104`; T4's never-written `Image` and silent leader `design:3133`; T13 and T14 as named at `design:198` |
| C21 (DMS-23) the switch floor | **Closed** | D15 `design:2122` (the leader refuses within `switch_min_interval`; each follower one whole comparison per interval by its own clock; `mode_switches`); `limits.md` row `design:3054`; T4 `design:3133` |
| C22 (DMS-24) the two sentences | **Closed** | check 5's restart flags under the segment's alphabet `design:2531-2534`; the straddling merge's output in D8 `design:1760` and D18 `design:2753` |

### Status

| Item | Status | Where |
|---|---|---|
| DM-R48 (C42) | **Holds**; one sentence owed (DMS-29) | `design:1345-1362`, `2182-2188`, `2711-2720`, `2851-2858`; the reruns |
| DM-R49 (C43) | **Holds as modeled; open as written** (DMS-25, DMS-26) | `design:2123`, `2189-2190`, `2697-2702`, `2774-2839`, `2985` |
| DM-R50 (C43) | **Holds for the leader-alone bit; open for `replica`** (DMS-27) | `design:2792-2797`, `2800-2815` |
| DM-R53 (C41) | **Holds**; one precision | `design:1789-1827` |
| CL-10 (Kronuz/sha256) | **No security finding**; pin by hash (DMS-28) | `design:2244-2262`, `3058`, `3131` |
| Security C18 to C22 | **Closed** | the table above |
| Security C1 to C17 | Hold as ruled | |

The reruns (`.scratch/durability-sec-r8/runs/`, the branch's models unedited):

| Run | Bound | States | Result | Equals `results/r8/` |
|---|---|---|---|---|
| `rw-kept-e3-d13` | 13 | 4,272 | P1 lost in 6 | yes |
| `rw-clipped-e2-d12`, `-e3-d11` | 12 / 11 | 507,095 / 970,016 | hold | yes |
| `rw-clean-interval_kept-e3-d13` | 13 | 9,104 | lost in 6 | yes |
| `rw-clean-standing_kept-e3-d13` | 13 | 398,561 | lost in 9 | yes |
| `rw-clean-fix-e2-d11`, `-e3-d9` | 11 / 9 | 859,430 / 654,736 | hold | yes |
| `cy-up_written-yes_written-e3-d13`, `-nomiss` | 13 | 4,422 / 4,418 | lost in 5 | yes |
| `cy-up_first-yes_written-e3-d13` | 13 | 25,425 | lost in 6 | yes |
| `cy-up_first-yes_checked-e3-d8`, `-e2-all` | 8 / all | 443,964 / 2,686,382 | hold | yes |
| `cyb-fix-e3-d9`, `-e4-d8`, `-e2-all` | 9 / 8 / all | 770,749 / 904,879 / 922,928 | P1, P2, P3 hold | yes |
| `cyb-switch_ground_all-e3-d13` | 13 | 893 | lost in 4 | yes |
| `cyb-up_written-e4-d9` | 9 | 382,389 | lost in 8 | yes |
| `cyb-yes_written-e3-d13` | 13 | 194,150 | lost in 8 | yes |

The variant (`runs-deferred/`, `vouch_model_cycle_deferred.py`): `--switch-ground deferred` holds
P1, P2, and P3 over every reachable state with two entries (1,096,346), to depth 9 with three
(828,817), and to depth 8 with four (933,528); without its "no switch away since" clause
(`deferred_any`) to depth 9 with three (835,565); `--switch-ground all` still loses in 4.

### Findings

#### DMS-25. The once-only `grounded_by_switch` is spent by a switch the leader times while no follower is current in segment mode, after which no member of the group is ever grounded (medium)

Attacker: a compromised leader; also no attacker (an operator's switch during a catch-up or a
release).

Evidence: `grounded_by_switch` applies once per group, at its first switch to `install`, to "each
follower whose installed image is current in segment mode at the switch's entry (not in catch-up,
and not applying the batches itself after a failed check or D8's release)" (`design:2784-2788`,
D15 `design:2123`); a later switch grounds nothing (`design:2810-2815`); and the switch to `install`
is leader-written, needing no statement (`design:2123`). Two levers the design itself describes
empty "current in segment mode" of every follower at a moment the leader chooses: propose no
`Image` until each follower's retained log passes the share, at which it leaves segment mode
(`design:1772-1788`, D8's release, a reaction to the leader's silence); or flood ingest past D9's
replay bound, which sends each follower into catch-up (`design:2663-2668`). Then write `verify:
install`. `switch_consumed.py` drives the branch's cycle model (revision 8's rules, a group born
under `sample`) along that schedule, both followers in catch-up when the `up` is appended and each
reinstalling whole after it, "equal, still unvouched", and searches from there: in 87,753 states to
depth 10 no reachable state has a grounded segment-mode member; the control with the followers
current grounds them at the `up`; with one follower out, it re-fetches from the other and is
grounded in one step. So the loss is total only when every follower is out, which the two levers
achieve at once in a group of any size, and partial otherwise, where rule 4 repairs it.

What they gain: with one log entry and some silence, a group under option 3 in which no member is
ever grounded: every new member and every repair installs from the leader alone for good, counted
`install_unvouched` and shown in the `unvouched` gauge but with no remedy short of re-indexing
(Q9's (c)), and no member serves a whole image or vouches. That is exactly what (a) exists to
prevent ("Kept unvouched, no member of such a group is ever grounded again", `design:2777-2781`).
Loud, but permanent and cheap.

Fix, in D15's `verify` row, D18's switch section, Q9's (a), and T4, before the `verify` switch
lands: the first switch's grounding is per follower and deferred for a follower that is not
current at the switch's entry: one in catch-up, or released, whose image rests on its own sampled
installs (its leader-alone bit clear) counts as grounded at its first passing whole comparison
after the switch's entry (its catch-up's or its return's reinstall, rule 3's comparison on a
sampled base), provided no switch away from `install` applied in between; its interval starts
after that comparison's entry, so it answers `skipped` for everything at or before it; the
residual is the same, what its own samples missed, since the comparison refuses every plant made
after its base. The variant above is that rule (`vouch_model_cycle_deferred.diff`): it holds over
every reachable state with two entries and to depth 9 and 8 with three and four, with or without
the "no switch away since" clause, and the `all` mutant still loses; under it the attack schedule
reaches a grounded member in one step (`switch_consumed_deferred.out`). T4 gains the schedule: a
switch to `install` while every follower is in catch-up or released under sustained ingest, each
returning grounded (`grounded_by_switch` counted at the return), with revision 8's rule, under
which no member is ever grounded, as the mutant.

#### DMS-26. The statement's verdict is per follower (its clock, its directory) while the setting's consequences are stated as replicated state, and a time rule keyed to apply time breaks replay (medium)

Attacker: a compromised leader (one reading); no attacker, an honest restart or a new member (the
other).

Evidence: the follower verifies the statement "as it applies the entry", including "the signed
time within D17's 10 minutes of the follower's own clock" and "the signer's pin against the admin
set as its node holds it", and a failed statement "is a no-op at that follower, which keeps the
setting it had" (`design:2123`); so the setting in force is per follower. But D16 keeps "the
replicated `verify` history (two bits: has run `install`, has run `replica`) with each admin's
last applied sequence for the group" (`design:2189-2190`), D15 checks the sequence against "the
last one the group applied" (`design:2123`), and D18 decides `grounded_by_switch` and the
re-grounding by those bits (`design:2782-2783`). Replicated state must be a function of the log,
and the verdict is not: the clock and the directory version differ per node.

- Read as replicated (the bits and the sequence advance when the entry applies, verified or not):
  a leader writes `verify: replica` with no statement; every follower keeps its setting and counts
  `verify_unsigned`, and the group's "has run `replica`" bit is set. Its first switch to `install`
  then takes rule 2 ("from `replica`": fetch from a document-mode member that does not exist, so
  every fetch is refused and nothing grounds) or, with "has run `install`" set by an earlier
  leader-written `install`, rule 3, which grounds nothing. The leader spends the once-only switch
  with no key at all, DMS-25's outcome by another door.
- Read as per follower: an honest follower that restarts replays the entries above its checkpoint
  (D1), a statement entry among them more than 10 minutes after its signed time, refuses it, and
  keeps the checkpoint's setting; a new member whose fetched checkpoint predates the entry does the
  same. The setting then differs across the group: for `install` to `sample` the laggard checks
  more, harmless; for `sample` to `replica` the member chosen for document mode may be the one
  that refused, and the group has no document-mode member while its history says `replica`; and
  "each admin's last applied sequence" per follower diverges, so a statement is replayable at one
  follower and dead at another. D15's own safety argument for replicated settings, "every replica
  decides by the same log position" (`design:2120`), no longer applies to `verify`, and the text
  does not say so.

PT-R25 met this question for root entries and answered it: "Replicas apply the root's state
without reading a clock, since one replica's clock is not another's. The root leader stamps each
entry ... a statement whose signed time differs from its entry's stamp by more than 10 minutes is
refused" (`PT:1144-1155`), with the stated residual that a compromised leader chooses its stamps.

What they gain: under the first reading, DMS-25's permanent loss with no admin key; under the
second, a group whose `verify` setting and history are not a function of its log.

Fix, in D15, D16, and D18, before the `verify` switch lands: (1) every consequence of a `verify`
statement is per replica, kept in the standing attribute each checkpoint writes (D16): the setting
in force, the two history bits, and the last sequence this replica applied per admin, each derived
only from statements this replica verified; drop "replicated" and "the group applied", and say
what a checkpoint without them reads as (the setting `install`, the bits as if `install` had run,
so an older checkpoint grounds nothing by a switch); (2) the signed time is checked against the
entry's leader stamp, as PT-R25 does, so a replay at any later time gives the same verdict, with
PT's residual stated (a compromised leader can hold a statement and stamp it into range later,
which gains it nothing beyond not proposing the admin's undo, and a stolen statement stays dead
once a higher sequence applied); (3) the directory edge stated: a follower whose admin set lacks
the signer refuses and keeps its setting, conservative, and re-verifies at no later time; (4) T4
gains a restart that replays a statement entry an hour after its signed time, where the follower's
setting equals the group's, with the clock rule as the mutant, and a leader-written `replica` with
no statement, after which the group's first switch to `install` still grounds the followers whose
own history is `sample` only.

#### DMS-27. The re-grounding from `replica` presumes a document-mode member every member can name and whose image is grounded, and states neither (low)

Attacker: none for safety; a compromised leader for availability (it chooses when to write the
`install` switch that starts the path).

Evidence: "`replica` ... keeps one voter in document mode" (`design:2971`), "one follower applies
the log itself" (`design:3187`); nothing says which, how every member learns it from replicated
state, or what happens when that voter is elected leader, which rule 4 excludes as a source
(`design:2692`). Rule 2 says "the document-mode member is grounded by construction (rule 1)"
(`design:2801-2802`); rule 1 grounds an image built "from the index's creation (document mode) or
on top of a grounded image" (`design:2687-2688`); a member that entered document mode at a signed
switch from `sample` to `replica` builds on its sampled image, unvouched, serves no file
(`design:2703-2704`), and the segment-mode followers' fetches are refused for good while it waits
"until every segment-mode follower has grounded" (`design:2805-2807`). Nothing false is grounded;
the path does not end, and "with no residual" (`design:2805`, `3236-3237`) is claimed for a
group that can reach `replica` without one.

What they gain: nothing; an operator gains a path that silently never completes.

Fix, text, with DMS-26's: name the document-mode member by a rule every replica computes from the
log (say, the lowest member id among the voters other than the leader at the switch's position,
re-chosen the same way when it becomes leader, and recorded with the setting); rule 2 applies only
when that member's image is grounded (rule 1 from the creation, or grounded before it entered
document mode), else the group is bucket 3 and the text says so; "until every segment-mode
follower has grounded" reads "every follower of the membership in force, a removed one excluded".

#### DMS-28. Kronuz/sha256: no security finding; the pin must be a commit, not `main` (low)

Attacker: a compromised dependency, through a floating pin.

Evidence: ruling 5. The row says "pin `main` `45b7123` or later" (`design:3058`); strata's
`CMakeLists.txt:10-32` pins every library by hash; the library's README suggests `GIT_TAG main`.

What they gain: with `GIT_TAG main`, whoever can write the library's `main` writes the hash every
member computes, and a wrong kernel on one platform fails every honest transfer there.

Fix, in the Kronuz/sha256 row and first step 8: the pin is the commit hash `45b7123`, re-pinned by
hash when it moves, never a branch; and D17's "one implementation" sentence restated as ruling 5's
last paragraph says.

#### DMS-29. A whole install ships the source's manifest with its index-level attributes, and nothing says the receiver discards the standing in it (low)

Attacker: a compromised non-leader voter serving files, with a compromised leader to profit.

Evidence: the stream carries "the application's metadata (strata's manifest)" after the file
list (`design:2215`), checked only against the descriptor's SHA-256, which the serving member
writes; the standing is an index-level attribute of the manifest (`design:2182-2188`); for an
`Image` the list must equal the committed entry's files (`design:2252`), the manifest's
attributes are bound to nothing. F13-R51 says `install` "keeps the target's own list"
(`format-13.md:1120-1121`), and D18 sets a new member's standing by rule 4 or leaves it unvouched
(`design:2840-2846`), so the intent is clear; a receiver that took the shipped list as its own
would carry the source's `grounded` bit and interval, and answer `yes` for entries it never
checked, which with a planted entry inside that interval grounds the plant at the next receiver
through rule 5. A new member has no "own list" for `install` to keep, which is where the sentence
is missing.

What they gain: a proxy vouch, DMS-15's class, from a member that never checked anything.

Fix, one sentence in D17 and D16, before `Vouch` answering lands: the index-level attribute list
in a shipped manifest is the source's and is discarded at the receiver, which writes its own
standing (unvouched with an empty interval, or grounded by rule 4 with an interval starting after
the install) at its next checkpoint; T4's "two members that each installed the leader's image
alone" case gains a source whose manifest claims a wide interval, where the receiver still answers
`skipped` for it.

### Blocking items

1. **DMS-25** and **DMS-26** block the `verify` switch: the deferred per-follower grounding, and
   the per-replica setting, bits, and sequence with the time checked against the entry's stamp;
   a design-only re-check from `vouch_model_cycle_deferred.py` and `switch_consumed.py` (the
   rechecker can run both).
2. **DMS-29** blocks `Vouch` answering until its sentence is in D17 and D16; nothing else does.

Nothing here blocks the whole-image comparison, the grounding rules, the mode switch, or the
release, which the architect approved at revision 7 and which this re-check leaves as approved.

### Conditions

1. **C23 (DMS-25)**: the first switch's grounding per follower, deferred to its first passing whole
   comparison after the switch for a follower in catch-up or released at the switch's entry, the
   leader-alone bit still excluding, the interval starting after; T4's schedule with revision 8's
   rule as the mutant. Before the `verify` switch lands.
2. **C24 (DMS-26)**: `verify`'s setting, history bits, and per-admin sequence per replica in the
   standing attribute, derived only from statements that replica verified, with what an absent
   one reads as; the signed time against the entry's leader stamp (PT-R25), PT's residual stated;
   the directory edge stated; T4's replay-after-an-hour and unsigned-`replica` cases. Before the
   `verify` switch lands.
3. **C25 (DMS-27)**: the document-mode member named by a replicated rule, rule 2 conditioned on its
   image being grounded, else bucket 3; the membership-in-force wording. Text, with C24.
4. **C26 (DMS-28)**: Kronuz/sha256 pinned by commit hash in strata's and consensus's CMake, the
   row's "or later" read as a re-pin by hash; D17's "one implementation" sentence restated. Text
   now; the pin at step 8.
5. **C27 (DMS-29)**: the shipped manifest's index-level attributes discarded at the receiver, one
   sentence in D17 and D16, and T4's wide-interval source. Before `Vouch` answering lands.
6. The threat model's client row states a compromised admin key's blast radius for `verify` in
   ruling 2's words, and the lead decides whether a multi-group switch away from `install` joins
   D17's two-admin class (a recommendation, not a condition).
7. C1 to C22: hold as ruled; C18 to C22 closed at the lines in ruling 6.

Not run: no strata, consensus, or journal build or suite (the branch changes no library code);
the architect's `r7/` probes, `seed_fp.py` and the `switch_model.py` reruns, not rerun here (the
seed rule and DM-R52 are outside C47's scope, and `results/r8/sw-*` were read only); the rewind
model to the depths above with one replacement, and the cycle model with `down` modeled as
`sample` only, so `replica`'s path (DMS-27) and the released follower's return (DM-R53) are argued
from the text, not searched; DMS-26's two readings are argued from the text, not modeled; no
statement was constructed or verified (no `DTADMS01` code exists in strata yet); Kronuz/sha256's
SHA-NI and ARMv8 kernels were not run on this host (x86-64 without SHA-NI), their agreement with
OpenSSL rests on the library's CI at `45b7123` as cited; no fuzzing; no Linux, FreeBSD, or arm64;
no Xapian or Xapiand GPL source opened.

## Re-check (revision 9, scoped)

- Date: 2026-10-20 (runs 03:00 to 03:06 UTC). Scoped to DMS-25 to DMS-29 and C23 to C27 of the
  re-check of revision 8 above, and to whether `Vouch` answering and the `verify` switch may land.
  Reviewed: `design/durability-modes` at `6425b08` (revision 9 is `4766094` models and `5460e20`
  design; `6425b08` the lead's ledger), in `~/code/Kronuz/strata-wt/review-durability-sec`. Read:
  `git diff 134eee8 6425b08` of `docs/design/durability-modes.md` whole (the "Revision 9" section
  and changes table, `design:100-155`; D15's `verify` row, `design:2196`; D16, `design:2255-2281`;
  D17, `design:2335-2372`; D18's standing and switch rules, `design:2826-2832`, `2886-2960`,
  `2994-3004`; the threat and fault rows, `design:3146-3170`; the Kronuz/sha256 row, `design:3217`;
  T4, `design:3292`; first step 8, `design:3534`), the model diff
  (`git diff 134eee8 6425b08 -- bench/durability/model/vouch_model_cycle.py`), `runs_r9.sh`,
  `switch_consumed.py` (the designer's port), and `results/r9/`. Not read: any Xapian or Xapiand GPL
  source; the architect's two notes (outside this scope).
- Reruns, kept with their output in `.scratch/durability-sec-r9/` (`runs_sec_r9.sh`, `runs/`;
  `quiet check` exited 2 at every launch, never 1, `quiet.log`; each run at `nice -n 19` under the
  branch's `run_capped.sh`, at most two at once, 800 to 2,200 MB caps, peaks 9 to 903 MB):
  1. My own `switch_consumed.py` (unedited, pointed at the tip's model): `--switch-ground eligible`
     (revision 8's rule) gives 87,753 states to depth 10 and no grounded segment-mode member; with
     `--switch-ground deferred` the attack prefix reaches "F1 reinstalls entry 2 whole: equal,
     grounded by the switch (deferred)" in one step, and with one follower out F2 is grounded the
     same way. The same counts the designer's `results/r9/sc-*.out` report (they differ only in the
     header and the "F1 grounded, F2 not" wording).
  2. My `vouch_model_cycle_deferred.py` (unedited): P1 to P3 hold over 1,096,346 states with two
     entries (530,777 with a grounded segment-mode member), 828,817 to depth 9 with three, 933,528 to
     depth 8 with four, and 835,565 to depth 9 with three without the `down` clause; `all` loses in
     4 steps (893 states). All identical to `runs-deferred/`.
  3. The designer's `runs_r9.sh` rows against the tip's `vouch_model_cycle.py`, 11 runs, each state
     count and verdict identical to `results/r9/`: `deferred` 1,096,346 / 828,817 / 933,528 hold;
     `deferred_any` 835,565 holds; `all` loses in 4 (893); `deferred_lw` loses in 5 (3,965); the
     deferral under `--up-rule written` (389,747) loses in 8 and under `--yes-rule written`
     (201,530) in 8; the regressions equal revision 8's: `eligible` 922,928 (two entries) and 770,749
     (depth 9), and `--born install` 443,964. The port's diff is the reviewer's variant plus
     `deferred_lw` and the unchanged default, so revision 8's `results/r8/` still reproduces.
  4. The test with the fix reverted: `eligible` is revision 8's rule, the mutant of item 1, and it
     spends the switch (87,753 states, none grounded); `deferred_lw` is the mutant with the
     leader-alone exclusion removed and `all` the mutant with the switch grounding everything; each
     fails at 5 and 4 steps.

### Verdicts

| Finding | Verdict | Where | Evidence |
|---|---|---|---|
| DMS-25 | **CLOSED** | `design:2196` (D15, "A replica current in segment mode ... grounded at its first passing whole comparison after the entry ... holds the owed grounding, the entry's index"), `2893-2925` (D18 item 1: owed bit, rules, forfeits), `2258-2266` (D16: the owed bit and the 8-byte index), `3292` (T4: the schedule, revision 8's rule as the mutant, `deferred_lw` as the mutant) | Reruns 1 to 4. The rule is the one the finding proposed: leader-alone bit still excludes, a `down` since forfeits, the interval starts after the comparison's entry. The residual is stated as the same as the entry's (what the follower's own samples missed before its base). |
| DMS-26 | **CLOSED**, with one new finding (DMS-30, low) | `design:2196` (D15: "The entry is replicated; its consequences are not"; setting, two bits, per-admin sequence "derived only from entries that replica verified"; "advances no bit and no sequence" on a failed or unsigned entry; the signed time "within D17's 10 minutes of the entry's stamp, never of the replica's clock", with PT-R25 cited at `peer-transport.md:1144-1155` and PT's residual stated; the admin-set edge stated: a signer outside the set is refused and the setting kept), `2258-2275` (D16: all of it in the standing attribute; absent reads as `install` with the bits as if `install` had run; the stamp (8) in the setting's command), `1427-1431` (D6: the state goes back with a repair and is re-derived from the log by each entry's stamp), `3148` (the compromised admin key's blast radius, ruling 2's figures, and the lead's call on the two-admin class left named), `3292` (T4: the hour-later restart replay with the clock rule as the mutant, the unsigned `replica` after which the first switch still grounds, the signer outside the set, the absent attribute) | Read, not modeled (revision 8 said so). Each C24 element is present. The new-member sentence in T4 is where DMS-30 starts. |
| DMS-27 | **CLOSED** (the designer's choice accepted, below) | `design:2196` and `2276-2278` (the `replica` statement's body names the document-mode member; each replica checks it is a voter of the membership in force at the entry, so the leader cannot choose it), `2926-2960` (D18 item 2: re-grounding only while the member "answers in document mode with an image that is grounded"; reasons `document_member_unvouched`, `_leads`, `_gone`, `_not_in_mode`, counted and logged at error level as `regrounding_refused{reason}`; item 3 absorbs the refusals; "every follower of the membership in force ... a removed member is excluded", `2946-2947`; the operator's order stated, `2940-2943`), `3292` (T4 cases for each reason) | By reading; the path is argued from rules 1, 3, 4 and not modeled, which the text says and puts a K7 case before the `verify` switch. |
| DMS-28 | **CLOSED** | `design:27`, `134-137`, `2336-2343` (D17), `3217` (the row: pinned `45b7123c295f8c416eb6b7a852b0b0b2c47d1286` as a `FetchContent_Declare` with the hash as `GIT_TAG`, "never a branch", the README's `GIT_TAG main` "not copied", a move is "a re-pin by hash through review"), `3534` (step 8), `3292` (T4 cross-checks the hardware, portable, and EVP digests) | `git ls-remote https://github.com/Kronuz/sha256.git` gives the 40 characters as `HEAD` and `refs/heads/main`: the hash is the real commit, in full. D17's "one implementation" sentence is restated as what ruling 5 asked ("SHA-256, not one implementation, makes the digests agree", `2339-2343`). The CMake files themselves land at step 8; strata's `CMakeLists.txt:10-32` pins every other library by hash today. |
| DMS-29 | **CLOSED** | `design:2279-2281` (D16), `2360-2371` (D17: the receiver discards the manifest's index-level attributes, keeps the target's own list on an `install` into an existing index (F13-R51), and a new member takes the absent reading), `2826-2832` and `2994-3004` (D18), `3292` (T4: a source whose manifest claims a grounded bit and a wide interval, the receiver still answering `skipped`, the receiver that keeps the shipped list as the mutant) | By reading. The one sentence is in D16, D17, and D18, and the case is in T4. |

### DMS-27: the designer's choice

The designer refuses a `sample`-to-`replica` group's re-grounding with a named reason and leaves it in
bucket 3, where the lead's alternative was a new grounding path. Accepted, for three reasons:

1. No grounded source exists. The named member in such a group built its image from its own sampled
   installs, truncated checkpoints removed the log it would be rebuilt from, and every other member is
   unvouched too. A path that grounds it would have to ground an image no one has checked, which is
   DMS-25's failure in the other direction (a grounding with a residual of 63/64 per corrupt flush
   that the text then calls "no residual"). Nothing false is grounded now.
2. The refusal is reachable only through an administrator's signed statement (`sample` to `replica`),
   so a compromised leader cannot cause it; the cost is the admin's act, which the statement already
   says it carries (`design:2196`, "images installed under `sample` or `replica` are unvouched").
3. It is loud (error log, `regrounding_refused{reason}`), and the text gives the working order
   (`install`, then `replica` naming a grounded voter, then `install`).

One thing is left on the table, a recommendation and no condition: the trap is entered at the
`sample`-to-`replica` statement and found at the `replica`-to-`install` switch, which can be months
later. A replica that applies a `replica` statement whose named member's own standing is unvouched
could log at warning level the one line "re-grounding from this group will refuse
(`document_member_unvouched`)", at the point where the admin can still choose.

### New finding

#### DMS-30. A replica that holds no sequence for a signer admits a replayed old statement, and nothing bounds the stamp it is checked against (low)

Attacker: a compromised leader, with an old statement an administrator signed for this group (any
`sample` or `replica` statement that is in the log, which the leader reads).

Evidence: the signed time is checked against the entry's leader stamp, "never the replica's clock"
(`design:2196`), and "a compromised leader chooses its stamps" is stated as PT's residual, but the
residual stated is only that a statement can be held and stamped forward. A leader can equally stamp
backward: it appends a `verify` entry carrying an old statement S (signed at time T) with a stamp
within 10 minutes of T. A replica that applied a higher sequence for that signer refuses S (the text
says so). A replica that holds no sequence for the signer admits it: a new member, whose shipped
`verify` state is discarded and starts at the absent reading (`design:2269-2271`, `2994-3004`, DMS-29's
own sentence), and a replica whose checkpoint was written without the attribute (D16's absent reading).
D16 says exactly this: "the stamp's 10 minutes are the only replay bound at that replica"
(`design:2270-2271`), a bound that a leader that picks the stamp does not meet. The effect is one
replica's setting moving from `install` to `sample` or `replica` by an entry no administrator signed
now: its checks fall from every flush to the samples, 96.9% of plants missed at rf = 3, for that
replica only (the others keep `install` and the group-level grounding is unaffected, since a switch
away from `install` already grounds nothing). Low for that reason, but it is the property DMS-26
exists to give ("a statement cannot be replayed to turn checks off"), and T4's new case states it
for the new member ("reach the verdict, the setting, the history bits, and the sequences every other
replica reached", `design:3292`), which is wrong in the other direction too: a new member's setting
is `install` at its image's position whatever the group's is.

Fix, text and one case, with the `verify` switch's code: because the state is per replica now, the
refusal can use that replica's own clock and stays a no-op that keeps the stricter setting: a replica
with no sequence for the signer refuses a statement signed more than D17's 10 minutes before its own
image was installed or its checkpoint written (by its own clock), and says so; honest statements are
signed after the admin reads the group's state, which is after the member joined or was written, so
none is refused that an honest admin signed for it. Or bind the statement to the log: its body names
the index of the `verify` entry it replaces, which every replica reads from its own log or image.
T4 gains the replay of an old `sample` statement at a new member under a back-dated stamp (the new
member keeps `install`), with the statement-only check as the mutant; and the new-member sentence says
"reaches the verdict every other replica would reach for that entry", not the group's setting.

### Blocking items and conditions

1. C23, C24, C25, C26, C27 are closed at the lines above. Nothing in DMS-25 to DMS-29 blocks.
2. **`Vouch` answering may land**: DMS-29, its only blocker, is closed, and nothing else was owed.
3. **The `verify` switch may land**: DMS-25 and DMS-26, its two blockers, are closed. DMS-30 is a
   condition on the switch's code, not on the design text: **C28**, the replay bound above, with its
   T4 case, before a `verify` statement is accepted by a replica that holds no sequence for its
   signer. The three text conditions that were owed with C24 to C27 (the K7 case for `replica`'s
   re-grounding, the CMake pin at step 8, and the PT row asking for a bound on the admin set) stay
   with their steps.
4. The architect's two notes (T4's `--merge-held` mutant, the K7 case) are outside this scope; both
   are present in T4 (`design:3292`) and the ledger lists the K7 case.

Not run: no strata, consensus, or journal build or suite (the branch changes no library code);
`vouch_model_rewind.py` and the architect's `switch_model.py` (outside DMS-25 to DMS-29); the cycle
model with `down` as `sample` only, so `replica`'s re-grounding (DMS-27) and the released follower's
return are argued from the text, as the designer says; DMS-26's replay and DMS-30 are argued, not
modeled, and no statement was constructed or verified (no `DTADMS01` code exists); no CMake file
(step 8) and no build of Kronuz/sha256 (the pin was checked against the remote's `HEAD`); no
benchmark; no Xapian or Xapiand GPL source opened.
