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
