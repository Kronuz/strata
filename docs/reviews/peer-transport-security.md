# strata security review: peer transport (design, revision 0)

- Date: 2026-10-17.
- Reviewed: `docs/design/peer-transport.md`, revision 0 at `8a1f840` on `design/peer-transport`
  (worktree `~/code/Kronuz/strata-wt/peer-transport` at `3015cab`), with its ledger
  `docs/status/peer-transport.md`. The architect reviews the design as a whole in parallel; this
  review covers what an attacker can do under it. No code exists for D2 to D15; the code read is
  what they extend.
- Surfaces read: consensus `fc66c0e` (`transport/{tls,hello,profile,session,codec,peers,
  snapshot_write}.h`, `core.h`, `worker.h`, `membership.h`, `types.h`, `snapshot_transfer.h`,
  `snapshot_channel.h`; `git diff --stat fc66c0e ef30b4f -- include/consensus/transport/` is
  empty, as the design's header says); reactor `e19afd9` (`reactor_tls.h`, `reactor.h:280-330`);
  journal `3dccf50`, consensus's pin (`codec.h:70-83`); strata `main` `5691ec7`
  (`docs/reviews/calibration.md`, `docs/reviews/system-2026-10-17.md` SYS-2, SYS-3, SYS-12,
  SYS-14, `docs/design/track-c.md`, `docs/design/limits.md`, `docs/DISTRIBUTION.md`,
  `docs/PLAN.md:26-46`); `design/simulator` `a6c1f4e` (`simulator.md`); Xapiand `design/pki` at
  `bfc11c50c` (`pki.md` at `d05ed6bab`, `pki-design.md`, `pki-security.md` with its re-check) and
  `master` `7e8cccb7d` (`transport-security.md`, `transport-security-design.md`); Detent
  `deae42d`, read only: `AUTHORITY_TRANSPORT.md`, `SHARED_LIBRARY_DECISIONS.md`,
  `LIBRARY_BOUNDARIES.md:180-200, 245-260`, `LIBRARY_ADOPTION_ARCHITECTURE.md:15-25, 75-95`,
  `DEPENDENCY_UPGRADES.md:35-45, 72-82`, `cmake/dependencies.json`.
- Checked by: `.scratch/review-peer-transport-security/pins.sh`, output in `pins.out`, under
  OpenSSL 3.6.5 (`/usr/local/opt/openssl@3`) and 4.0.3 (`/usr/local/bin/openssl`): the three
  SPKI digest forms for a P-256 leaf with an uncompressed point, a compressed point, and
  explicit curve parameters; a same-key renewal; a leaf the cluster CA mints for a node's public
  key; a leaf a rogue CA mints for it; re-verification under a bundle that dropped the CA. CI of
  the commits the design cites: consensus `fc66c0e` (run 39051586055, success) and reactor
  `e19afd9` (run 39047241795, success); the design's two commits are docs-only and skip CI by
  rule.
  - Not run: nothing compiled, started, fuzzed, or measured (the strata-trials strict hold); no
    listener, so no TLS version, suite, renegotiation, or certificate-list probe against a
    running node, no handshake-slot exhaustion, no flow-control run, and no truncated,
    oversized, or malformed frame fed to a decoder; no Linux or FreeBSD; Detent read only in the
    cited documents.
- Citations: `design:N` is line N of `docs/design/peer-transport.md` at `8a1f840`; code is at
  the commits above; `pki:N` is `pki.md` at `d05ed6bab`.

## Verdict

**APPROVE WITH CONDITIONS. The shape is right: mutual TLS with the chain verified first and
the leaf pinned by its public key, a directory that makes removal a pin change, frames that
name their incarnations with a binding checked before the core, credit for data and bounded
counts for control, and one sans-I/O policy. Three findings are high, and each is a rule or a
specification rather than a mechanism: D5's witness clause lets a vouched node speak as any
incarnation the replica has not seen, so a compromised learner can lead a group (SR1); the
directory, the identity root of the whole design, is applied from one root member's answer
with nothing but that member's session behind it (SR2); and the admin path that can register
a pin under any node id has no specification and no threat-model row (SR3). They are
conditions on revision 1, before first step 5, before any directory code beyond D3's static
preset, and before the root's admin interface. First steps 1 to 3 are additive and may start.**

What holds, and reproduces (`pins.out`, both versions):

- T1's procedure is right: `openssl x509 -pubkey | openssl pkey -pubin -outform DER | openssl
  dgst -sha256` equals the SHA-256 of the SubjectPublicKeyInfo bytes as they sit in the
  certificate's DER for uncompressed (`node`), compressed (`node-c`), and
  explicit-parameter (`node-x`) keys, and differs from the digest of the key's BIT STRING
  alone, `X509_pubkey_digest`'s input (SR12 of the PKI review). A same-key renewal (`node-r`)
  keeps the SPKI digest and changes the DER digest, which is the property D2 buys.
- The chain-first rule (`design:153-154`) is what stops a rogue CA: a leaf a rogue CA mints for
  the node's own key (`node-rogue`) carries the pinned SPKI and fails `openssl verify
  -CAfile ca.pem`. A leaf the *cluster* CA mints for that key (`node-mint`) verifies and
  matches the pin; only the handshake's proof of possession refuses it, which SR14 asks T1 to
  say.
- Re-verification against a new store has a mechanism: `node.pem` fails under a bundle holding
  only `ca2.pem` (D14, `design:546-548`).
- Today's code already orders the checks the design relies on: chain, then pin, then the hello
  (`session.h:124-135`); the dialer requires the node it dialed (`peers.h:344`); the codec
  bounds a count by the bytes left before it reserves (`codec.h:163-165`) and a payload by the
  bytes left before it copies (`codec.h:170-173`); `get32` and `get64` throw on short input
  (journal `codec.h:70-83`), so a truncated control frame cannot read past its end.
- Every figure in the design is cited code, arithmetic, or a labeled estimate; no row cites a
  measurement as a baseline (calibration row 3).

What blocks step 5, D4's code, and the admin interface: SR1, SR2, SR3, one line each under
"Blocking items".

## Attackers considered

| Attacker | Under the design | Gains | Findings |
|---|---|---|---|
| On the network, no key | TLS 1.2 or later, both chains verified, leaf pinned; hellos inside TLS | Nothing on content; with TLS 1.2, the certificate chains in clear name both nodes (SPIFFE URI, SPKI); 3.2 stalled connections a second hold every handshake slot of a node (`design:119`) | SR7, SR12 |
| Holder of a leaf the bundle verifies and no entry pins (a `client` leaf, a retired node's leaf) | Refused at the pin | A handshake slot for one handshake; under D12's event order, one hello parsed before the pin decides | SR13 |
| One compromised node, a voter | Acts as its incarnations | Everything Raft lets a Byzantine voter do in its groups: the accepted residual (`design:122`) | — |
| One compromised node, a learner, a witness-role member, or a stale former host | Rule 2's witness clause | Speaks as any incarnation the replica has not seen: leads voters or fences them, in every group it is vouched for | SR1 |
| One compromised node that is a root or placement-group member | Serves directory fetches and witness answers | Rewrites identities for every node that fetches from it: any pin under any node id | SR2 |
| Holder of an admin key | D4's commands | `AddPin` on any entry: impersonation of any node; `Remove`: eviction of any node; `Join` and `Activate`: a node of its own | SR3 |
| A peer that lies in its hello | Limits, versions, boot identity, endpoints | A command limit of 1 byte for every group it is in; a redial storm at an address of its choosing | SR4, SR11 |
| A peer that floods | Credit, budgets, control bounds | The node's whole incoming budget as outstanding grants; a frame that closes every other session while it is read; 43,800 heartbeat records resolved at once | SR5, SR6, SR8 |
| A removed node whose leaf is valid | Tombstone, sessions closed on apply | Its old sessions until each node applies the removal; indefinitely at a node partitioned from the root; a new identity if an admin registers its old pin again | SR9 |
| Replay or reordering across reconnects | TLS per session; a reset discards unadmitted frames | Nothing: Raft is idempotent by term and index, transfers carry session identities, boot identities are random | — |
| During a reload | A new context for new sessions, re-verification | A handshake that straddles the swap verifies under the old bundle; a resumed session skips re-verification where `strict` is off | SR10 |
| Local file access on a node | Key, leaf, bundle, seed, admin set, cached table | That node, or its trust: not in the threat model | SR13 |
| A compromised dependency | Kronuz libraries pinned by commit; OpenSSL from the platform | Not in the threat model | SR13 |

## Rulings

| Decision | Ruling | Findings |
|---|---|---|
| D1 One transport | Sound: no datagram path, no half-secured cluster | — |
| D2 SPKI pins beside certificate pins | Sound; the probes confirm the bytes and the chain-first rule; the ordered lookup's timing argument holds (a public key is public) | SR14 |
| D3 The directory | Sound; a removed pin may be registered again | SR9 |
| D4 Home, writers, dissemination | Wrong in two places: a version applied from one member's answer; an admin path with no specification | SR2, SR3, SR9 |
| D5 Admission | Sound for known incarnations; the witness clause is wrong as written | SR1 |
| D6, D7 Versions | Sound: hellos ride inside TLS, the floor is N − 1, the dialer re-checks the choice; a compromised peer buys features only | — |
| D8 The hello | Sound in shape; the limits field hands a member a lever over the group; the event order in D12 parses it before the pin | SR4, SR13 |
| D9 Frames and tags | Length rules stated, value rules not; one lane cap contradicts the fragment size; the directory delta has no format | SR8 |
| D10 Connections | Sound; the handshake caps are pre-authentication and have no per-source rule; the backoff is flat | SR7, SR11 |
| D11 Flow control | Sound mechanism; grants and reassembly can hold the budget without bound; control frames have no size rule of their own | SR5, SR6 |
| D12 The policy | Sound; `handshaken` carries the hello, which reorders today's checks | SR13 |
| D13 Keepalive | Sound; the version hint it carries drives fetches without a rate rule | SR9 |
| D14 TLS settings, reload | Sound in intent; the swap's edge cases and the key file's handling are unstated; peers could take TLS 1.3 | SR10, SR12 |
| D15 Enrollment | Right to defer (Q5); `Enroll` contradicts D4's "never over the peer transport" | SR3 |

## Calibration checks applied

- Row 1 (a second implementation): the keepalive detector names `cluster::LivenessTracker` and
  why it cannot run in the policy (`design:533-535`). D10's least-recently-used contact cache
  names nothing; `Kronuz/lru-cache` exists (header-only, O(1), TTL through `aging_lru`), and
  the design should name it or say why the policy's determinism rule excludes it. The
  windows, fragments, and `Directory` have no counterpart in the catalog. For the architect.
- Row 2 (CI per platform at the reviewed commit): the design's commits are docs-only; the
  code commits it reads are green (consensus `fc66c0e`, reactor `e19afd9`, above).
- Row 3 (baselines with raw files): none cited as measured. "45 s at p99" is the simulator's
  target bound (`simulator.md:124`), which the design says is never measured (`design:641`).

## Findings

### SR1. The witness clause lets a vouched node speak as any incarnation the replica has not seen (high)

Attacker: one compromised node that the witness vouches for and whose incarnation in the
group is not one it wants to use: a learner (every joining replica is one while it catches
up), a witness-role member, or a former host whose placement record lags.

Evidence:

- Rule 2 (`design:264-267`): the source is bound to n when "a membership the replica holds
  ... names it on n, or, when none names it, the host's witness vouches that n hosts a replica
  of the group." The witness vouches for the node, not for the incarnation (`Vouch{group,
  node}`, `design:492`). An incarnation no membership names, picked by the attacker, passes
  the second clause on any node the witness vouches for, lagging replica or not: the current
  voters do not know it either.
- The core admits any nonzero incarnation but its own (`core.h:736`) and lets any source lead:
  an `AppendRequest` at a term at or above the replica's makes it a follower of that source
  (`core.h:822-826`), a higher term resets its ballot (`core.h:750-751`), and a `VoteRequest`
  with `transfer` set bypasses the leader-contact guard (`core.h:746`; `types.h:129-130`). A
  learner's own incarnation can do none of this: it never campaigns (`core.h:977`) and its
  grants are not counted (`core.h:818`). As a fabricated incarnation, the same node makes every
  voter its follower, replicates what it likes, and advances their commit, or sends entries
  that conflict with a committed prefix and fences them (`core.h:843-846`).
- The design's own facts close it: an incarnation is never reused and never moves
  (`membership.h:128, 132`), and a node holds at most one incarnation of a group
  (`design:289-290`), which D5 uses for vote counting and not for admission.
- The threat model's row says a compromised node keeps "its own incarnations in the groups
  placed on it" and may "vote, append, or fence replicas there as itself" (`design:122`). For
  a learner that is more than its own incarnation allows; for a stale former host, the Limits
  section (`design:643-645`) says it "matters only if that node is compromised", which is the
  case the row is about.

What they gain: leadership of a group over its current voters from a non-voting seat, or the
fencing of its replicas, with no lag required; from a stale record, the same for a group the
node was removed from, for as long as the record lags.

Fix, three clauses on rule 2 and one on the witness, all checkable in T7:

1. A node that any membership the replica holds binds to an incarnation of this group is
   never vouched for another: one node, one incarnation per group (`design:289-290`), applied
   to admission. Such a frame is `forged_source` and closes the session.
2. An incarnation no membership names is admitted only when it is at or above the newest held
   membership's `next_incarnation`; below it, it is retired or invented, so `forged_source`.
3. The witness vouches for `(group, node, incarnation)`: the parent authority records the
   incarnation it placed, so an invented incarnation is never vouched for.
4. T7 adds, beside the admitted cases: a learner's session sending as an unknown incarnation
   (refused by clause 1); a removed member's old incarnation from its node after compaction
   (refused by clause 2); an unknown incarnation the witness does not vouch for by number
   (dropped as `unbound_source`).

The residual after the fix: a replica behind by a whole add-and-remove cycle while the record
lags admits the removed node's real former incarnation. State it in the Limits section; Q2's
option 2 is the path if T10 shows record lag past the compaction horizon.

### SR2. The directory and the witness are applied from one member's answer (high)

Attacker: one compromised node that is a root member, voter or learner, or a member of a
placement group; the design's threat model treats every node alike (`design:122`).

Evidence:

- "A node that sees a newer version fetches the delta from a root member, voter or learner,
  over its own authenticated session" (`design:246-247`). The session authenticates the
  member; nothing authenticates the table: a read served by one replica carries no quorum, and
  a learner's table is whatever it says it is.
- The invariants are "checked again by every node that applies a change" (`design:187-191`),
  which refuses an inconsistent table and accepts a consistent forged one: `Join{attacker's
  id, attacker's second pin}` then `Activate`, or `AddPin{victim, attacker's pin}`, which
  passes invariant 1 as long as the pin names no other node.
- The witness has the same shape: the host asks "the group's parent authority"
  (`design:279-281`) and the answer comes from a member of that group over the transport, with
  no statement that it is the leader's linearizable read or a signed record.
- The root membership a node needs to know whom to fetch from is "in its seed, which admins
  update with each root membership change" (`design:281-283`): a file on 100,000 nodes,
  updated by hand, which is also where a node learns whose answer to trust.
- The design already names the remedy, as a fallback for load: "root voters sign each version
  by quorum and gossip relays it" (`design:255-257`).

What they gain: with `AddPin` on any entry, authentication as any node to every node that
fetched from the attacker, and so that node's incarnations in every group placed on it; with
`Remove`, eviction of any node from those nodes' view; with a forged witness answer, the
vouching SR1 restricts. One compromised node becomes the cluster's identity authority for
everyone it serves.

Fix: a directory version is applied only when it carries signatures from a quorum of the root
voters named by the version before it, chained from the seed's founding voters; the root
membership is a column of the signed table, so the seed is read once and never updated by
hand; any node may relay a signed version, which also answers D4's load question. The witness
answers come from the parent group's leader through a linearizable read, or as a record the
parent group's voters signed; the design states which. T2 gains a forged delta from a learner
(refused unsigned, applied signed) and a replayed older version (ignored); T10's fetch rows
measure the signed form. Until this lands, the threat model says that a compromised root or
placement member is a cluster compromise.

### SR3. The admin path can register a pin under any node id and has no specification (high)

Attacker: whoever holds an admin key, reaches the admin interface, or alters the admin set;
the design has no row for any of them.

Evidence:

- Writers: "a client whose SPKI pin is in the root's admin set, fixed when the root is founded
  and changed by admins" (`design:218-220`). Unstated: the interface's listener and address,
  its TLS role (reactor's `Peer` role needs a chain to the bundle, so an admin leaf is a
  `client` leaf of the same CA; what EKU and SPIFFE path it needs), how the admin set changes
  (no command in the table, `design:222-229`), whether one admin suffices, replay and
  idempotence of commands, rate limits, and an audit record.
- `AddPin{node, pin}` is refused only when "the entry has two, the pin names another node, or
  the node is removed" (`design:226`): an admin adds its own pin to any node's entry and
  authenticates as that node everywhere, with the overlap D4 designed for re-keys as cover.
- D15 has the issuer commit `Enroll{token, node, pin}` to the root (`design:568-570`), a root
  command over the peer transport, while D4 says root commands travel "never over the peer
  transport" (`design:219`).

What they gain: with one admin key, impersonation of any node, eviction of any node, or a
node of their own, cluster-wide, with no log that says so.

Fix: a threat-model row for the admin-key holder and one for the admin interface on the
network; a specification: the listener on root hosts bound to an address the operator
chooses, mutual TLS under the cluster CA with the admin's leaf checked by SPKI pin against the
admin set and by a `client` path and `clientAuth` EKU (the PKI design's D5), commands as
signed, numbered, idempotent entries committed by the root (so a replay is a no-op and a
retry safe), `AddAdmin` and `RemoveAdmin` with a two-admin rule or a stated reason to have
none, `AddPin` and `Join` logged with the admin's pin in the root's log, and a rate limit per
admin. Resolve `Enroll`: the issuer is an admin-class principal with its own pin in the admin
set, and D4's sentence says so.

### SR4. A member's node limits in the hello cap the group's command limit at 1 byte (medium)

Attacker: one compromised node in a group, a learner included; or a misconfigured honest one.

Evidence:

- The hello carries the node's limits, "reported to the core as `PeerLimits` for each of the
  node's incarnations" (`design:357, 364-365`). `validate_limits` requires only
  `command_bytes > 0`, `command_bytes <= rpc_bytes`, and `rpc_bytes >= membership_bytes(members)`
  (`membership.h:47-56`), 1,652 bytes at 64 members.
- The leader's `wire_limits()` takes the minimum of every replica's reported `command_bytes`,
  `rpc_bytes`, and `rpc_entries` (`core.h:313-331`), and a proposal above it is rejected as
  `TooLarge` (`core.h:700`). `peer_wire_limits()` already does the right thing for the
  wire: a member's lower limits hold back what is sent to it and nothing else (`core.h:333-348`,
  "M8").

What they gain: every command above 1 byte refused in every group the node is in, reported to
clients as `TooLarge`, with nothing in the transport's counters.

Fix: `wire_limits()` caps by the node's own limits and the membership's limits in force only;
a member whose report is below the group's limits is held back by `peer_wire_limits`, counted
(`limits_below_group`), and logged with its node id, and placement may treat it as unable. A
report may not lower a group's limits; raising the group's limits still checks every member
(`core.h:620-628`), which is the right direction. T7 adds a learner reporting the floor while
commands at the group's limit still commit.

### SR5. Grants and reassembly can hold the node's whole incoming budget (medium)

Attacker: one authenticated peer, or a few, against a node's 256 MiB incoming budget
(`design:446-447`).

Evidence:

- A blocked sender asks with `wanted`, and "the receiver grants at least that once the group's
  queue is empty, within the node ceilings (commands up to 64 MiB) and the budget"
  (`design:449-452`). A peer asks for 64 MiB on each of four groups and sends nothing; the
  grants are outstanding until the connection closes (`design:471-475`), and the design says
  only that it "never grants past the budget" (`design:447-448`). Initial grants already take
  64 MiB of the 256 at P = 64 (`design:591`). No per-connection or per-peer share exists.
- "The whole frame's credit [is] held from its first fragment" (`design:462`), with no rule
  for a fragment sequence that stops: the credit, and the reassembly buffer the receiver
  allocates against it, live as long as the connection.
- `Window` carries "Increment (8)" (`design:396`) with no ceiling: a peer sends 2^64 − 1 and
  the honest sender's window arithmetic wraps, in the honest node.

What they gain: other peers' groups never grow past their initial windows, or a node whose
budget is pinned by a few idle grants; an overflow in the sender's bookkeeping.

Fix: outstanding grants per connection capped at the connection window's maximum (64 MiB) and
per peer at the budget divided by the active peers, so one peer holds at most its share;
`wanted` honored up to the group's maximum window (4 MiB on the control lane), and up to the
command ceiling only for an incarnation the replica binds as the group's leader; a fragment
sequence idle for `rpc_timeout` is discarded, its credit returned and `fragment_stalled`
counted; a window above 2^62 is `credit_violation`. T4's oracles gain all four; T6 adds one
peer asking for 64 MiB on 100 groups while another's commits stay within 1.5×.

### SR6. A control frame's size has no rule of its own, and one node budget serves every session (medium)

Attacker: one authenticated peer on a node whose node limits are near the ceilings, or several
peers at the defaults.

Evidence:

- "Control frames need no credit" (`design:453`); their bounds are counts (8 per group, 16 host
  control frames) and, for host control frames alone, 4 KiB (`design:608-609`). Every other
  control frame is bounded by the lane cap, `control_frame_bound(node limits)`
  (`design:610`): 2.0 MiB at the defaults and about 258 MiB at the ceilings
  (`104 + 32 × 65,536 + 256 MiB`; `membership.h:37`, `codec.h:10-15`).
- The session allocates a frame as soon as its 4-byte length passes the cap
  (`session.h:164-168`), from one `ByteBudget` shared by every session of the node
  (`peers.h:301`, `peers.h:60`); a frame that cannot acquire throws, and `receive()` closes
  that session (`session.h:62-64`, `peers.h:414-416`). The decoder refuses the padding only
  after the whole frame is read (`codec.h:211-212`), up to 5 s later (`session.h:159`).
- D9's kind sits in the 40-byte header after the 4-byte length (`design:375-383`); the
  design says the hello is read in two steps (`design:604`) and says nothing like it for
  frames, so the credit or class check cannot run before the allocation T8 promises happens
  after it ("allocates nothing before its credit", `design:672`).

What they gain: while a near-budget "control" frame is being read, every other session's next
frame fails admission and closes, every few seconds, from one peer; at the defaults, the same
needs the sum of a few peers' frames (2.0 MiB each, two lanes) against a 16 MiB budget
(`peers.h:32`) in Detent's configuration.

Fix: read the 40-byte header first, then decide: a data-class frame against the group's and
connection's credit, a control-class frame against a per-kind maximum (votes, responses,
`TimeoutNow`, `Wake`, `LeaderGone`, `Ping`, `Pong`, `Window` at their fixed sizes; coalesced
heartbeats at 48 bytes per record, bounded by SR8's record rule; host control at 4 KiB), and
refuse before allocating; a per-connection share of the budget, so one session's bytes never
close another. T8's allocation-counting test covers a control frame declaring the lane cap.

### SR7. Sixteen handshake slots are the whole pre-authentication defense (medium)

Attacker: any host that can reach a node's peer port; also a compromised peer, which can hold
slots before it is identified.

Evidence: 16 handshakes in flight, each under 5 s (`design:429-431, 612`), so 3.2 stalled
connections a second from one source keep every slot busy, as the design computes
(`design:119`). Nothing is per source. Today's cap is 2, and the accept loop drops the socket
at the cap (`peers.h:33, 284-292`). reactor's accept admission counts connections, not
handshakes per source (`reactor.h:287-289`). A compromised node can stall 16 handshakes at a
victim before any pin is checked, and it dials from inside.

What they gain: a node that accepts no new peer or contact session while the flood lasts; its
existing sessions survive, and every reconnect after a blip fails, so the keepalive declares
it down to each peer whose connection drops. Cheap against one node; a cluster-wide outage of
new sessions costs the attacker 3.2 connections a second per node.

Fix: an in-flight cap per source address (2) under the total, counted and logged; a shorter
pre-authentication deadline (the TLS handshake and the hello within 2 s, the rest of today's
5 s for the session) so a stalled slot frees sooner; since either node may dial
(`design:426-428`), a node whose acceptor is full keeps dialing out, so a pair with one
jammed acceptor still connects; the row in the threat model states the residual as what any
host on the network can do, with the figures. T8 adds the flood with a legitimate peer
connecting through it.

### SR8. D9 states length rules and no value rules, one cap contradicts the fragment size, and the directory delta has no format (medium)

Attacker: an authenticated peer sending well-formed frames with hostile values; the fuzzing
T8 promises finds crashes, not these.

Evidence and fixes, one per decoder:

- **Frame header** (`design:377-383`): a group-level kind with a zero group or incarnation,
  or a node-level kind with a nonzero one, has no stated refusal. Rule: refuse both as protocol
  errors that close the session.
- **`Fragment`** (`design:398, 459-467`): the inner kind is unconstrained, so a fragmented
  `VoteRequest` would ride on credit and skip the control-frame counts; the design has no
  order, overlap, or total rule beyond T4's "a fragment accepted out of order" mutant. Rule:
  the inner kind is a data-class kind; fragments arrive in order, contiguous (`offset` equals
  the bytes received), with `offset + bytes <= total` checked without overflow; `total` is at
  most the lane's cap for that kind and at most the credit held; a mismatch closes the session.
- **The bulk lane's cap**: "`maximum_transfer_bytes` plus 40 on the bulk lane" (`design:610`;
  65,728 bytes) against 256 KiB fragments for host data (`design:461-462`) and D18's 64 KiB
  chunks (`design:445`). One of the two numbers is wrong; the cap is what `session.h:164`
  enforces before allocation, so it must be stated for each class on each lane.
- **`Ping`, `Pong`, `Window`** (`design:394-396`): fixed bodies with no "exactly" rule.
  Rule: exact lengths (24, 16, 16), anything else a protocol error. `Pong`'s echoed time is
  the sender's own and is clamped against a negative difference.
- **Coalesced heartbeats** (`design:392, 272-274`): 48 bytes per record under the lane cap
  gives about 43,800 records per frame at the defaults, each resolved at once since
  node-level control frames "run at once and are never queued" (`design:455`). Rule: records
  per frame at most the groups the receiver shares with that node, and one record per group;
  more is a protocol error. This is C2's body; the bound belongs here because the transport
  enforces it.
- **The hello's extensions** (`design:360`): the count is 2 bytes with no rule beyond "within
  the length", and tag 1 may repeat. Rule: count at most the remaining bytes divided by 6;
  a repeated tag is refused; tag 1's family is 4 or 6 and its length is a multiple of 19.
- **`Transfer`** (`design:397`): "whose envelope names the header's incarnations" is an
  assertion, not a check. Rule: the envelope's source and destination equal the header's
  incarnations and its session identities equal the connection's, else the frame is refused,
  as `snapshot_channel.h:215` checks today.
- **The directory delta**: T8 fuzzes it (`design:672`) and D3 and D4 define no wire form.
  Rule: a versioned record (magic, version, from, to, a bounded count of entries, each fixed
  size) decoded with the count bounded by the bytes left before any allocation; SR2 adds the
  signatures.

### SR9. Revocation's window is unbounded in a partition, hints drive fetches without a rate, and a removed pin can return (medium)

Attacker: a removed node whose leaf is valid (its key was lost or exposed, `design:192-193`);
a compromised node forging version hints; an operator at `Join`.

Evidence:

- "A node that cannot reach the root keeps its table: existing nodes keep authenticating"
  (`design:251-252`). A node cut off from the root never applies the removal, so the removed
  node's sessions to it, and new ones, last as long as the partition. The threat model's
  residual, "D4's window, at each node that has not applied the removal" (`design:121`), reads
  as the 45 s plus a fetch of `design:641`.
- "A forged hint costs one fetch" (`design:248`), per hint: each `Ping` carries the sender's
  version (`design:394, 526-527`) every 500 ms to every peer and contact, so one node makes
  up to 192 nodes fetch twice a second, 384 fetches a second at the root from one node, for as
  long as it likes.
- `Join` is refused when "the id was ever used, or the pin names another node"
  (`design:224`); a removed entry holds no pins (`design:183`), so the removed node's pin names
  no node and a `Join` with it under a new id is accepted: the exposed key gets a new identity.

What they gain: the removed node acts as its incarnations toward partitioned nodes for the
partition's life; a fetch load the root did not budget (`simulator.md:132` budgets one entry a
second); an identity for a key that was removed because it was exposed.

Fix: a node fetches at most once per interval (1 s) whatever the hints, ignores hints above the
newest version a root member confirmed, and counts the excess (`hint_ignored`); the threat
model states the partition residual plainly, and the owner decides whether a node whose table
is older than a deadline (an hour) refuses *new* sessions from nodes it cannot confirm, at the
cost of availability during long partitions (my recommendation: no, state it); a removed pin is
burned (invariant 4: a digest ever removed is never registered again) unless an admin passes
an explicit override that the root logs. T2 gains the burned pin and the hint rate.

### SR10. Reload's edges: the straddling handshake, resumed sessions, and the key file (low)

Attacker: a peer whose CA or pin a reload removes; a local user who can replace a file the
reload reads.

Evidence:

- Sessions hold a `shared_ptr<asio::ssl::context>` (`session.h:102-106, 256`), so a
  handshake in flight at the swap completes under the old bundle; D14 re-verifies "each
  session's peer chain" when the bundle changes (`design:546-548`) without saying that the
  re-verification covers sessions that become ready after it ran.
- Without `strict`, OpenSSL may resume sessions (`reactor_tls.h:70-75` turns resumption off
  only under `strict`), and Detent keeps `strict` off (`design:539-542`); a resumed session's
  chain is the original handshake's, so a re-verification keyed on handshakes misses it.
- The host's files are read by path: `use_certificate_chain_file`, `use_private_key_file`
  (`reactor_tls.h:77-78`), which follow symlinks and open after whatever mode check the host
  made (SYS-13, `PLAN.md:42-44`), a time-of-check race the PKI design closes for its own files
  with `openat` (`pki:337-339`). D14 says nothing about the key file's mode at startup or at
  reload, and "Nothing changes in reactor" (`design:552-553`) leaves no way to hand reactor an
  already-checked descriptor.

What they gain: a session under a dropped CA that survives the reload until the next; a key
file swapped between check and open by whoever can write its directory.

Fix: the swap sets a generation; every session records the generation it verified under, and
the re-verification runs over every session whose generation is older, including sessions that
become ready after the swap and resumed ones (`SSL_get0_verified_chain` against the new store);
the host applies SYS-13's rule to the peer key at startup and at every reload, opening the file
once, `fstat`ing the descriptor, and reading from it; reactor gains an additive `Options` field
for in-memory PEM so the checked bytes, not the path, reach OpenSSL, or the design states that
the race is accepted and why. T11's reload row adds a handshake started before the swap and
completed after it.

### SR11. A flat backoff and announced endpoints make every peer a dialer for the attacker (low)

Attacker: one compromised node.

Evidence: the backoff is `100 + (peer × 17 + lane × 31 + reconnects × 13) mod 100` ms
(`peers.h:329-331`), kept by the policy (`design:101, 498`): 5 to 10 dials a second per node
pair and lane, forever. A node announces its listen endpoints in its hello (`design:360`), and
hints "may come from gossip or configuration; a wrong one costs a failed dial" (`design:194-195`).

What they gain: every node that wants the attacker as peer or contact, up to 192, dials an
address of the attacker's choosing 10 to 20 times a second: a modest connection flood from the
cluster's addresses with a TLS ClientHello in each, and a scan of whatever the attacker names.
Without an attacker, a dead peer is redialed by every node that wants it at the same rate.

Fix: exponential backoff from 100 ms doubling to a cap (30 s) with the existing jitter, reset
on success; an endpoint accepted from a hello only for the node that sent it, only within the
address ranges the operator configures for the cluster, and only while a reason to dial exists;
dial failures counted per endpoint. T4's backoff oracle changes to match.

### SR12. Peers could take TLS 1.3 and keep nothing of 1.2 (low)

Attacker: a passive observer; a recorder who later steals an RSA key (the PKI review's SR9).

Evidence: the floor is TLS 1.2 for every role (`reactor_tls.h:66`); `strict` restricts 1.2's
suites and turns resumption off (`reactor_tls.h:70-75`) and is off by default so Detent's
contexts stay (`design:539-542`). In TLS 1.2 the certificate chains travel in clear: each
handshake names both nodes by SPIFFE URI and public key to anyone on the path. Both ends of a
peer connection are this library on OpenSSL 3, so no client needs 1.2.

What they gain: the cluster's node identities and topology from handshakes; on non-strict
contexts with RSA leaves, static-RSA sessions decryptable after a key theft.

Fix: the `Peer` role under `strict` sets `SSL_CTX_set_min_proto_version(TLS1_3_VERSION)`;
strata and Xapiand set it, Detent decides for itself as today. T1 adds a TLS 1.2 ClientHello
refused by a strict peer beside a 1.3 one accepted. The threat model's row 1 residual says
what TLS 1.2 handshakes reveal.

### SR13. The threat model omits actors, and D12's event order parses the hello before the pin (low)

Evidence:

- No row for local file access on a node: the key, the leaf, the bundle, the seed
  (`design:231-233`, the root's founding pins, so whoever writes it chooses the root a node
  trusts), the admin set, and a cached table. The PKI review's SR13 row for "a writer of a
  consumer's certificate files" covers the CA file and the key's symlink, for Xapiand.
- No row for the admin-key holder (SR3), for a compromised root or placement member (SR2),
  or for a compromised dependency: the Kronuz libraries are pinned by commit (consensus pins
  journal `3dccf50` and reactor `e19afd9`; strata and Detent pin by commit), OpenSSL comes
  from the platform and floats with it, and nothing logs the OpenSSL version a node links.
- The "one compromised node" row (`design:122`) describes a voter; a learner is also a node
  placed on it (SR1).
- D12's input is `handshaken{connection, certificate digest, key digest, hello}`
  (`design:489`): the driver has read the hello before the policy's pin lookup decides
  admission. Today the pin is checked before the hello is read (`session.h:125, 131`), which
  is what T7's "a TLS client with an unpinned key reach[es] no decoder" (`design:671`)
  requires. Any holder of a leaf the bundle verifies would otherwise reach the hello decoder.

Fix: the rows, in the table's form; `handshaken` carries the digests alone, the policy answers
`Admit` or `Close`, and the driver reads the hello only after `Admit`, so T7's claim holds
by construction; the OpenSSL version logged at startup.

### SR14. Gate-row notes from the probes (low)

- T1's pipeline is the right oracle: it equals the in-certificate SPKI bytes for uncompressed,
  compressed, and explicit-parameter P-256 keys under 3.6.5 and 4.0.3 (`pins.out`). The
  design's `i2d_X509_PUBKEY(X509_get_X509_PUBKEY(leaf))` re-encodes the parsed structure, so
  it equals the DER's bytes for any DER certificate; T1 may state that.
- A leaf with explicit curve parameters fails chain verification under both versions, "error
  94 ... Certificate public key has explicit ECC parameters" (`pins.out`). T1 lists it as a
  refusal case so no one relaxes the verifier to admit a bring-your-own leaf.
- `node-mint`: a leaf the cluster CA mints for a node's key verifies and matches its SPKI pin.
  Only the handshake's proof of possession (TLS 1.3's `CertificateVerify`, TLS 1.2's
  `CertificateVerify` and signed key exchange) refuses it, and OpenSSL refuses to configure a
  mismatched key locally (`SSL_CTX_check_private_key`, `reactor_tls.h:79`), so T1 cannot test
  it with OpenSSL as the client. T1 states the mechanism and the design's threat model names it
  in the CA-holder row, as the PKI review did (`pki-security.md`, "Rulings").
- T3's downgrade case is a peer that offers (N − 1, N − 1): accepted, as D6 intends; the row
  should also show a peer whose hello's chosen version is above both maxima or below both
  minima refused by the dialer's own computation (`design:308`).

## Blocking items

None for first steps 1 to 3, which are additive and touch none of the findings. The following
block the work named after each, until revision 1 closes them:

1. SR1: first step 5 (the binding rule and the witness hook), and any claim of T7 for
   learners and former hosts.
2. SR2: any directory code beyond D3's static preset, Track D's node table, and the witness
   from placement records.
3. SR3: the root's admin interface and its commands.

## Conditions

1. C1 (SR1): rule 2's three clauses and the incarnation in `Vouch`, with T7's four cases, in
   revision 1.
2. C2 (SR2): quorum-signed directory versions as D4's rule, the root membership in the signed
   table, the witness's source stated, and T2's forged-delta cases, in revision 1.
3. C3 (SR3): the admin path specified with its threat-model rows, the admin-set commands, the
   audit record, and `Enroll` resolved, in revision 1.
4. C4 (SR4): `wire_limits()` caps by the node's own and the group's limits only, a member
   below them held back and counted, with its T7 case, before step 4's hello carries limits.
5. C5 (SR5, SR6): per-connection and per-peer grant caps, `wanted` bounded, a fragment stall
   timeout, a window ceiling, the two-step frame read with per-kind control caps, and a
   per-connection budget share, with T4, T6, and T8 cases, before step 4.
6. C6 (SR7): a per-source handshake cap, the pre-authentication deadline, and the residual
   stated with its figures, before step 4.
7. C7 (SR8): the value rules per decoder, the bulk-lane cap stated per class, the delta's wire
   form, before step 4's fuzz targets are written.
8. C8 (SR9): the fetch rate and hint ceiling, the burned-pin invariant, and the partition
   residual stated, with the owner's answer on a staleness deadline, in revision 1.
9. C9 (SR10 to SR14): the reload generation and the key file's handling; exponential backoff
   and endpoint acceptance rules; TLS 1.3 for strict peers; the threat-model rows and
   `handshaken` without the hello; the T1 and T3 notes; in revision 1 or the next edit.

Not run: nothing was compiled, started, fuzzed, or measured, under the strata-trials strict
hold; no listener existed, so TLS versions, suites, renegotiation, the certificate-list cap,
handshake exhaustion, credit, and every decoder's behavior on truncated, oversized, or
malformed input are judged from the design's text and the code's current form, not exercised;
the probes are `openssl` CLI only and never crossed a socket; no Linux or FreeBSD host;
Detent was read only in the documents cited; the simulator's and PKI's designs were read at the
commits named, not run; the architect's parallel review was not read.

## Re-check

- Date: 2026-10-17.
- Reviewed: `git log cb893e8..design/peer-transport`: `f1f1d85` and `e4bfa3c` (the two reviews),
  `ef82b06` (the ledger's decisions on Q1 to Q5), `90d417e` (revision 1 of
  `docs/design/peer-transport.md`, 1,261 lines, with its "Changes from revision 0" table),
  and `2600377` (the ledger, with Q6 decided at `docs/status/peer-transport.md:108`). Against
  them: the architect's review (`docs/reviews/peer-transport-design.md`, PT-R1 to PT-R19, its
  PT-R10 and PT-R11 read in full), Track C's D1 and D3 (`track-c.md:216-218, 324-327`), and
  consensus `fc66c0e`: `membership.h:91-140, 185`, `core.h:78-84, 185, 275-279, 302-308,
  610-616, 643-647, 850-890, 1010-1026`, `types.h:180-184, 236-240`; `Kronuz/lru-cache`
  `70b9663` (`lru.h:209, 227, 241, 298`).
- Checked by: reading and arithmetic only. The kind sizes in D9's table were recomputed from
  the codec's fields with the kind moved into the header (`codec.h:51-112`); `pins.sh` was not
  rerun, since D2's bytes are unchanged.
  - Not run: nothing compiled, started, fuzzed, or measured (the strict hold held throughout);
    no new probe crossed a socket; no Linux or FreeBSD.
- Citations: `design:N` is line N of revision 1 at `90d417e`; `ledger:N` of
  `docs/status/peer-transport.md` at `2600377`; `arch:N` of `peer-transport-design.md` at
  `e4bfa3c`.

### Verdict

**APPROVE WITH CONDITIONS. Revision 1 closes SR1 to SR14: the three high findings have their
mechanisms (D5's rules with a vouch by incarnation and the committed counter, D16's certified
versions, D17's signed statements), and every medium one has its rule and its gate row. The two
rulings asked for: the incarnation counter rule holds; "one voice per node" is right in intent and
wrong in two details (SR16). One new high finding (SR17): the invariant that D5's vote argument and
the removal remedy rest on, one incarnation of a group per node, is stated in Track C and enforced
by no validator, so D18's check as specified does not carry it. Step 5 may land once SR16's two
changes and SR17's rule are in its own scope, with their T7 cases; SR15 and SR18 are text for the
next revision and step 8, and block nothing before it.**

What holds, and reproduces:

- **The counter rule** (`design:364-366`): every incarnation a change adds is at or above the
  previous membership's counter (`membership.h:128`), and the counter never moves back
  (`membership.h:123`), so an unknown incarnation below the newest committed membership's
  counter is retired or invented, never new. "Uncommitted bindings only admit" (`design:363,
  367-373`) is right for the reason given: two leaders' uncommitted entries can give one number
  to two nodes, and only the committed log decides. The core already takes the newest held entry,
  committed or not, as the membership in force (`core.h:185`; `track-c.md:216-218`, Raft's own
  rule), so what guards a replica against an uncommitted entry's content is D18's check at
  append, which is where SR17 lands.
- **Certified versions** (`design:919-938`): a node adopts a version only when its own S(v),
  computed from the table it applied the delta to, is what a majority of the named epoch
  signed, each signature checked against the epoch's SPKI; epochs chain from the seed by a
  majority of the one before; relays carry nothing a node trusts. The context prefix
  (`design:926-931`) separates these signatures from TLS 1.3's `CertificateVerify`, which uses
  the same key. A fork in the root's log at one version converges through the full-table path,
  since the certificate is over the table's hash and "any failure keeps the old table"
  (`design:938`); SR15 asks that the table be requested after a delta fails.
- **The admin path** (`design:978-986`): statements verified again by every root replica as it
  applies them, so a root leader cannot add directory changes alone; the signature, not the
  channel, carries authority (`design:997-999`), which resolves `Enroll`.
- **The kind sizes** (`design:551-562`): with the kind in the header, a `VoteRequest` body is
  term, last index, last term, and transfer, 32 bytes, so 72 with the header; `AppendRequest`
  without entries 8 fields, 104; `AppendResponse` 96 or 112; `SnapshotResponse` 88; the
  pre-vote pair and `TimeoutNow` 64; `Ping` 64 and `Pong` 56; `Chunk` 40 + 65,688 = 65,728;
  `TransferControl` 40 + 128 + 6,312 = 6,480 (`snapshot.h:53`). The table is right, and the 1 MiB
  wire maximum removes revision 0's bulk-lane contradiction (`design:1064, 1069`).
- **Resumption across a reload** (`design:854-856`): a new `SSL_CTX` has its own session cache
  and fresh ticket keys, so nothing resumes into it; the generation argument holds.

### Status

| ID | Status | Evidence in revision 1 |
|---|---|---|
| SR1 | Closed; its first clause's replacement is SR16 | `Vouch{group, n, incarnation}` (`design:367-368, 760`); an unknown incarnation below the committed counter is `forged_source` (`design:364-366`); the witness answers for one incarnation from committed records or a linearizable read through the parent's leader (`design:396-409`); T7's cases (`design:1187`); the residual stated as one of safety (`design:185, 1149-1153`) |
| SR2 | Closed | D16: a majority of the signer epoch over the node's own S(v) (`design:919-923`), the chain from the seed (`design:905-911, 932-938`), relays (`design:939-943`), the root's members in the certified state so no seed is edited by hand (`design:911-912`); the witness's source (`design:401-405`); threat rows (`design:186-187`); T2's forged, minority, learner, replayed, and forged-epoch cases (`design:1182`). The hint path is SR15 |
| SR3 | Closed | D17 (`design:968-999`): the listener, admins pinned by SPKI and path, signed numbered idempotent statements verified by every replica, two admins for `AddPin`, `ReplacePin`, `AddAdmin`, `RemoveAdmin`, rate limits, the audit; the issuer an admin-class principal (`design:894-896`); threat rows (`design:188-189`). What one admin keeps is SR18 |
| SR4 | Closed | `wire_limits()` from the group's limits, the leader's node, and what a quorum of voters accepts; a voter below held back and counted (`design:508-517`); T7's case (`design:1187`) |
| SR5 | Closed | The per-peer share (`design:681-683`); `wanted` bounded by the announced bound (`design:701-703`); the FIFO reserve with `reserve_stalled` (`design:709-712`); windows above 2^62 (`design:715-716`); `fragment_stalled` (`design:585-586`); growth only to the known leader or transfer source (`design:679-681`); T4 and T6 (`design:1184, 1186`) |
| SR6 | Closed | Length and header before the body, the decision before allocation (`design:539-545`); per-kind sizes (`design:549-568`); per-connection control pools (`design:718-721`); the wire maximum (`design:1064`); T8's control frame declaring the maximum (`design:1188`) |
| SR7 | Closed | Two per source, 16 in all, 2 s before authentication, the operator's ranges, dialing out while full (`design:645-652`); the residual with its figures (`design:180`); T13 (`design:1193`) |
| SR8 | Closed | Levels (`design:546-547`); value rules and the `Chunk`/`TransferControl` split (`design:563-564, 570-576`); fragments (`design:577-586`); the close-or-drop table (`design:588-595`); hello extensions (`design:498-505`); the directory's four formats with their bounds (`design:957-966`) |
| SR9 | Closed | One fetch a second whatever the hints (`design:944-947`); burned digests with `allow_burned` audited (`design:318-320`); the partition residual (`design:183, 1145-1148`); Q6 decided, no deadline (`ledger:108`); T2 (`design:1182`). The fetch target is SR15 |
| SR10 | Closed | Generations over every older session, straddling and resumed ones (`design:854-859`); key files read once through a descriptor into reactor's in-memory PEM, at startup and reload (`design:845-850`); T11 (`design:1191`) |
| SR11 | Closed | Exponential backoff to 30 s, Detent's preset flat by its choice (`design:653-655`); endpoints only for their sender within the operator's ranges (`design:276-280, 501-505`); T4 (`design:1184`) |
| SR12 | Closed | TLS 1.3 minimum for the `Peer` role under `strict`, no running configuration changed (`design:839-844`); the TLS 1.2 residual (`design:180`); T1 (`design:1181`) |
| SR13 | Closed | Rows for local files, dependencies, the CA holder, admins, root and placement members, and the learner case (`design:182, 184-189, 195-197`); `handshaken` carries digests only and the hello follows `Admit` (`design:757, 768-770`); the OpenSSL version logged (`design:865`) |
| SR14 | Closed | The re-encoded SPKI, explicit parameters refused, proof of possession stated without a case (`design:226-236`); T1 (`design:1181`); T3's chosen-version refusals (`design:1183`) |

Conditions C1 to C9 of the first review are closed by the rows above.

### New findings

#### SR15. The fetch target follows the highest hint, so one peer can hold a node's directory back (medium)

Attacker: a peer that advertises versions it cannot certify; the node it most wants kept stale
is each of its own peers, since those are the sessions a removal would close.

Evidence: a node "fetches from the peer that advertised the highest version, at most once a
second whatever the hints" (`design:945-946`), a reply that fails verification marks that
peer's hint ignored "for that version" (`design:946-947`), and the 60 s pass to a peer in turn
runs only when the node "heard no newer hint for 60 s" (`design:948`). A peer that advertises a
new higher version each second keeps the one fetch a second pointed at itself and keeps the
60 s pass from running; versions another peer certified wait as long as the hints continue.
The design's T10 window (`design:949-951`) assumes the hint path is honest.

What they gain: a node that applies no removal, its own included, while it stays connected to
the attacker.

Fix: a peer whose reply failed verification is distrusted for hints for a doubling interval
(1 s to 60 s); the fetch target rotates among peers whose hints are not distrusted and exceed the
held version; the 60 s pass runs on a timer, hints or none; after a delta's hash check fails the
node asks the same or another peer for the table, so a fork at one version converges. T2 adds
a peer advertising ever-higher versions while another's certified version applies within a
second and a fetch.

#### SR16. "One voice per node" counts uncommitted bindings, and silences by closing (medium)

Attacker: a leader that proposes a `Configuration` entry adding a learner on an honest node, an
entry every follower accepts (`validate_change` counts no voting change for a new learner,
`membership.h:127-130`) and that need never commit.

Evidence: rule 3 applies "when bindings give n two incarnations" (`design:374-375`), and rule
2.2 makes an uncommitted entry a binding (`design:363`). At every replica holding such an
entry, the honest node's committed incarnation becomes "the lower", and its frames are
`forged_source`, which closes the connection (`design:375, 592`) and resets every group the pair
shares (`design:597-598`). The rule's justification, that the older incarnation never speaks,
rests on Track C's D3: a same-node replacement "runs only once the old incarnation's state is
gone from that node" (`track-c.md:324-327`), which an uncommitted entry does not make true.

What they gain: a leader of one group severs an honest node from every replica that holds its
entry, across every group they share, for as long as it resends the entry.

Fix: rule 3 compares committed and vouched bindings only, never uncommitted ones, and frames
from the lower incarnation drop as `superseded`, counted, without closing: Track C's D3 makes
the older incarnation silent before the newer exists, so a drop costs an honest pair nothing,
while a close costs it every shared group. T7 adds: an uncommitted entry naming a learner on
node n, held at replica R, while n's committed incarnation keeps speaking to R; and a late frame
from a replaced incarnation dropped, not closing.

#### SR17. One incarnation of a group per node is stated, relied on, and enforced nowhere (high)

Attacker: a compromised node while it leads a group, the root included.

Evidence: Track C's D3 states "A node holds at most one incarnation of a group"
(`track-c.md:324-327`), D5's vote argument rests on it ("rule 3 lets a node cast at most one of
those", `design:413-415`), and D18 says what every replica now checks: `validate_change` against
the membership before the entry (`design:1008-1012`). `validate_membership` checks incarnation
order, a nonzero node, roles, voter counts, `leaving`, limits, and the witness shapes
(`membership.h:91-116`); `validate_change` checks the counter, reuse, moves, one voting change,
and new members as learners (`membership.h:121-140`); neither compares two members' nodes
(`grep -n node membership.h`: lines 97 and 132 only). So D18's check accepts, one entry at a
time, a membership whose voters share a node, including the proposer's own. The per-node vote
bound does not hold for a leader that proposes such entries; its removal from the directory,
the remedy the threat model names ("until removed", `design:184`), then leaves the group
without a quorum of honest voters. In the root, the voters are the signer epoch
(`design:905-907`), so the same gap turns one root node into a signer majority, the case the
threat model calls a cluster compromise (`design:186`).

Fix: `validate_membership` refuses a membership with two voting members on one node, and allows
two members on one node only in Track C's D3 shape, a `leaving` voter and a learner; D18's check
then carries it at append, at proposal, and at recovery, `known_members()`'s fencing of "one
incarnation on two nodes" (`design:1024-1025`) gains its dual, and T7's mutants add the check
off, caught in T2's harness, beside a leader whose entry adds a second voter on its own node,
refused by every follower and never stored. Detent's fixed groups have no `Configuration`
entries, so nothing changes for them.

#### SR18. What one admin key still buys (low)

Evidence: `Join` and `Activate` need one admin (`design:990-991`): a node of the attacker's own,
which placement then uses, receives real replicated data as a member. `Remove` needs one admin
at 10 a minute (`design:992-993`): 600 nodes an hour, each re-replicating about 891 GB
(`design:333`). The threat model states both (`design:188`); this is a recommendation, not a
defect in what it states. Pending two-admin statements (`design:988-989`) have no cap.

Fix: `Activate` under the two-admin rule, since it is what hands a node data, with `Join`
staying one-admin for bulk registration; `Remove` under the two-admin rule for a node placement
reports live, one admin for a node it already reports dead or draining, or a limit of 10 an
hour; pending statements capped per admin (64). For the owner, through the lead.

### New blocking items

1. SR17 blocks step 5's landing (K4) until `validate_membership` carries the one-voting-member-
   per-node rule and T7 has its mutant; the change is in the same validator D18 adds its call
   to, so it lands with step 5.
2. SR16 blocks step 5's landing until rule 3 compares committed and vouched bindings only and
   drops the lower incarnation's frames without closing, with its T7 cases; the same scope.

Step 5 may start now and may land with both folded in. Steps 1 to 3 and 6 are unaffected. Step
4's hello, `Window`, `Fragment`, and flow control are cleared on the security side (SR5 to SR8
closed), subject to the architect's revision check. Step 8 (D16 and D17's code) waits for SR15's
text and the owner's answer on SR18.

### Conditions

1. C10 (SR15): the hint rules in D16 as stated above, with T2's case, in the next revision.
2. C11 (SR16, SR17): rule 3's two changes and the validator's rule in D5 and D18, in the next
   revision, and in step 5's code before it lands; K4 gains both T7 cases and the mutant.
3. C12 (SR18): the owner's decision on `Activate` and `Remove` under the two-admin rule, and the
   pending cap, recorded in D17 before step 8.

Not run (re-check): nothing compiled, started, fuzzed, or measured; no probe beyond reading and
arithmetic; `pins.sh` not rerun; no Linux or FreeBSD; Detent read only in the cited documents;
the simulator's and Track C's designs read at the lines cited, not run.

## Second re-check

- Date: 2026-10-17.
- Reviewed: `git log 2598de2..HEAD` on `design/peer-transport` (worktree
  `~/code/Kronuz/strata-wt/peer-transport`, HEAD `65d6f5e`, equal to `origin/design/peer-transport`
  after a fetch): `590505e` (ledger, K1 to K9 confirmed, K10 added), `2949082` (ledger),
  `6c49839` (revision 2 of `docs/design/peer-transport.md`, 1,532 lines, with its "Revision 2"
  table; `git diff 90d417e 6c49839` read whole, its removed lines included, and the design is
  unchanged at HEAD), and `65d6f5e` (ledger, ready for re-checks). Against them: the architect's
  re-check (PT-R20 to PT-R26, `peer-transport-design.md:643-926`, read in full), the ledger's Q6,
  Q7, and SR18 entries (`status:130-136`), `calibration.md` (six rows), Track C's D3, D5, D12,
  D13, D15, and D18 (`track-c.md:255-270, 300-312, 320-330, 625-652, 670-676, 730-740, 775-781`
  at strata `4c9ebfd`), `simulator.md:82-134` at `7d2f92b`, and consensus `fc66c0e`, each file
  taken at that commit: `membership.h:30-60, 85-200`, `core.h:296-312, 600-660, 728-760,
  800-892, 1005-1030`, `snapshot_channel.h:25-40, 75-100, 145-160, 205-240`, `worker.h:293-321`,
  `types.h:50-70, 108-135, 178-186, 210-242`. The local consensus clone holds two commits past
  `origin/main` (`c2cbf2e`, `ef30b4f`, unpushed), which touch `core.h` in one hunk at 1138 and no
  transport header, as the design's header says.
- Checked by: reading and arithmetic. The designer's `.scratch/peer-transport-r2/arith.out` was
  read against the design's figures (15.2 and 36.6 MiB of control pools, 50.6 KiB of `Begin`s,
  seven failures and 63 s to the distrust cap, 525 and 1,686 bytes of signatures, 4 MiB of pending
  statements, 1,196 record reads), and each matches. This re-check's own figures are in
  `.scratch/review-peer-transport-security/arith-recheck2.py`, output `arith-recheck2.out` beside
  it, Python 3.14 at nice 19. `quiet check` exited 0 (the host was free); nothing needed a build,
  since none of revision 2 exists as code; `pins.sh` was not rerun, D2's bytes being unchanged.
  - Not run: the last line of this section.
- Citations: `design:N` is line N of revision 2 at `6c49839`; `status:N` of
  `docs/status/peer-transport.md` at `65d6f5e`; `arch:N` of `peer-transport-design.md` at HEAD;
  code at the commits above.

### Verdict

**APPROVE WITH CONDITIONS. SR15 to SR18 close, SR17 (high) among them: `validate_membership`
gains the node rule in Track C's two replacement shapes, D16 counts one signer per node, and what a
compromised leader keeps is stated with Q7. The hold the first re-check put on step 5 lifts on the
design side; K10 still lands SR16's and SR17's rules in step 5's code with T7's cases and mutant.
No new high finding. Four new medium findings bind step 8 and step 4's `Begin` admission, none a
step that may start now: the fork detection PT-R24 asked for cannot happen through D16's fetch as
specified, and the freeze it prescribes would become a denial-of-service lever once it could
(SR19); one admin key grows the directory past strata's 8 MiB fetch bound in 100 minutes and the
cleanup runs a hundred times slower (SR20); with exactly two admins a stolen key cannot be evicted
and no re-founding path exists (SR21); a bound member holds every incoming transfer slot at a node
for as long as it likes, since the `Begin` caps bound pending frames and nothing bounds transfers
per source (SR22). Six low findings (SR23 to SR28) are text. Q7: option 1 now, with three
conditions; the compromised-voter row is right once it names the gain as availability.**

What holds, and reproduces:

- **Hints against k liars** (`design:1080-1107`). A fetch in turn among peers in good standing
  whose hint exceeds the held version, distrust from 1 s doubling to 60 s after a failed or
  timed-out reply, the table after a delta whose result fails S(v), and anti-entropy on a timer:
  the arithmetic reproduces (seven failures, 63 s to the cap), and the turn is the right choice
  over PT-R23's highest-first for the reason given, since every liar whose interval just ended
  would otherwise go first again. The bound "k fetch slots" holds for liars that fail fast; a
  liar that stalls is SR24.
- **Rule 3 on committed and vouched bindings only** (`design:410-424`). Verified against the
  validator: `leaving` is one incarnation (`encode_membership` writes it as one `put64`,
  `membership.h:153`; `validate_membership` requires it to vote, `membership.h:106`), so at most one
  `leaving` member exists in a membership, and a change may set it only beside exactly one
  promotion (`membership.h:139`, "leaving without a promotion"). So a committed membership gives
  one node two voting members only through `Replace`, and nothing else supersedes: not an
  uncommitted entry, not a learner beside a voter (`design:420-424`). The two T7 cases from SR16
  are in the row (`design:1438`). Rule 2.3 dropping instead of closing is right: a node that
  lagged past its own removal campaigns as its old incarnation (Raft's disruptive removed server),
  and closing would reset every shared group at each of its election timeouts (`design:395-402`).
- **The node rule's shapes** (`design:1210-1224`). SR17 asked for "a `leaving` voter and a
  learner", which is not a shape `Replace` produces: the entry that marks the old voter `leaving`
  promotes the learner (`track-c.md:262-264`; `membership.h:139`), so the shapes are a voter with
  its replacing learner before `Replace` and the `leaving` member with its replacement after.
  Revision 2's refinement is right and K10's "one voting member per node" reads correctly as one
  besides the `leaving` one. `decode_membership` runs `validate_membership` (`membership.h:189`),
  so the rule reaches recovery, checkpoints, founding records, and descriptors as D18 says. Fixed
  groups pass (each founding voter its own node and incarnation). The epoch lists the
  non-`leaving` member and refuses a node or digest twice (`design:1030-1035`).
- **Votes.** `grants_` is keyed by incarnation (`core.h:819`), so two voting incarnations on one
  node would count twice; the node rule confines that to the replacement window, where rule 3
  drops the `leaving` one's frames at every replica that committed the `Replace`. In the window
  before commit a compromised node under replacement grants twice, which Raft's model already
  loses to a voter that grants two candidates (`design:464-472`): the design's statement holds.
- **Two admins** (`design:1158-1172`). `Activate` and `Remove` of a node not `drained` take two;
  `Drain` then `Remove` by one admin is possible, by design and said so: the drain moves the
  replicas and loses none, at 10 a minute, 600 nodes and 535 TB of moves an hour (arithmetic),
  which the threat row states (`design:217`). What one key still buys beyond that is SR20 and
  SR21, new findings rather than SR18 reopened.
- **The `Begin` caps** (`design:809-819`). 2 per connection and 8 per node, from one node pool of
  50.6 KiB; the pools total 15.2 MiB at 341 connections and 36.6 MiB at 1,024 (arithmetic
  reproduced). The departure from SR6's per-connection pools costs a dropped `Begin` one
  `rpc_timeout` (`snapshot_channel.h:224-232` answers a repeated `Begin` again), never a close:
  accepted. What the caps do not bound is SR22.
- **Time from the entry** (`design:1144-1155`). Every replica decides from the leader's stamp, so
  replay is deterministic and clocks never diverge the state; the 10-minute skew binds a statement
  to its stamp. What a compromised leader keeps is stated, with one omission (SR23).
- **`DTADMS01`** (`design:1188-1189`). Every length and count is bounded before allocation (64 KiB,
  256 commands, SPKI 1,024, signature 512), the cluster identity stops cross-cluster replay, the
  SPKI inside the statement names the admin and the set's pin confirms it, and the stamp sits
  outside the signed bytes where it belongs. Three specification gaps are SR26.
- **Removed text.** Every line revision 2 removed (`git diff 90d417e 6c49839`) is replaced by a
  stronger rule: the old rule 3's `forged_source` close, the flat `hint_ignored`, the doubling at
  a return, the one-admin `Activate` and `Remove`, and `DTDIRE01` without a first version. Nothing
  was weakened.

### Status

| ID | Status | Evidence in revision 2 |
|---|---|---|
| SR15 | Closed | Distrust 1 s to 60 s, reset on a verified reply (`design:1084-1087`); the turn by node id (`design:1082-1083`); anti-entropy every 60 s whatever the hints (`design:1090-1093`); the table after a failed delta (`design:1087-1089`); the threat row (`design:220`); T2's lying-peer cases (`design:1433`); T10 with a lying neighbor (`design:1441`). One cost is understated for a liar that stalls (SR24, low) |
| SR16 | Closed | Rule 3 on committed memberships and vouches only, a learner never superseding, `superseded` dropped with the connection kept (`design:410-424`); a removed replica's queued frames discarded (`design:432-435`); rule 2.3 dropping (`design:395-402`); the close-or-drop table (`design:654, 657-662`); T7's cases (`design:1438`); T4's `superseded` and `retired_source` oracles (`design:1435`) |
| SR17 | Closed | The node rule in `validate_membership`, carried wherever a membership is decoded (`design:1210-1221`); its two shapes, a refinement of SR17's wording that the validator confirms (`design:1222-1224`); one node, one signer, a majority of distinct nodes (`design:1030-1035`); the limits rows (`design:1295-1296`); the residual in the threat model and the limits (`design:213, 1392-1396`) with Q7 (`design:1470-1490`); T7's three refused shapes, the accepted replacement, and the node-rule mutant (`design:1438`); K10 (`status:46-48`) |
| SR18 | Closed, as decided | Two admins for `Activate` and `Remove` of a node not `drained` (`design:339, 344, 1158-1168`); `drained` in place of "dead or draining", with the reason (`design:1169-1172`); 64 pending statements per admin, 4 MiB (`design:1173-1175`); the threat row (`design:217`); T2's cases (`design:1433`); the decision (`status:136`). Its provenance is SR27 |

Conditions C10 to C12 of the first re-check are closed by the rows above. Of the seven departures
the design's "Status" lists (`design:1514-1532`), items 1, 2, 4, and 5 are accepted above; item 3
is accepted with SR19 and SR24; item 6 is ruled under Q7; item 7 is the owner's document change
and raises nothing here.

### New findings

#### SR19. The fork detection PT-R24 asked for cannot happen through D16's fetch, and the freeze it prescribes would be a denial-of-service lever once it could (medium)

Attacker: a holder of a majority of a retired epoch e's keys, former root voters' keys not yet
destroyed (`design:1042-1050`), with one pinned session: its own at a node whose directory still
pins those keys, or any compromised node's for the freeze.

Evidence:

- The fetch request names "cluster, held version, held epoch, the highest format magics"
  (`design:1077`), and the reply carries "the `DTDIRE01` epochs above the node's" (`design:1067`).
  A node forked onto a forged e + 1 holds epoch number e + 1, so an honest relay sends it no epoch
  of that number, only the newest certificate, whose signers are the real e + 1's members. Against
  the forged epoch they are not members, the certificate fails (`design:1069-1071`), the fetch
  "fails verification", and the honest relay is distrusted (`design:1084-1087`). The node never
  sees the real e + 1; "offered two different epochs with one number" (`design:1047-1048`) never
  happens there. It distrusts its honest peers one by one and keeps the attacker's relay, and an
  honest peer that fetches from it sees only a liar.
- Once the proof can travel, the rule "applies no new version until an operator re-seeds it"
  (`design:1049-1050`) fires at every node shown a second, validly signed e + 1, honest or forked,
  since signatures alone cannot say which is real. A compromised pinned node relaying the forged
  epoch would then freeze every node that fetches from it.

What they gain: today, a forked node that stays forked, undetected by the mechanism the design
names, holding the attacker's pins under any node id; the attacker then authenticates as the
leader of the groups that node hosts, rule 2.1 binds it (the committed membership names the leader's
node and the forged table maps that node to the attacker's key), and its appends raise the
replica's commit index (`core.h:884-888`), a loss of safety at that replica. With the detection
fixed as written, a cluster-wide stop of directory updates from one pinned node plus the retired
keys.

Fix: the request carries the held epoch's digest (the SHA-256 of its `DTDIRE01` bytes); a relay
whose epoch of that number differs answers with its own copy of that epoch and the chain after it,
which is the conflict proof; a node whose certificate names signers outside its epoch of that
number asks for that epoch before it distrusts anyone. On conflict the node keeps the epoch it
verified first in sequence, refuses the other, counts `epoch_conflict`, logs both, and keeps
applying versions under its own chain: no freeze, so the honest majority of nodes cannot be
stopped, and the forked minority, found by the counter, is what the operator re-seeds. A node
whose fetches from three distinct peers in a row fail verification counts `chain_isolated` and
logs it. T2 adds: a node forked by retired keys detects the conflict at its first fetch from an
honest relay, the honest relay at its first fetch from the forked node, and neither stops applying
versions under its own chain.

#### SR20. One admin key grows the directory past the fetch bound, a hundred times faster than it can be cleaned (medium)

Attacker: the holder of one admin key (`design:217`), or an admin's runaway script.

Evidence:

- `Join` takes one admin at 1,000 commands a minute (`design:338, 1166-1167, 1176-1178`), and each
  adds an entry every node holds (`design:366-368`). A `DTDIRT01` table carries 42 bytes per
  one-pin entry (`design:1118`); strata's host data bound is 8 MiB (`design:554, 1079`), which
  bounds a reassembled host data frame (`design:628`) and closes the connection above it
  (`design:651`): 199,727 entries fit (arithmetic). From 100,000 nodes, 99,727 junk joins take
  100 minutes.
- After that no relay can serve the table. A joiner's first fetch is a table (`design:279-280,
  353`), and so is any node's more than 1,024 versions behind (`design:1075-1076`; 262 minutes at
  that rate): each fetches, fails or times out, distrusts its peers in turn, and never starts or
  catches up.
- `Remove` is 10 a minute whatever the node's state (`design:1176-1177`): 6.9 days for one honest
  admin, while the attacker adds 1,000 a minute. The threat row's residual, "a registered node
  that placement never uses" (`design:217`), omits this. Each junk entry is also an authenticated
  identity for a key the attacker holds: a guest at any node, within D10's caps.
- The 8 MiB preset silently caps the cluster at twice the contract, which the limits table does
  not say (`design:1276-1300`).

What they gain: after 100 minutes with one key, no node joins and no node recovers from an
outage longer than 262 minutes, cluster-wide, for days.

Fix: a cap on entries that never left `joining` per admin (a preset, 1,024), beyond which `Join`
and `Reserve` are refused and recorded; `Remove` of a node that is `drained` or never left
`joining` counted under the 1,000-a-minute limit, since it loses nothing; the table reply paged
by node id (the request names the first node id wanted, a page is at most the receiver's bound,
and S(v) is computed over the assembled table), so the host data bound caps no n; or, if paging
is not taken, the n the 8 MiB preset supports stated in the limits table with its mechanism. T2
adds the 1,025th join refused and a 200,000-entry table fetched in pages and verified.

#### SR21. With exactly two admins, a stolen key cannot be evicted, and no re-founding path exists (medium)

Attacker: the holder of one stolen admin key in a two-admin set, the smallest set the two-admin
rule is on for (`design:1158`).

Evidence: `RemoveAdmin` takes two admins (`design:346, 1158-1159`); with admins A and B, removing
B needs B's signature. B keeps its one-admin powers (`Join`, SR20; `Drain` at 600 nodes and 535 TB
of moves an hour, arithmetic; `RemovePin` cutting a re-key short; `Reserve`) and vetoes every
two-admin command: no `Activate`, `AddPin`, `ReplacePin`, `Remove` of a live node, or admin-set
change applies until the operator recovers the key. The set "is founded with the root by the
founding command" (`design:1134-1135`) and nothing names a path to replace it afterwards; D16's
"the operator re-seeds by hand" (`design:1039-1041`) is about epochs. The threat row covers one
admin and two colluding admins (`design:217`), not this.

What they gain: indefinite retention of one-admin powers and a standing veto over the admin
plane.

Fix: `RemoveAdmin{X}` counts statements from admins other than X, so two honest admins of three
evict the third; the design names the operator's re-founding of the admin set as the disaster
path (a root-local command run on a majority of root hosts, or a statement under the seed's
founding key) and recommends at least three admins for a production cluster, which the threat row
says. T2 adds a stolen key evicted by the other two of three, and the re-founding path exercised.

#### SR22. A bound member holds every incoming transfer slot; the `Begin` caps bound pending frames, not transfers per source (medium)

Attacker: one compromised node that is a bound member, voter, learner, or vouched (`design:436-437`
lets vouched sources send transfers), of at least four groups with the victim; an average peer
shares about 31 (arithmetic: 1,000 groups × 2 peers / 64).

Evidence:

- PT-R20's caps bound pending `Begin`s, 2 per connection and 8 per node (`design:809-819`), which
  drop as `busy` and are resent every `rpc_timeout` (`design:844-846`). An admitted `Begin` takes
  one of `snapshot_concurrency` = 4 incoming transfers at the node (`track-c.md:673-674`), the
  channel's single `receiving_` per group (`snapshot_channel.h:224-232`), and the worker's disk
  reservation for the descriptor's `application_bytes` (`worker.h:302`), and holds them until no
  chunk is acknowledged for `snapshot_timeout`, 60 s (`track-c.md:779-781`; `types.h:114`).
- Nothing bounds transfers per source node: the channel answers `Busy` to a further `Begin`
  (`snapshot_channel.h:230`), which stays pending. Four `Begin`s from one member over its one
  bulk connection, two pending at a time and admitted as slots free, fill the node; honest
  leaders' `Begin`s stay pending or drop and resend; after 60 s the attacker begins again.

What they gain: a victim that installs no snapshot from any honest leader for as long as the
attacker likes, so every group in which it lagged past the compaction horizon runs one replica
short; disk reserved for four phantom transfers, up to the bundle maximum each; and the attacker
can rotate victims among its peers.

Fix: at most one incoming transfer per source node while any other source's `Begin` is pending,
else up to `snapshot_concurrency`, so the slots stay work-conserving and one compromised node
holds one; a transfer whose source sends no chunk within the frame deadline (5 s) of `Accepted`
is cancelled, so a silent `Begin` holds a slot for 5 s, not 60. The rule is the transport's at
`Begin` admission (D11) or Track C's D13's; the lead routes it. T4 adds the slot rule's oracle and
a mutant with it off; T6 adds one peer beginning four silent transfers while another's install
completes within 1.5× of the unslowed run.

#### SR23. Time from the entry: the replay residual is understated, and the sequence and expiry rules are unspecified (low)

Attacker: a compromised root member: as a non-leader host it forwards statements to the leader
(`design:1181-1183`) and can keep one; as leader it stamps entries (`design:1144-1155`).

Evidence: "a signed statement cannot be kept and spent later" (`design:1149`) holds against an
honest leader's stamp. A compromised leader proposes a kept statement stamped at the statement's
own signed time, which passes the 10-minute rule at every replica (`design:1148-1149`), since
"stamps need not increase" (`design:1154`). What bounds it is the sequence, and the sequence rule
says only that "a replay or a retry of an applied sequence is a no-op" (`design:1156-1157`):
whether a refused statement advances the admin's last sequence, whether a lower one is refused,
and whether gaps are allowed are unstated; so is whether a pending statement's expiry
(`design:1160-1161`) is judged before a second admin's match in the same entry, which differs when
a compromised leader has kept stamps low for an hour-old pending statement.

What they gain: a one-admin statement the admin signed and the root never answered (`Drain`,
`Join`, `RemovePin`, `Reserve`) applied at a time of the leader's choosing, until that admin's
next statement; little, since an unanswered statement is retried.

Fix: say it: every statement the root sees, applied or refused, advances its admin's last
sequence; one at or below it is a no-op returning the original answer; so a kept statement is
spendable only until the admin's next, which the residual states; expiry is judged before
matching. Do not make stamps monotone: a compromised leader with a stolen admin key could then
stamp an entry years ahead and lock every honest admin out until real time caught up. T2 adds a
kept statement replayed after its admin's next statement as a no-op, and an expired pending
statement unmatched by a second admin's in the same entry.

#### SR24. A stalling liar costs a 5 s fetch slot, not a second, and distrust must survive reconnects (low)

Attacker: a peer that advertises versions it cannot certify and answers slowly or not at all.

Evidence: "k lying peers delay an honest version by at most k fetch slots", "about a second"
(`design:220, 1098-1099`), assumes the liar fails fast. A liar that stalls is distrusted when its
reply "does not arrive within the frame deadline (5 s)" (`design:1085`). Whether the policy
issues the next second's fetch while one is outstanding is unstated; if fetches are serialized,
each stalling liar costs 5 s, twelve cost 60 s (arithmetic), T10's whole bound, whenever their
distrust intervals end together, and T2's "three such peers delay it by three slots"
(`design:1433`) passes with fast-failing liars and misses it. Distrust is "that peer"
(`design:1084`): by node id, so a reconnect with a fresh hello does not clear it; say so.

What they gain: directory updates at a node delayed 5 s per stalling peer per round, with twelve
at the bound.

Fix: the one-a-second rule issues a fetch whether or not the last answered, at most five
outstanding, so a stall costs one slot; or a fetch deadline of its own, 1 s to 2 s, since a reply
is a delta over a held connection; distrust keyed by node id. T2's lying peers stall instead of
failing fast.

#### SR25. Held credit requests need a per-connection cap (low)

Attacker: any authenticated node, as a guest (`design:687-688`), since the hold needs no
membership.

Evidence: a `wanted` from a node the binding does not admit is held, "the newest one per
connection and group" (`design:754-760`), and drops when the vouch is refused or the connection
closes. A `Window` is a node-level control frame that runs at once (`design:820`), its record
count is bounded by the groups shared plus one (`design:622`), so a sender sharing none sends one
record per frame and, frame by frame, holds one request for every hosted group: 1,000 at the
contract, about 48 KB per guest, 24.6 MB at 512 guests (arithmetic), outside the budget. No vouch
is ever asked, since only a control frame naming an incarnation starts one (`design:759-760`), so
nothing but the close releases them, and a guest that keeps sending is never idle.

What they gain: policy memory pinned, modest, from identities that need no membership.

Fix: at most 64 held requests per connection (a preset; a restarted node's new leaders need about
16 per peer, 1,000 groups over 64 peers), the oldest dropped beyond it and counted; the
record-count rule says it bounds hosted groups, so the held path it serves is reachable. T4's
oracle.

#### SR26. `DTADMS01`: what the signature covers, the op bodies, mixed statements, and the role at apply (low)

Evidence:

- Revision 1 listed the magic inside the statement ("magic `DTADMS01`, cluster, the admin's pin",
  `git diff 90d417e 6c49839`); revision 2's table starts at the cluster (`design:1188`) and the
  signature is "over the SHA-256 of the context and what precedes it", so whether the magic is
  signed is unstated. Unsigned, a later `DTADMS02` that renumbers ops could read an old statement's
  bytes under the same context string.
- Each body is "exactly its op's layout" (`design:1188`) and no op layout is given: `Join`'s pin
  and domain (D3 says "a 4-byte domain", `design:297`, while the failure-domain path is
  placement's), `allow_burned`'s place (`design:348-349`), `Reserve`'s expiry and token digest,
  `Enroll`'s. T8 fuzzes a decoder the design does not specify.
- A statement carries up to 256 commands of both tiers (`design:1138, 1158-1161`) and the cost
  table assumes `Remove` and `Join` in one (`design:1260`); whether the two-admin match is per
  command or per statement, and whether a statement's one-admin commands apply while its two-admin
  ones wait, is unstated.
- The listener checks the admin's path and role (`design:1133-1134`); the apply path checks "the
  SPKI against the admin set's pin" (`design:1140`) and should check the role too (`issuer` sends
  only `Enroll`, `design:1001-1003`), since a forwarded statement never met the listener that
  admitted it.

Fix: the magic in the signed bytes, or the context naming it (`strata admin command DTADMS01`);
an op table with each body's bytes; the match per command, a statement's one-admin commands
applied at once and each two-admin command held by itself; the role checked at apply. Before T8's
target is written.

#### SR27. `drained` carries the one-admin `Remove`, and its provenance is unspecified (low)

Attacker: a compromised placement leader (`design:216`) holding or colluding with one admin key.

Evidence: `Remove` of a `drained` node takes one admin (`design:344, 1164-1168`); the mark comes
"as Track D3 reports" (`design:1164-1166`), and the report's path, signer, and freshness are left
to D3. A report any host frame can carry, or one not re-checked at apply, lets a false `drained`
turn one admin's `Remove` into a live node's removal with its replicas.

What they gain: little beyond placement's trust, since a placement leader can move replicas off a
node anyway; a specification item.

Fix: `drained` is set only by a committed root entry the placement group's leader proposes from
its committed assignments, re-checked at the `Remove`'s apply, and cleared by any later assignment
to that node; a row for D3's design in "Changes to other documents".

#### SR28. Smaller points (low)

- A `peer_ranges` cap may equal the total: "an operator who runs k nodes behind one address lists
  that address with a cap of 2k, ... still under the 16 in all" (`design:717-721`) asks the
  operator; a cap of 16 lets one source hold every slot (SR7). The policy refuses a range cap
  above 14, or raises the total with it.
- `vouched{group, node, incarnation, answer}` (`design:864`) must match an in-flight question's
  triple exactly; an answer for another triple is ignored and counted, so a witness never binds
  what the policy did not ask.
- Two exponential backoffs now live in the policy, dials (`design:723-724`) and hint distrust
  (`design:1084-1087`): calibration row 1; one `Backoff{floor, cap}` serves both when steps 2 and
  8 are coded.
- For the architect: a node recovering 1,000 groups receives their leaders' `Begin` resends at
  1,000 × 6,480 bytes per 500 ms, 13 MB/s or 10% of 1 Gb/s (arithmetic), for the whole catch-up;
  a `busy` answer carrying a retry-after, or a resend interval that backs off, would spare it.

### Q7, ruled

Option 1 now, as the designer recommends, with three conditions.

What a compromised leader gains under option 1, named: it commits phantom promotions and honest
removals, each D18-valid. It needs honest acknowledgments for every commit, since a phantom's
frames would have to come from the node the entry names (rule 2.1, `design:387-388`), so with
rf = 3 it can leave the group with no live quorum and stop it, which it could also do by stalling.
The difference is recovery: an election recovers a stall, and only an operator recovers a
vandalized membership, from a replica's checkpoint. Safety toward clients is already the lying
leader's, which acknowledges what it likes. So the residual is one of availability and operator
effort, which the threat row and the limits should say in those words (`design:213, 1392-1396`).
Option 2 as written covers adds and promotions and not removals (`design:1480-1486`), so it would
bound the phantom and leave the one-at-a-time removal; a check against placement's records that
covers all three confines a compromised leader to Raft's crash-only model for membership, at a
record read per follower per change (1,196 per lost node, arithmetic reproduced) and an
asynchronous check in the append path.

Conditions: (a) the residual stated as availability and operator recovery in the next edit; (b)
Track D3's design answers option 2 for adds, promotions, and removals, with its reason, before
placement drives any shard group's membership changes, and carries the detection option 1 relies
on as a mechanism with a bound: every assignment holder compares the group's committed membership
with its record at each reconciliation and raises `membership_divergence`; (c) T7 adds, once D3
exists, a phantom promotion committed and flagged within one reconciliation interval. Nothing
before step 8 waits on it, and fixed groups, Detent's, have no membership changes.

### Calibration checks applied

- Row 1: two exponential backoffs in one class (SR28); `lru` is reused as before; the turn, the
  distrust table, and the held requests have no catalog counterpart.
- Row 2: the four commits are docs-only and skip CI by rule; the code commits cited are
  `fc66c0e` and reactor `e19afd9`, green in the first review.
- Row 3: no row cites a measurement as a baseline; T10's 60 s and the 1.5× in T6 are targets.
- Row 4: every ruling addressed to this revision, PT-R20 to PT-R26 and SR15 to SR18, is answered
  where the "Revision 2" table says (`design:35-48`); the architect's C6 to C9 and this review's
  C10 to C12 are each present.
- Row 5: HEAD equals `origin/design/peer-transport`; the local consensus clone holds two unpushed
  commits (`c2cbf2e`, `ef30b4f`), named above and read around.
- Row 6: SR20 recommends paging over a cap, so no limit is lowered; the finding is that the 8 MiB
  preset implies an n ceiling the limits table does not name.

### New blocking items

1. SR22 blocks step 4's `Begin` admission code until the per-source slot rule and the first-chunk
   deadline are in D11 or carried to Track C's D13, with T4's and T6's cases; the frame header,
   hello, and kinds 1 to 8 and 13 to 18 may still be drafted, and SR25's cap is small enough to
   land with the flow-control code.
2. SR19, SR20, SR21, SR23, SR26, and SR27 bind step 8 (strata's D16 and D17) and D3's design where
   named; none blocks steps 1, 2, 3, 5, 6, or 7.

Step 5 is cleared on the security side: SR16's and SR17's rules are in D5 and D18 as the design
states, and K10 lands them in its code with T7's cases and the node-rule mutant. Steps 1 to 3
and 6 are unaffected, as before.

### Conditions

1. C13 (SR19): the epoch digest in the request, the conflict reply, no freeze, and
   `chain_isolated`, with T2's cases, in the design before step 8.
2. C14 (SR20): the `joining` cap, the lossless `Remove` under the 1,000-a-minute limit, and paged
   tables or a stated n ceiling, in D16, D17, and the limits table before step 8.
3. C15 (SR21): `RemoveAdmin` counting admins other than the removed, the re-founding path, and the
   threat row's sentence, before step 8.
4. C16 (SR22): the per-source transfer slot rule and the first-chunk deadline, in D11 or Track C's
   D13, with T4's mutant and T6's case, before step 4's flow-control code.
5. C17 (SR23 to SR28): the sequence and expiry rules; overlapping fetches or a fetch deadline, and
   distrust by node id; the held-request cap (before step 4's flow-control code); `DTADMS01`'s
   magic, op table, match granularity, and role at apply (before T8's target); `drained`'s
   provenance as D3's row; the range-cap bound and `vouched` matching; in the next revision or the
   first edit before step 8.
6. C18 (Q7): conditions (a) to (c) above: (a) in the next edit, (b) in Track D3's design, (c) in
   T7 when D3 exists.

Not run (second re-check): nothing compiled, started, fuzzed, or measured; the host was free
(`quiet check` exited 0) but no probe needed a build, since none of revision 2 exists as code;
`pins.sh` not rerun, D2's bytes being unchanged; the designer's `arith.py` read and matched, not
rerun; this re-check's arithmetic in Python only; no socket crossed, so the hint, `Begin`, and
admin paths are judged from the design's text and the code's current form; no Linux or FreeBSD;
Detent read only in the documents the design cites; Track C, the simulator, and the PKI designs
read at the lines cited, not run.
