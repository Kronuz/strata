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
