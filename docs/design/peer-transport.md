# Peer transport: identity, versions, and flow control

PLAN's owner decisions SYS-2 and SYS-3 (`docs/PLAN.md:26-33`) put peer identity and wire
versions into one peer transport design, Track C's D16 extended, and every design that names
peer identity defers to it: Track C, the simulator's network, Xapiand's deferred transport (its
D2 to D8), and the PKI helper. This is that design. It covers the identity check, the node
directory and where it lives, version negotiation and what one version back promises,
multiplexing and flow control (Track C's C4), the send policy as a sans-I/O class with the
keepalive failure detector (the simulator's Q3), and the admission rule that keeps a forged vote
out of every group (SYS-14). The code lands in `Kronuz/consensus` (`consensus::transport`), with
two additive options in `reactor`; strata supplies the directory's home; Xapiand adopts the
transport at Phase 7b.

Revision 1 folds in the architect's preliminary review (`docs/reviews/peer-transport-design.md`,
PT-R1 to PT-R19) and the security reviewer's (`docs/reviews/peer-transport-security.md`, SR1 to
SR14), both APPROVE WITH CONDITIONS, and the owner's decisions on Q1 to Q5
(`docs/status/peer-transport.md`). Every finding is closed where its ID appears below.

Read at: consensus `fc66c0e` (`origin/main`; the local `ef30b4f` changes no transport header and
`core.h` only at :1141, so every line cited holds there too), reactor `e19afd9` (consensus's pin,
equal to `origin`), strata `main` `c5b571a` (this branch's base; PLAN lines are its),
`design/simulator` `7d2f92b` (its design unchanged since `0054b30`), Xapiand `master` `7e8cccb7d`
and `design/pki` `bfc11c50c` (local only; its design unchanged since `d05ed6bab`),
`Kronuz/lru-cache` `70b9663`, and Detent `deae42d`, read only. `simulator.md` and `pki.md` are
those branches' designs. Nothing here is built or measured: every figure is the cited code's or
design's, arithmetic, or a labeled estimate. The arithmetic is
`~/.scratch/peer-transport-r1/arith.py`, its output `arith.out` beside it.

## Summary

- **Identity.** A peer authenticates by the SHA-256 of its leaf's SubjectPublicKeyInfo (SPKI),
  looked up in a node directory after the chain verifies and before the hello is read.
  Certificate pins stay valid beside it, so Detent's configuration works unchanged, and a renewal
  that keeps the key changes no pin.
- **The directory.** `consensus` holds only authentication: node ids from a counter and one or
  two pins each. Its home is strata's root (Q1), whose versions a majority of the root's
  voters sign and any node relays, so no single member's answer is trusted. Addresses are hints
  in an address book beside it; placement state, enrollment reservations, and the admin set stay
  in the root, outside `consensus`.
- **Admission.** Every frame names its group and its source and destination incarnations. A
  committed membership binds an incarnation to its node for good; an unknown one is admitted only
  when the group's parent authority vouches for that exact incarnation on that node; a node
  speaks as one incarnation per group. Every replica, not just the leader, checks each membership
  change, so no leader can move another node's incarnation onto itself.
- **Versions.** Each binary speaks wire versions N − 1 and N; a session runs at the highest both
  speak. Version 2 is the first that negotiates, and Detent leaves version 1 by one coordinated
  restart (Q4).
- **Flow control.** One connection per node pair and lane carries every group. Credit is per
  connection and group, granted after the hello within an incoming budget split into an initial
  pool, a growth pool, and a reserve that always fits the largest frame the node accepts. Control
  frames go first and large frames go as fragments, so a control frame waits behind about 3 ms
  at 1 Gb/s.
- **One policy.** A sans-I/O class decides everything the transport decides. `Peers` drives it
  over sockets and the simulator drives it over its network.
- **Admins.** Directory commands are statements an admin signs, which every root replica
  verifies before applying; changing an existing node's pins or the admin set takes two admins.

## Changes from revision 0

| ID | Change | Where |
|---|---|---|
| PT-R1 | The wire fixes no window and there is no connection window: credit is per connection and group, granted after the hello from an initial pool of a quarter of the budget (63.0 MiB at the contract, arithmetic) | D11; cost table; T4 |
| PT-R2 | `Window` is a node-level frame of records, each naming one group (zero for node-level data); credit returns at half a window with no timer; its rate is a cost row (about 204 frames a second at the simulator's median write rate) | D9; D11; cost table |
| PT-R3 | A node refuses to start when its largest accepted frame exceeds half its budget; a blocked request grows its window or waits in FIFO order for a reserve that growth never takes | D11; T4 |
| PT-R4 | The hello's fixed part is 210 bytes and freezes whole; the magic stays `DTHL0002` in later versions; endpoint families 4 and 6; extension tags strictly increasing; a hello naming the local node refused; the acceptor's refusal hello; `b7073bb` counts and logs nothing | D6; D8; T3 |
| PT-R5 | Kinds 19 to 255 reserved and refused; host frames node-level and outside the binding; fragments checked on the first fragment, one in progress per group and connection, data kinds only, each inner kind bounded; `Credit` a protocol error; C2's bodies before the freeze; what closes and what drops | D9 |
| PT-R6 | Pin kinds 1 and 2; `consensus`'s `Directory` holds authentication only; addresses in an address book with each phase's source; the directory's formats specified, owned by strata; enrollment's reservations in the root; ids from a counter, no tombstones | D2; D3; D4; D15; D16 |
| PT-R7 | Directory versions certified by a majority of the root's voters, relayed by any node, verified at each node against a signer chain from the seed; a threat row for a compromised serving member | D16; D4; threat model; T2; T10 |
| PT-R8 | No new `PeerOptions::strict` (`tls.strict` serves); `PeerConfiguration` untouched; closing at notAfter opt-in (`close_at_expiry`); the preset builds a `Directory` and an address book | D3; D12; D14; library changes; for Detent |
| PT-R9 | A characterization test in `consensus` before the relocation, rerun unchanged after it and after every step that moves behavior out of `Peers`; Detent's transport suite at every additive step's commit | D12; T5; T9; first steps |
| PT-R10 | Every replica checks each `Configuration` entry against the membership before it, at append and at recovery; committed bindings decide and uncommitted ones only admit; one voice per node; the wider hole stated | D18; D5; starting point; T7 |
| PT-R11 | The witness answers by incarnation from the assignment that placed the replica, else a linearizable read through the parent's leader, and its answer expires with the record's version; leader-only kinds | D5; T7 |
| PT-R12 | Query fan-out over held connections along a stable tree, never contacts; acceptors keep guests in an LRU; handshakes at query load in the cost table; the policy's executor | D10; D12; cost table; T12 |
| PT-R13 | Two handshakes per source, a 2 s pre-authentication deadline, probes over held monitor connections; the slot residual in the threat model | D10; threat model; T13 |
| PT-R14 | "Every node runs N" defined over the directory; a straggler's refusal says to upgrade; directory formats cross versions by magic; feature gating tested; Detent's candidate as Q4's condition; the simulator's lanes and shared directory | D6; D7; D16; T3; changes to other documents |
| PT-R15 | `ReplacePin` for an exposed key on a trusted machine, with no overlap and no re-replication; removal stays for a compromised one | D4 |
| PT-R16 | `Kronuz/lru-cache`'s `lru` for contacts and guests; `Flow::frame_bytes` kept only if T6 measures a gain; the incoming budget joins Track C's node pools on `ledger` | D10; D11 |
| PT-R17 | T1 to T12 changed as the review lists; T13 for D10's bounds; T6 with a bounded queue and dummynet | Gate rows |
| PT-R18 | The text owed to Track C, DISTRIBUTION, transport-security, and the simulator, as checked; a joiner checks itself after its first fetch; two budgets that share a number | Changes to other documents; D2; D11 |
| PT-R19 | Enrollment's owner, trigger, and root side (`Reserve`, `Enroll`) | D15; D4 |
| SR1 | `Vouch{group, node, incarnation}`; an unknown incarnation below the committed counter is forged; a node speaks only as its highest incarnation of a group; T7's cases; the residual stated. Clause 1's "never vouched for another" becomes rule 3, so a same-node replacement still reaches a lagging replica | D5; T7; limits of this design |
| SR2 | As PT-R7; the witness's source stated; T2's forged-delta and replay cases | D16; D5; T2; threat model |
| SR3 | The admin path: a listener on root hosts, mutual TLS, admins pinned, signed numbered idempotent statements that every root replica verifies, two admins for pin and admin-set changes, rate limits, an audit; the issuer an admin-class principal; threat rows | D17; D15; D4; threat model |
| SR4 | Admission by the limits a quorum of voters accepts, so no minority lowers a group's commands; a member below is held back and counted | D8; T7 |
| SR5 | A per-peer share of growth; `wanted` bounded by lane and role; reserve and fragment stalls close; windows above 2^62 a violation | D11; T4; T6 |
| SR6 | Length and header read before the body; per-kind control caps; per-connection control pools; transfer control split from chunks, so every kind's class is known from the header | D9; D11; T8 |
| SR7 | Two handshakes per source, sixteen in all, 2 s each; the operator's address ranges; dialing out while the acceptor is full; the residual with its figures | D10; threat model; T13 |
| SR8 | Value rules per decoder; the 1 MiB wire maximum and each kind's bound; the directory's formats | D8; D9; D16 |
| SR9 | Fetches at most once a second whatever the hints, verified locally; burned digests; the partition residual; Q6 asks for a staleness deadline | D4; D16; Q6; threat model |
| SR10 | Reload generations re-verify every older session, straddling and resumed ones included; key files read once through a descriptor into reactor's in-memory PEM | D14; T11 |
| SR11 | Exponential backoff for strata and Xapiand, the flat one kept in Detent's preset; endpoints only for their sender, within the operator's ranges | D10; D3; T4 |
| SR12 | TLS 1.3 minimum for reactor's `Peer` role under `strict`; the TLS 1.2 residual | D14; threat model; T1 |
| SR13 | Rows for local files, the CA holder, admins, root and placement members, and dependencies; `handshaken` carries digests only and the hello is read after `Admit`; the OpenSSL version logged | Threat model; D12; D14 |
| SR14 | T1's notes: the re-encoded SPKI, explicit parameters refused, proof of possession for a CA-minted leaf; T3's chosen-version refusals | D2; T1; T3 |
| Q1 to Q5 | Recorded as decided, each with its reviewers' conditions | Open questions |

## What it answers, and what it changes

### Requirements carried

| Requirement | From | Answered by |
|---|---|---|
| One identity model; the directory and its home; the discovery transport; enrollment; the sans-I/O send policy | SYS-2 (`system-2026-10-17.md:193-246`) | D2, D3, D4, D16, D1 with Q3, D15, D12 |
| A version range in place of the hello's profile equality | SYS-3 (`:248-286`) | D6, D7, D8 |
| Lengths and counts bounded before allocation; identities a simulation can supply; no dependence on reactor's UDP loop | SYS-12 (`:514-541`); its recovery bound stays Track C's (`:521-525`) | D9 and D16 for wire decoders, D12, D1 |
| No forged vote | SYS-14 (`:560-585`); the owner's ruling (`PLAN.md:45-46`) | D5, D18, D1 |
| The dialer checks the node it meant; the acceptor takes identity from the certificate; one key names one node | TS-R2 | D2, D3 |
| Replay across restarts; the sealed frame's header, nonce, and size | TS-R3, TS-R4 | No datagram path exists (D1; Q3 decided); TLS sessions are fresh; boot identities mark restarts (D8) |
| Evicting a compromised node; explicit bootstrap; enrollment; the authority off data nodes | TS-R5 to TS-R8 | D4, D15; the PKI helper's D8 and D13 |
| Reload without restart, trust bundles, revoked sessions | TS-R12 | D14 |
| No half-secured cluster | TS-R13 | D1 |
| The certificate profile; the SPKI digest's exact bytes | PKI D5, D10, SR12 | D2 |
| Enrollment, decided to belong here | PKI Q6 (the `pki` ledger, 2026-10-17) | D15, with Q5 decided |
| `Peers`' queues, drop rule, control-first order, backoff, and the keepalive detector as one sans-I/O class | Simulator Q3 (`simulator.md:762-773`), SIM-R6 | D12, D13 |
| Connections in order or not at all; no partition oracle on send | SIM-R7 (`simulator.md:233-247`) | D11, D12 |
| A key identity per node, refusals by pin and by version gap | Simulator D4 (`simulator.md:249-261`) | D2, D3, D6 |
| Per-group receive credit; a group's appends on one lane; authentication at scale | Track C's R12 | D5, D10, D11 |
| Control frames ahead of data, a frame cap, bounded unsent bytes, and T7's latency row | Track C's R19 | D11, T6 |
| The hello carries node limits | `track-c.md:42-45` | D8 |
| SPKI pins beside certificate pins; N and N − 1 | Detent's `SHARED_LIBRARY_DECISIONS.md`, sections 1 and 2 | D2, D6, D7 |

### Changes to other documents

Each is the owning document's to make; the lead applies the ones on `main` (PT-R18).

| Document | Change |
|---|---|
| `track-c.md` D1 (`:212-215`) | The directory's home is the root (D4); it holds pins only, SPKI or certificate (D2, D3); addresses leave it for an address book |
| `track-c.md` `:42-45` | "The transport's hello (`DTPROF02`)": the hello is `DTHL0002` (D8), which carries the limits; `DTPROF02` is its profile |
| `track-c.md` D2 (`:138-140`, `:148-150`) | `DTPROF02` leaves out the wire version (D6); a leader admits within the limits a quorum of voters accepts, not every reported member's (D8, SR4) |
| `track-c.md` D5 (`:305-309`) and the site table (`:236, 244, 251`) | Any incarnation D5's binding admits; every replica checks membership changes (D18) |
| `track-c.md` D12 (`:629-650`) | A coalesced record carries its source incarnation; coalesced records, `Wake`, `LeaderGone`, and `TimeoutNow` act only from the replica's known leader (D5); records bounded by the groups shared (D9); the detector is D13 here |
| `track-c.md` D16 (`:754-757`), D18 (`:796-797`), the format table (`:182-200`) | Credit per connection and group only, no connection window (D11); add `DTHL0002`; kinds 16 and 17 replace the snapshot chunk row (`:199`); `Ping` and `Pong` move from C2 to this design (`:196`); window updates are kind 15's records (`:198`) |
| `DISTRIBUTION.md:22, 44, 49, 71, 129-135` | The root holds the node table and the signer epochs; the transport row names this design; open decision 2 is decided (Q3); cluster-state memory gains the directory's 64 bytes per member |
| `simulator.md:134, 233-242, 249-261, 369, 762-773` | Tier A shares one directory with a version per node (8 bytes each, not 6.4 MB); the transport's lanes are separate connections that reset apart, plus held, contact, and guest connections; refusals by pin and version are the policy's; Q3's seam is D12 |
| Xapiand `transport-security.md:118-134, 153-154` | D2 is answered from Phase 7b, and node-to-node legs stay in clear until then (`:32-34`); D3 is answered by Q3's option 1; D4 and D8 are answered; D5 in part: TS-R5 by removal, TS-R6 by the seed, TS-R7 by the enrollment design, TS-R8 by the PKI helper's D8 and D13; answer 2 ("leaf pins: no") is superseded for peers by SYS-2 |
| `pki.md:255-271` | Enrollment is D15's own design; reload is D14; pins change without a restart through D4 and D16 |
| Detent `LIBRARY_BOUNDARIES.md:187`, `AUTHORITY_TRANSPORT.md:9, 47` | `DTHL0001` and `DTC1` change only with version 2; the profile's role (D6); the configuration as a static directory (D3) |
| Track D2, D3, and D5's designs, when written | D3: placement records carry each replica's incarnation (D5), and the root runs no witness (D16). D2: probes ride held connections in a fixed monitoring topology (D10). D5: fan-out rides held connections along a stable tree (D10) |

## Starting point

| Fact | Where |
|---|---|
| One `Peers` per group: `PeerConfiguration` holds one `FixedConfiguration`, its limits, the group's cluster, group, and epoch identities, and the profile | `transport/peers.h:14-19, 27-38` |
| A peer's identity is the SHA-256 of its leaf's DER (`X509_digest`): one pin per voter, unique, the local leaf matching its own pin; the verified leaf is compared with every pin by `CRYPTO_memcmp` | `transport/tls.h:12-20, 21-36, 45-66` |
| Chains verify under reactor's `Peer` role with a TLS 1.2 floor, no renegotiation or early data, and a 64 KiB certificate-list cap; `strict` keeps TLS 1.2 to forward-secret AEAD suites and turns resumption off; `PeerOptions::tls` is that `Options` | reactor `reactor_tls.h:42-46, 56-86`; `peers.h:28, 65` |
| The hello `DTHL0001` is 120 bytes: node, lane, cluster, group, epoch, a fresh connection id, and the profile; every field but the connection id must be equal, and a hello naming the local node is refused | `transport/hello.h:11-17, 24, 48-54` |
| `DTPROF01` hashes the application's bytes, the cluster and configuration ids, the sorted voters, and nine consensus limits | `transport/profile.h:7-37` |
| Only the lower node id dials; the dialer requires the node it dialed; an acceptor refuses a dialer whose id is not lower, and a second connection per peer and lane | `transport/peers.h:90-94, 343-346` |
| Authentication, then the pin, then the hello: the pin is checked before the hello is read | `transport/session.h:124-135` |
| A frame is a 4-byte length bounded before allocation and charged to one node-wide byte budget, read under one deadline from its first byte; a frame that cannot be charged closes its session | `transport/session.h:12-32, 149-171`; `peers.h:301, 414-417` |
| The codec writes `DTC2` with tags 1 to 8 and no group header, bounding counts and lengths before it allocates | `transport/codec.h:22, 33-50, 121-122, 163-165` |
| Send queues per connection: 4 control frames, appends with entries apart (4 frames, 4 MiB), control first; a full queue drops the frame and counts it | `transport/peers.h:33-36, 125-156, 422-428` |
| The reader holds one decoded event per connection until the Worker admits it, so a busy group stops its connection; the owner pass ends at the first `Busy` | `transport/peers.h:180-182, 368-394` |
| Time comes from `steady_clock` inline; the reconnect backoff is `100 + (peer·17 + lane·31 + reconnects·13) mod 100` ms | `transport/peers.h:171, 190, 329-331` |
| A refused peer at Detent's pin is counted and not logged | `transport/peers.h:342-345` at `b7073bb` |
| The core admits a `Receive` from any nonzero incarnation but its own; terms decide | `core.h:733-736` |
| Only a proposing leader runs `validate_change`; followers and recovery decode `Configuration` entries structurally | `core.h:302-308, 613`; `membership.h:121-140, 161-194` |
| An accepted append raises the follower's commit index to what it verified, and a conflict with the committed prefix throws, which fences the replica | `core.h:884-888, 843-846`; `worker.h:667-676` |
| A leader admits a command only within the minimum of every reported member's limits; above it, `TooLarge` | `core.h:314-332, 700` |
| An incarnation is never reused and never moves to another node, along the log a leader proposes | `membership.h:128, 132` |
| `PeerLimits` reports a member's node limits; a member nobody reported is taken to match the leader's node | `types.h:214-217` |
| A transfer frame is at most 65,688 bytes; a snapshot descriptor at most 6,312 | `snapshot_transfer.h:37`; `snapshot.h:53` |
| Detent pins consensus at `b7073bb` (`DTC1`, `DTHL0001`, certificate pins), builds `PeerConfiguration` by position and `PeerOptions` by name, sets no `strict`, and reports no `PeerLimits` | Detent `cmake/dependencies.json`; `src/detent_authority/peers.hh:11-17`; `service.cc:299, 305-343` |
| In consensus, `Peers` has one test, an instantiation over a host whose members abort; Detent keeps the tests that run it | `test/transport/peers_test.cc:9-16`; Detent `LIBRARY_BOUNDARIES.md:251` |

With one group per connection, the transport hands the core the authenticated node as the
sender (`peers.h:378-379`), so a fixed voter's identity holds. Once frames name their
incarnations (D9), the transport has to bind them. Otherwise any authenticated node can send a
higher-term append into any group on any node it reaches (`core.h:733-736`): one that passes log
matching raises the follower's commit index to what it just appended, so the follower applies
entries no leader committed (`core.h:884-888`), and one that conflicts fences the replica
(`core.h:843-846`). D5 is that binding, and D18 the check it rests on (PT-R10).

## Threat model

| Actor | Capability | Stopped by | Residual |
|---|---|---|---|
| On the network, holding no key | Read, tamper, replay, inject, impersonate; hold handshake slots | Both chains verified to the cluster bundle and the leaf pinned before the hello is read (D2, D12); TLS 1.3 only under `strict` (D14); fresh connection identities; no plaintext or datagram path (D1); the operator's address ranges, 2 handshakes in flight per source and 16 in all, 2 s each (D10) | Traffic analysis. Under TLS 1.2 (Detent, `strict` off) both chains travel in clear and name both nodes (SR12). Holding a node's 16 slots takes 8 sources and 8 new connections a second (arithmetic); existing sessions and held monitor connections survive, and the node keeps dialing out |
| Holder of a leaf the bundle verifies and the directory does not pin: a `client` leaf, a retired node's | Present it as a node | Pins (D2), checked before any decoder runs | One handshake slot for at most 2 s |
| Holder of the cluster CA key | Mint leaves | A leaf for its own key matches no pin; one for a node's key needs that node's private key, since the handshake proves possession (SR14); admins are pinned too (D17) | None for peers; what API clients trust is the PKI helper's (its D8) |
| Holder of a removed node's key | Reconnect | Removal deletes the entry and burns its digests (D4); sessions close when the removal applies (D14) | Until each node applies it, within the dissemination window T10 measures; indefinitely at a node that reaches no relay (SR9; Q6) |
| One compromised node, a voter | Act as itself; speak as another node's incarnation; inject into groups not placed on it; move incarnations as leader; cap a group's commands | Binding (D5); every replica checks membership changes (D18); admission by the limits a quorum accepts (D8) | Raft assumes members that crash but never lie: in the groups placed on it, it can vote, append, fence replicas, and stall the group as leader, until removed |
| One compromised node, a learner, a witness, or a stale former host (SR1) | Speak as an incarnation the replica has not seen | An unknown incarnation below the committed counter is forged; above it, only the incarnation the parent's record places on that node; one voice per node (D5) | A replica behind a whole add-and-remove cycle while the record lags admits the removed node's real former incarnation, and an append from it can raise that replica's commit index (`core.h:884-888`): a residual of safety at that replica, not only of availability (PT-R10) |
| A compromised root member, voter or learner (PT-R7, SR2) | Serve a forged directory; withhold | A version applies only with a majority of the signer epoch's signatures over the digest the node computes itself; any node relays (D16); root replicas apply no unsigned admin statement (D17) | Withheld signatures or relays delay changes. A majority of the root's voters compromised is a cluster compromise |
| A compromised placement member (SR2, PT-R11) | Vouch for any node | Vouches come from the assignment that placed the replica or a linearizable read through the parent's leader, by incarnation, expiring with the record (D5) | A compromised parent leader vouches across its range, about 22,700 groups (`simulator.md:86`): the trust placement already holds, since it decides those groups' memberships |
| Holder of one admin key (SR3) | Register, remove, drain nodes; change pins; change the admin set | Signed, numbered statements that every root replica verifies, in an audit (D17); pin changes on existing nodes and admin-set changes need two admins; `Remove` and `Drain` at most 10 a minute | A node of its own, or evictions at the rate limit, each logged. In a one-admin cluster, or with two colluding admins, the admins are the cluster's identity authority |
| On the network, at the admin listener (SR3) | Reach it | Bound only where the operator says, on root hosts; mutual TLS with admins pinned; D10's handshake caps | Its handshake slots, as for peers |
| A peer that lies in its hello (SR4, SR11) | Limits, versions, boot identity, endpoints | Its limits hold back only itself (D8); a floor of N − 1 (D6); endpoints only its own and within the operator's ranges, with exponential backoff (D10) | A false restart report wakes its own groups (Track C's D12); dials inside the operator's ranges |
| A peer that floods (SR5, SR6, SR8) | Exhaust memory, starve groups, close other sessions | Credit per connection and group within a per-peer share and a FIFO reserve; the header read first, per-kind caps, per-connection control pools, value rules, stall closes (D9, D11) | CPU for the frames it sends within those bounds |
| A forger of addresses (gossip, configuration) | Point node n elsewhere | The pin binds the node whatever the address (D2, TS-R2) | A failed dial |
| Replay or reordering across reconnects | Resend old frames | TLS per session; a reset discards unadmitted frames and credit (D11); random boot identities | None |
| During a reload (SR10) | Keep a session under a dropped CA | Generations: every session of an older context is re-verified, straddling and resumed ones included (D14) | None |
| Local file access on a node (SR13) | Key, leaf, bundle, cached directory, seed | SYS-13's mode rule on the key, read once through a descriptor (D14); the cached directory re-verified against its certificate at load (D16); the seed read only at the first start | Whoever writes a node's key file is that node |
| A compromised dependency (SR13) | Change behavior under the transport | Kronuz libraries pinned by commit; the OpenSSL version logged at startup (D14) | OpenSSL floats with the platform; otherwise outside the model |
| Operator error | A duplicate pin, a reused id, a re-key out of order | The invariants (D3), ids from the counter, burned digests (D4), the overlap rule, two admins for pin changes | A pin two admins add to the wrong node authenticates as that node |
| Version downgrade | Push a session to an older version | Hellos inside TLS after the pin; a floor of N − 1 (D6) | A compromised node can offer only N − 1, which costs features only |

## Decisions

### D1. One transport for every node-to-node message

- Everything nodes send each other rides `consensus::transport` sessions: Raft for every group
  kind (the root, placement groups, shard groups, and Detent's fixed groups), snapshot
  transfers, the transport's control frames, and host frames (D9). Host frames carry Track D2's
  membership gossip (Q3, decided), Track D5's shard RPC, the directory's fetches (D16), the
  witness's reads (D5), and the admin statements a root host forwards to its leader (D17).
  Nothing travels in plaintext or as a datagram, and no second channel authenticates nodes. The
  admin and enrollment listeners (D17, D15) serve principals that are not nodes.
- A half-secured cluster (TS-R13) therefore cannot be configured. The replay and nonce rules
  that sealed datagrams needed (TS-R3, TS-R4) do not arise, since each TLS session has its own
  keys and keeps its frames in order; that holds because Q3 chose option 1, and a datagram path
  would bring them back (PT-R18). The transport needs nothing from reactor's UDP loop (SYS-12).
- The sans-I/O policy decides (D12) and drivers do I/O: `Peers` over Asio and reactor's TLS,
  and the simulator's adapter over its network. The core gains one query and one check (D5,
  D18).
- Hosts: Detent, one fixed group per process as today; strata, about 1,000 groups per node;
  Xapiand, strata's groups from Phase 7b.

### D2. Identity: SPKI pins beside certificate pins (SYS-2)

- A pin is a kind byte and a SHA-256 digest: kind 1, `Certificate`, hashes the leaf's DER as
  `certificate_fingerprint` does (`tls.h:12-20`); kind 2, `PublicKey`, hashes its
  SubjectPublicKeyInfo DER, `i2d_X509_PUBKEY(X509_get_X509_PUBKEY(leaf))`, the digest the PKI
  helper prints (`pki.md:598-606`). Every other kind byte is refused (PT-R6). The call
  re-encodes the parsed structure, so it equals the SPKI bytes inside any DER certificate; the
  security review's probe matched uncompressed, compressed, and explicit-parameter keys under
  OpenSSL 3.6.5 and 4.0.3 (SR14). `X509_pubkey_digest` hashes only the key's bit string (SR12 of
  the PKI review), so nothing uses it.
- The chain verifies first (`reactor_tls.h:83-85`), so a pinned key under a chain outside the
  bundle is refused, and a leaf with explicit curve parameters fails there (OpenSSL's error 94
  in the same probe) and stays refused (SR14). A leaf the cluster CA mints for a node's public
  key verifies and matches the pin; the handshake's proof of possession is what refuses a holder
  without the private key, and OpenSSL refuses to configure a mismatched key locally
  (`reactor_tls.h:79`).
- Either node dials (D10), so a peer leaf carries both EKUs, the PKI helper's `peer` role
  (`pki.md:406`). OpenSSL checks the client purpose when a node accepts and the server purpose
  when it dials, so a `server`-only or `client`-only leaf is refused in the direction it lacks
  (PT-R17).
- Authentication computes both digests of the verified leaf and looks each up in the directory
  (D3) before the hello is read (D12, SR13). One match names the node; matches under two
  different node ids refuse the session as ambiguous; no match refuses it.
- The dialer refuses a session whose node is not the one it dialed (`peers.h:344`). The
  acceptor takes the node from the pin, never from the address; the hello's node must equal it
  and differ from the local node (`hello.h:50`). Peers check no host name, so the CN fallback the
  PKI security review found in `bind_hostname` (`reactor_tls.h:91-101`) never reaches them.
- A node checks that its own leaf matches a pin of its own entry: at start under a static
  directory (`tls.h:33-35`), and after its first fetch as a joiner, whose directory arrives then
  (PT-R18).
- Lookup is an ordered index from digest to node, confirmed by `CRYPTO_memcmp`. The
  constant-time scan over every pin (`tls.h:56-62`) is not kept: a pin hashes a public key, so a
  lookup's timing tells an attacker nothing it lacks, and at 100,000 nodes a scan would read
  3.2 MB of digests per handshake (arithmetic).
- Additive: `certificate_fingerprint`, `validate_peer_identity`, and `authenticated_voter` keep
  their signatures and behavior. New: `public_key_fingerprint`, `PinKind`, `Pin`, `Directory`
  and `AddressBook` (D3), and `authenticated_node(SSL*, const Directory&)`.
- A same-key renewal (`pki request --key`, `pki.md:515-516`) leaves a `PublicKey` pin as it is,
  so renewals, 274 a day at 100,000 nodes on 365-day leaves by the PKI helper's estimate
  (`pki.md:257`), never touch the directory. The identity URI stays advisory for peers.

### D3. The node directory in `consensus`: authentication only (PT-R6)

- `Directory` is a value type: a version, the next node id, and entries from node id to one or
  two pins. Placement's states (joining, active, draining) and failure domains are the root's
  (D4), so activating or draining a node changes no version, and a 4-byte domain does not freeze
  in a library Detent links.
- Invariants, checked by the root and again by every node that applies a change:
  1. A digest of one kind names at most one node.
  2. A new node's id is at least the next node id, which then passes it; so no id is reused and
     no tombstone is kept, since a removed node's entry is deleted (PT-R6).
  3. An entry holds one or two pins.
- `apply(version, changes)` applies all or none: a change set that breaks an invariant is
  refused and the table kept (T2). It reports the node ids it touched, and the policy closes the
  sessions of removed nodes and of pins that left (D14).
- **Addresses** are hints in an `AddressBook` beside the directory, never in it (PT-R6, PT-R8):
  from configuration (Detent's `PeerOptions::addresses`; strata until Track D2), from hello tag 1
  for the node that sent it, within the operator's ranges (D8, SR11), and from Track D2's member
  view, whose 32 bytes per member (`simulator.md:134`) hold one 19-byte endpoint. A wrong hint
  costs a failed dial (TS-R2).
- **The static preset** builds the `Directory` from `PeerOptions::pins` (certificate pins,
  version 0, next id above the largest) and the `AddressBook` from `PeerOptions::addresses`
  (`peers.h:30-31`). Detent's configuration file (`AUTHORITY_TRANSPORT.md:47`) is that preset.
- Two pins per entry also serve a host that builds its `Directory` directly: listing a voter's
  old and renewed certificate pins together removes the restart order that one pin per voter
  forces (4 of 6 orders lose quorum, PKI-R2), and a certificate pin beside a `PublicKey` pin is
  how a voter switches kinds. `PeerOptions::pins` keeps one pin per voter.
- Size: a 16-byte entry and a 48-byte index record per pin, about 64 bytes per node: 6.4 MB at
  100,000, 11.2 MB if every node were in an overlap. An estimate from these fields; T12 measures
  it.
- `consensus` owns the value type and its invariants. The bytes that carry changes between nodes
  are strata's (D16), since only strata's root produces them.

### D4. Where the directory lives, and how it changes (Q1, decided)

- **Home: strata's root authority.** Its state holds the authentication directory (versioned and
  certified, D16), the placement node table (state and failure-domain path, placement's), the
  signer epochs (D16), enrollment reservations (D15), burned digests, and the admin set with its
  audit (D17). Only the directory and the signer epochs travel to every node. The root places
  placement groups, so it needs the nodes and their domains anyway, and its budget of one entry a
  second (`simulator.md:132`) covers the directory's changes. DISTRIBUTION's root row
  (`DISTRIBUTION.md:44`) changes to match.
- **Writers.** Every command is an admin's signed statement (D17), entering through the root's
  admin listener; no peer originates one.

| Command | Effect | Refused when |
|---|---|---|
| `Join{pin, domain}` | Assigns the next node id, adds the node with the pin, and records it joining | The pin is burned or names a node |
| `Activate{node}` | Placement only: placement may use the node | The node is not joining |
| `AddPin{node, pin}` | A second pin, for an overlap; two admins (D17) | The entry has two, the pin is burned or names another node, or the node is absent |
| `RemovePin{node, pin}` | Back to one pin; the digest is burned | It is the entry's only pin |
| `ReplacePin{node, old, new}` | Both in one version, with no overlap (PT-R15); the old digest is burned; two admins | `old` is not the node's, or `new` is burned or names another node |
| `Drain{node}` | Placement only: replicas move away | The node is absent |
| `Remove{node}` | The entry is deleted and its digests burned | Never (absent is a no-op) |
| `Reserve{expiry, issuer, token digest}`, `Enroll{node, token digest, pin}` | Enrollment's side (D15) | D15's refusals |
| `AddAdmin`, `RemoveAdmin` | The admin set (D17); two admins | D17's refusals |

- **Burned digests.** A digest the root ever dropped is never registered again unless the
  statement sets `allow_burned`, which the audit records (SR9). At 4% node loss that is about
  4,000 digests, 128 KB, a year (arithmetic), held at the root only.
- **Join.** An admin registers the node's pin before it starts; `Join` answers with its node id,
  which the node's configuration carries. It starts with its key, its leaf, the CA bundle, its
  id, and the root's seed (below), fetches the directory (D16), checks its own entry (D2), and an
  admin activates it once it runs.
- **Renewal** with the same key changes nothing.
- **Re-key**, with the old key still held: `AddPin`; wait until every peer node reports holding
  that version (each `Ping` carries it, D13); switch the node to its new key and leaf;
  `RemovePin` the old one. A node that switches early is refused only by peers still on the
  older version, until they fetch.
- **An exposed key on a machine still trusted:** `ReplacePin` (PT-R15). Peers that have not
  applied it refuse the node until they do, about one dissemination window, and nothing
  re-replicates. **A compromised machine:** `Remove`; it rejoins under a new id and key, and its
  replicas copy again, about 891 GB per node (`simulator.md:85`).
- **Removal** is revocation for peers (`pki.md:580-597`): each node that applies it closes its
  sessions with that node and refuses new ones (D14).
- **Dissemination** is D16: certified versions that any node relays. Every node holds the
  whole table, since it may connect to any node as a monitor, a fan-out child, or a coordinator
  (`DISTRIBUTION.md:64-65`).
- **Bootstrap.** The root is founded by an explicit command carrying the seed: the cluster
  identity, the founding root voters (node id, incarnation, pin), which are signer epoch 0
  (D16), and their endpoints. A node with neither a seed nor a cached certified directory refuses
  to start, so starting nodes never makes separate clusters (TS-R6).
- A node that reaches no relay keeps its certified table: existing nodes keep authenticating, and
  joins and removals wait. Q6 asks whether a stale table should refuse new sessions.

### D5. Admission: an incarnation speaks only from its own node (SYS-14; Q2, decided)

- A frame of a group kind names its group, its source incarnation, and its destination
  incarnation (D9). On a session authenticated as node n, a receiver admits it only when all of
  the following hold.
  1. **Hosting.** It hosts a replica of the group holding the destination incarnation (Track C's
     D5); otherwise the frame drops as `unhosted`.
  2. **Binding**, read from `Core::known_members()` (D18): every incarnation the replica's held
     memberships name, with its node and whether a committed membership names it, and the
     newest committed membership's `next_incarnation`.
     1. A committed membership names the source on node m: it is bound when m = n; otherwise it
        is `forged_source` and the session closes. Committed bindings never change: an
        incarnation is never reused or moved along the committed log (`membership.h:128, 132`),
        and every replica checks each change (D18). A removed member's incarnation stays bound
        to its node until compaction drops the entries that name it, so its frames still in
        flight, or a removed leader's hand-off (`core.h:1014-1019`), pass; rule 2.3 refuses it
        after that.
     2. Otherwise an uncommitted membership the replica holds names it on n: it is bound.
     3. Otherwise, below the committed `next_incarnation`, it is retired or invented:
        `forged_source` (SR1). Every incarnation added after that membership is at or above its
        counter (`membership.h:128`).
     4. Otherwise it is unbound: the frame drops as `unbound_source`, and the policy asks the
        witness `Vouch{group, n, incarnation}`. An uncommitted membership that names it on
        another node lands here, not in 1: two leaders' uncommitted entries can give one new
        incarnation number to two nodes, and only the committed log says which holds. A vouch
        outranks an uncommitted membership, since the record is committed and the entry is not:
        once the witness places the incarnation on n, frames naming it from another node drop as
        `unbound_source`.
  3. **One voice per node.** When bindings give n two incarnations of the group, n speaks only
     as the higher; a frame from the lower is `forged_source`. An honest node holds two only
     after Track C's same-node replacement or after a removal and a later re-add, and a node
     holds at most one incarnation of a group, so the older one's state is gone before the newer
     one exists (`track-c.md:324-327`) and the older one never speaks (PT-R10).
- SR1's first clause asked that a node bound to one incarnation never be vouched for another.
  Rule 3 keeps its intent, one incarnation per node per group, without refusing the replacement:
  a replica that lagged past a same-node replacement must admit the node's new incarnation when
  it leads, or it could never catch up, and refusing would close the session and reset every
  group on it at each resend. What stops a learner's invented incarnation is that the witness
  answers for an incarnation, not a node (SR1's third clause).
- **Sources bound only by a vouch** may send appends, transfers, and vote and pre-vote messages,
  which catch-up and elections need. Coalesced heartbeat records, `Wake`, `LeaderGone`, and
  `TimeoutNow` act only from the replica's known leader (`TimeoutNow` already does,
  `core.h:809`), so a stale vouch neither raises a commit index without log matching
  (`track-c.md:629-634`) nor starts a campaign; others drop as `not_leader` (PT-R11). C2's D12
  carries the rule for its kinds.
- **Node-level frames** (group zero) are the node's own. Each record of a coalesced heartbeat
  names its group and source and passes the rules by itself, so Track C's D12 record gains its
  source incarnation: 48 bytes with the group, term, destination, and commit. Host frames are
  node-level: a host that means a group puts it in its body and authorizes the frame by the
  session's node (PT-R5).
- **The witness** is the group's parent authority, answering for one incarnation (Q2's
  conditions, SR1, SR2, PT-R11):
  - A shard group's placement group, a placement group's root, and, for the root's own
    voters, the signer epochs (D16). Track D3's placement records therefore carry each replica's
    incarnation beside its node.
  - Answers come from committed records only. First, the assignment that placed the local
    replica lists the group's members (incarnation and node) as of that assignment and travels
    with it, so a new replica asks nothing about the members it starts with, where revision 0
    would have queried about 300 times per lost node (PT-R11). Otherwise, a linearizable read of
    the record through the parent's leader (the core's `Read`, `types.h:182`).
  - A vouch binds the incarnation to n at that replica until a committed membership names it,
    when rule 2.1 decides, or until the record's version changes, when the host asks again. A
    refusal is kept for the same version. The policy keeps at most one vouch in flight per group
    and node, and frames from an unbound source drop meanwhile; Raft resends.
  - Fixed groups never consult it: their members are their founding voters, each its own node
    and incarnation (`membership.h:69-75`). Until Track D3 exists, strata's host passes a static
    witness from its configuration.
- **Votes.** A candidate counts at most one grant per incarnation per term, only from voters of
  its membership in force (`core.h:814, 818`), and rule 3 lets a node cast at most one of those;
  no vote travels any other way (D1).
- **Why a witness.** A replica that lagged past a membership change does not know the new
  leader's incarnation. Refusing it would leave the replica unable to catch up while the group
  runs on its other members; Raft grants votes without consulting the receiver's configuration
  for the same reason (§4.1). Admitting any authenticated node, as Track C's D5 reads today
  (`track-c.md:305-309`), leaves the hole the starting point describes. Q2's option 2,
  membership changes signed by a quorum of the previous membership and chained to one the
  replica holds, closes the stale-witness residual (Limits of this design) without a witness and
  would reuse D16's statement-and-quorum form; it is the escalation if T10 shows records lagging
  past the compaction horizon (SR1).
- Xapiand's `ELECT_PRIMARY` votes travel over unauthenticated UDP until Phase 7b retires them
  (SYS-14); the owner's ruling and the failover branch's security review cover that path
  (`PLAN.md:45-46`).

### D6. Wire versions: a range, negotiated per session (SYS-3)

- A wire version is a number naming the hello and the codec's frames, kinds, and fields. Every
  binary speaks two: its own N and N − 1.
- The dialer's hello offers its range. The acceptor chooses v, the smaller of the two maxima,
  when v is at least the larger minimum. Otherwise it answers with its own hello, chosen version
  0, and closes; both ends count `version_refused` and log both ranges, and a node whose range
  lies below its peer's logs that it needs an upgrade, not a network fault (PT-R4, PT-R14). The
  dialer checks the acceptor's choice against its own computation and refuses any other value,
  above both maxima or below both minima included (SR14). A session then encodes and decodes at
  v: the policy drops a message newer than v and counts it as `unsupported`, and receiving one is
  a protocol error that closes the session.
- **Version 2 is the first that negotiates:** the `DTHL0002` hello (D8) and the frame header
  with kinds 1 to 18 (D9). Every later version keeps the magic `DTHL0002` and the fixed part,
  so any binary reads any other's range (PT-R4). Version 1 is `b7073bb`'s `DTHL0001` and `DTC1`,
  which cannot negotiate: a version-2 node refuses a `DTHL0001` hello by its magic and logs it as
  version 1, while `b7073bb` counts a rejection and logs nothing (`peers.h:342-345` at
  `b7073bb`).
- The frames consensus `main` writes from `0307308` to `fc66c0e` (`DTC2` tags 1 to 8 without the
  header) are a development state, never a supported version, on Q4's condition that Detent
  adopts version 2 itself (D7).
- So leaving version 1 is one coordinated restart, the "homogeneous DTC2 restart" Detent's
  Group F plans (`LIBRARY_ADOPTION_ARCHITECTURE.md:19, 86`); strata and Xapiand never run
  version 1 (Q4, decided).
- **`DTPROF02`** is the SHA-256 of `DTPROF02`, the cluster identity, and the host's profile
  bytes. It leaves the wire version out (Track C's D2 put it in, `track-c.md:138-140`), since two
  versions sharing one fingerprint is what lets them connect. A host keeps its profile bytes
  equal across a rolling upgrade; anything that differs between versions belongs to D7's
  activation. Detent may keep passing its `DTPROF01` value as the profile bytes, which keeps its
  rule that a different quorum or limit fails admission (`AUTHORITY_TRANSPORT.md:9`).

### D7. What one version back promises

| Promise | Rule |
|---|---|
| Connection | Two binaries whose ranges overlap connect at the highest common version, so a cluster whose nodes run N − 1 or N is fully connected |
| Messages | Every message of N − 1 flows on every session; a message new in N flows only on sessions at N |
| Group features | A feature that needs every member to understand a version-N message (C2's quiescence needs `Wake` and `LeaderGone`) is used by a group only while its leader's sessions to all its members run at N or later; the policy reports each peer node's version to the host, and T3 tests that a group turns such a feature off while one member's session runs N − 1 (PT-R14) |
| Formats | A wire version changes no storage, log entry, or snapshot format. A format change activates only after every node runs N, through an active version the cluster records durably (strata: a field of the root's certified state, D16; Detent: its configuration). Before activation a node may roll back to N − 1; after it, an N − 1 binary refuses the new formats, as Track C's readers refuse unknown magics (`track-c.md:130-134`), and never misreads them. Track D8 builds the activation |
| The directory's formats | Each has its own magic (D16). A new change kind or field comes only with a new magic, which a relay sends only to a node whose fetch names it, so N − 1 nodes keep fetching what they read (PT-R14) |
| Cadence | N + 1 starts only once every node runs N: every directory entry has reported a highest version of at least N, through Track D2's member view once it carries versions and, before it, a report each node sends the root at startup. A node down across an upgrade blocks N + 1 until it returns or is removed. A host release moves its consensus pin by at most one wire version; a gap of two is refused at both ends and counted, which the simulator's T15 checks (`simulator.md:739`) |
| Code | The codec keeps N − 1's branches for one version and N + 1 deletes them, so one implementation carries at most two versions |
| Host frames | Kinds 256 and above (D9) carry the hosts' protocols, which keep the same two-version rule by their own numbers |
| Detent | Detent leaves version 1 by one coordinated restart into version 2 itself, never into `d7192f7` or another commit of `0307308..fc66c0e`, whose `DTC2` frames under `DTHL0001` are no supported version; leaving one of those would take a second restart (Q4's condition, PT-R14) |
| Not promised | Skipping a version, three versions at once, or rolling back below an activated version |

### D8. The hello, `DTHL0002`

Each end sends one inside the TLS session, the dialer first as today (`session.h:126-139`), only
after the policy admits the pinned leaf (D12, SR13), all under the pre-authentication deadline
(D10). Integers are little-endian, as the codec's `put64` writes them.

| Offset | Bytes | Field | Check |
|---|---|---|---|
| 0 | 8 | `DTHL0002` | The same in every later version (PT-R4); `DTHL0001` is version 1; anything else is refused |
| 8 | 4 | The hello's whole length | 210 to 4,096, read with the magic as the first 12 bytes, before the rest is allocated |
| 12 | 2 | Lowest wire version | At least 2 |
| 14 | 2 | Highest wire version | At least the lowest |
| 16 | 2 | Chosen version | 0 from the dialer; the acceptor's choice, or 0 when it refuses (D6) |
| 18 | 1 | Lane | 1 control, 2 bulk; anything else is refused |
| 19 | 5 | Reserved | Zero |
| 24 | 8 | Node id | Equals the pinned node (D2) and differs from the local node (`hello.h:50`) |
| 32 | 16 | Cluster identity | Equal at both ends |
| 48 | 16 | Connection identity | Fresh, nonzero, and different at the two ends (`tls.h:37-44`) |
| 64 | 16 | Boot identity | Fresh and nonzero at each process start; a new one reports the node restarted (D13) |
| 80 | 32 | Profile (`DTPROF02`, D6) | Equal at both ends |
| 112 | 80 | Node limits: the ten `Limits` fields in declaration order (`types.h:55-67`) | `validate_limits` holds (`membership.h:47-56`); reported to the core as `PeerLimits` for each of the node's incarnations |
| 192 | 8 | Certified directory version held | 0 for a static directory |
| 200 | 8 | Largest host data frame accepted | Up to 2^32; bounds a reassembled host data frame (D9) |
| 208 | 2 | Extension count | At most (length − 210) / 4, the smallest encoding |
| 210 | rest | Extensions: a 2-byte tag, a 2-byte length, and its bytes, each | Tags strictly increasing, so a repeat or a descent is refused; each within the length; unknown tags skipped |

- **Tag 1, listen endpoints:** 1 to 8 records of 19 bytes: a family byte (4 or 6, never the
  platform's `AF_INET6`, which is 10 on Linux, 30 on macOS, and 28 on FreeBSD), a 16-byte address
  (IPv4 in its first 4 bytes, the rest zero), and a nonzero port; the length is a multiple of 19
  (SR8). Endpoints enter the address book only for the sending node and within the operator's
  ranges (D3).
- Bytes 0 to 209 and the extension rules freeze with version 2; later versions add extensions
  only (PT-R4).
- **Limits hold back only their node (SR4).** The core's `wire_limits()` takes, per field, the
  minimum of the group's limits in force, the leader's own node's, and the largest value a
  quorum of the voters in force accepts, a voter that reported nothing counting as the leader's
  node as today (`types.h:214-217`), in place of the minimum over every reported member
  (`core.h:314-332`). So neither one member nor any minority lowers what a group admits, and no
  learner counts, while a majority's lower limits still do, so a leader never admits a command
  no quorum can store, the wait Track C's D2 describes (`track-c.md:153-155`). A voter below an
  entry is held back (`peer_wire_limits`, `core.h:339-349`; counted by `held_back`,
  `core.h:98`) and catches up by install; the policy counts `limits_below_group` per node and
  logs it. Raising a group's limits still checks every reported member (`core.h:620-628`).
- Carrying node limits closes Track C's owed item (`track-c.md:42-45`): a member's limits reach
  the core once its node connects.
- The group and epoch identities leave the hello (`hello.h:11-16`) and the configuration
  identity leaves the profile (`profile.h:22-23`), since one connection carries many groups and
  frames name their group (D9). Detent's preset folds its group and restore epoch into the
  16-byte group id, the first 16 bytes of SHA-256 over `DTGROUP1`, the group, and the epoch, so a
  voter of another epoch names another group and its frames drop as `unhosted`, as `DTHL0001`'s
  equality check refused it before.

### D9. Frames and kinds: version 2

After the session's 4-byte length, every frame starts with a 40-byte header:

| Offset | Bytes | Field |
|---|---|---|
| 0 | 8 | Kind |
| 8 | 16 | Group identity; zero for node-level kinds |
| 24 | 8 | Source incarnation; zero for node-level kinds |
| 32 | 8 | Destination incarnation; zero for node-level kinds |
| 40 | rest | Body |

- **Header first (SR6).** The driver reads the length (at least 40, at most 1 MiB, the version-2
  wire maximum) and the header, and the policy decides from them before the body is allocated:
  a data frame against the credit of its connection and group, a control frame against its
  kind's size, both out of their own pools (D11). Kind 3 is control when its length is 104, its
  size without entries, and data otherwise. The 4-byte `DTC2` magic leaves the frame, since
  the session's version names the codec, and `control_frame_bound` grows from
  `76 + 32·rpc_entries + rpc_bytes` (`codec.h:10-15`) to `104 + 32·rpc_entries + rpc_bytes`.
- **Levels (SR8).** A group-level kind needs a nonzero group and nonzero incarnations; a
  node-level kind needs all three zero. Anything else is a protocol error.

| Kind | Name | Level | Lane | Class | Size with the header | Body | Owner |
|---|---|---|---|---|---|---|---|
| 1 | `VoteRequest` | Group | Control | Control | 72 | Today's, after the kind (`codec.h:51-112`) | C1 |
| 2 | `VoteResponse` | Group | Control | Control | 56 | Today's | C1 |
| 3 | `AppendRequest` | Group | Control | Control without entries, data with them | 104 without entries; at most `control_frame_bound(receiver's limits)` | Today's | C1 |
| 4 | `AppendResponse` | Group | Control | Control | 96, or 112 with its compacted boundary | Today's | C1 |
| 5 | `SnapshotResponse` | Group | Control | Control | 88 | Today's | C1 |
| 6, 7 | `PreVoteRequest`, `PreVoteResponse` | Group | Control | Control | 64 each | Today's | C1 |
| 8 | `TimeoutNow` | Group | Control | Control | 64 | Today's | C1 |
| 9, 10 | Coalesced heartbeat and reply | Node | Control | Control | C2 fixes it: records of 48 bytes, at most one per group shared with the sender | Track C's D12, each record with its source incarnation (D5) | C2 |
| 11, 12 | `Wake`, `LeaderGone` | Group | Control | Control | C2 fixes them | Track C's D12 | C2 |
| 13 | `Ping` | Node | Control | Control | Exactly 64 | Sequence (8), sender's monotonic time in ns (8), certified directory version (8) | D13 |
| 14 | `Pong` | Node | Control | Control | Exactly 56 | The `Ping`'s sequence and time | D13 |
| 15 | `Window` | Node | Either | Control | 48 plus 40 per record | A count (8), then records of group (16), increment (8), wanted (8), released (8): at most one per group, at most the groups shared plus group zero | D11 |
| 16 | `Chunk` | Group | Bulk | Data | At most 65,728 | One `RFTXFR01` frame of kind `Chunk` (`snapshot_transfer.h:29, 70-75`) | C4 (Track C's D18) |
| 17 | `TransferControl` | Group | Bulk | Control | At most 6,480 | One `RFTXFR01` frame of kind `Begin`, `Accepted`, `Result`, or `Cancel` | C4 (Track C's D18) |
| 18 | `Fragment` | That of its inner frame | Either | Data | At most the wire maximum | Inner kind (8), total length (8), offset (8), bytes | D11 |
| 19 to 255 | Reserved | | | | Refused at version 2 | | Later versions |
| 256 to 511 | Host control | Node | Control | Control | At most 4 KiB | The host's, undecoded | Host |
| 512 and up | Host data | Node | Either | Data | At most the fragment size whole; reassembled, at most the receiver's announced bound (D8) | The host's, undecoded | Host |

- **Value rules (SR8).** `Ping`, `Pong`, and records have exact lengths. A coalesced heartbeat
  naming a group twice, or more groups than the receiver shares with the sender, is a protocol
  error; C2 owns the body, and the transport enforces the bound. A `Chunk` or `TransferControl`
  envelope's source and destination must equal the header's incarnations and its session
  identities the connection's (`snapshot_channel.h:215` checks the latter today), and a
  `Credit` frame (`snapshot_transfer.h:29`) is a protocol error at version 2, not only unsent
  (PT-R5). Splitting transfer control from chunks makes every kind's class known from the header.
- **Fragments (PT-R5, SR8).** A data frame larger than the fragment size, a transport preset of
  256 KiB, goes as fragments; one no larger goes whole. The inner kind is a data kind other than
  `Fragment`. The first fragment (offset 0) is checked for hosting, binding, and credit for the
  whole total before reassembly allocates, and the total's credit is spent then. Fragments then
  arrive contiguous: each offset equals the bytes received, the bytes are nonzero, and offset plus
  bytes stays within the total, checked without overflow. At most one fragmented frame is in
  progress per connection and group. The reassembled bound is each inner kind's: an append's
  `control_frame_bound` at the receiver's limits, 65,728 for a chunk, the announced host data
  bound for host data. A sequence idle for the frame deadline closes the connection
  (`fragment_stalled`, SR5).

| Event | Effect |
|---|---|
| An unknown or reserved kind; a broken level or value rule; a control frame above its kind's size; a body that fails to decode; a message newer than the session's version (`unsupported`) | Protocol error: the connection closes |
| Data past the credit held (`credit_violation`); an increment that raises a window past 2^62; a `wanted` above the receiver's announced bound (D11) | The connection closes |
| A forged source (D5) | The connection closes |
| A fragment sequence or a reserve grant idle for the frame deadline (`fragment_stalled`, `reserve_stalled`) | The connection closes |
| An unhosted group or destination; an unbound source; a leader-only kind from a source bound only by a vouch (`not_leader`); more than 8 control frames pending for a group, or 8 host control frames for a connection (`busy`) | The frame drops and is counted |
| A frame its group's core refuses, such as an entry above that group's limits | The core answers it; the connection stays |

- Each close resets every group the node pair shares on that connection (Track C's condition
  22, `track-c-design.md:2136-2149`), which is why only what an honest peer never sends closes.
- Every kind here is fixed before the coordinated commit, since a kind added later is version 3
  (`track-c.md:177-180`); kinds 9 to 12 wait for C2's bodies (Track C's D12), as the ledger
  records.
- Every new decoder bounds each length and count before it allocates, as the codec does today
  (`codec.h:121-122, 163-165`) and as Detent asks of the library's recovery
  (`LIBRARY_ADOPTION_ARCHITECTURE.md:90`; SYS-12).
- The snapshot channel keeps its sessions, one per bulk connection and group
  (`snapshot_channel.h:33-35`), whose peer is the source incarnation D5 binds.
- The C1 frame fixture (`test/fixtures/c1/frame-timeoutnow`) is regenerated in version 2's
  envelope with its body unchanged.

### D10. Connections

- One connection per node pair and lane (Track C's D16). The control lane carries every group's
  Raft frames, keepalives, windows, and host control frames; the bulk lane carries transfers and
  host data that asks for it. A group's appends never split across lanes (R12).
- **Reasons to keep a connection**, at each end:
  - *Group*: the other node shares a local group, by the memberships (peers). Placement bounds
    them at 64 nodes (Track C's D12). A group connection closes 60 s after no group is shared.
  - *Held*: the host's persistent routes, Track D5's fan-out tree and Track D2's monitoring
    topology, at most 256 nodes. A held connection sends a `Ping` after 10 s idle, so the other
    end's idle rule never closes it.
  - *Contact*: the host's occasional use, such as a witness read or a fetch from a non-peer, at
    most 128 nodes, least recently used first, closed after 30 s idle.
  - *Guest*: an incoming connection its acceptor holds no reason for, at most 512, least
    recently used first, closed after 30 s idle.
  A node pair has at most one connection per lane whatever its reasons; a node holds at most
  1,024 connections, and when full closes the least recently used guest, then contact, never a
  group or held one.
- **Contacts and guests** reuse `Kronuz/lru-cache`'s plain `lru` (`lru.h:298-341`) with a fixed
  hash: its aging path reads `steady_clock` (`lru.h:209, 227, 242, 253, 673`), which the policy
  may not, and lookup in an unordered container is deterministic (`simulator.md:182-184`); the
  policy's `tick` expires idle entries (PT-R16).
- **Query fan-out (PT-R12)** rides held connections along a stable aggregator tree, never
  contacts. At random aggregators and replicas, a query to a 16,384-shard index at fan-in 64
  (`simulator.md:133`) touches 16,644 node pairs, nearly all new handshakes, and `strict` turns
  resumption off: 0.33 handshakes per node per second for each such query per second
  cluster-wide (arithmetic). Over held routes, handshakes happen only when the tree changes.
  Track D5 designs the tree; the cost table states both.
- **Monitoring (Q3's condition, PT-R13).** Track D2 probes over held connections in a fixed
  monitoring topology (Rapid's observers, HyParView's active view), so probes cost no handshake
  and survive a slot flood.
- **Dialing.** Either node may dial. When both dial at once, the connection the lower node id
  dialed stays and the other closes, the same choice at both ends. Today only the lower id dials
  (`peers.h:90-94, 344`), which would leave a higher id unable to reach a contact. A node whose
  acceptor is full keeps dialing out, so a pair with one jammed acceptor still connects (SR7).
- **Handshake admission (SR7, PT-R13).** Before TLS starts, the acceptor refuses a source outside
  the operator's address ranges (`peer_ranges`, any by default), a source with 2 handshakes in
  flight (an IPv4 address, or an IPv6 /64), and any source once 16 are in flight; each refusal
  closes the socket and is counted by reason. The TLS handshake, the pin, and the hello complete
  within the pre-authentication deadline, 2 s (`handshake_deadline`), against about 0.48 s for
  TLS 1.3 at the simulator's cross-region p99 round trip (arithmetic, `simulator.md:229-231`);
  frames then take today's 5 s deadline (`peers.h:37`). Holding all 16 slots then takes 8
  sources and 8 new connections a second, against 3.2 from anywhere in revision 0.
- **Backoff (SR11).** Exponential from 100 ms, doubling to 30 s, plus today's deterministic
  jitter, reset when a session becomes ready; failures are counted per endpoint. Detent's preset
  keeps today's flat 100 to 199 ms (`peers.h:329-331`) until Detent opts in.

### D11. Flow control (C4)

- **Credit** counts the bytes of data frames: appends with entries, `Chunk`, `Fragment`, and
  host data. It is kept per connection and group, group zero for node-level data, and granted
  by the receiver in `Window` records. No connection window exists: the reader always drains the
  socket into credited queues, so one would only repeat TCP's (PT-R1). No window is implied by
  the wire version, so a connection starts with no credit. A sender never sends data past its
  credit, and a receiver closes a connection that does (`credit_violation`).
- **The incoming budget B** (`PeerOptions::incoming_bytes`: 16 MiB in Detent's preset,
  `peers.h:32`; 256 MiB in strata's) splits three ways (PT-R1, PT-R3):
  - an *initial pool*, at most B/4;
  - a *reserve* R, the largest frame the node accepts: the larger of `control_frame_bound` at its
    own node limits and its announced host data bound. A node refuses to start when R exceeds
    B/2. At the default limits R is 2,105,448 bytes, or strata's 8 MiB host data bound (D16)
    where that is larger; commands at the 64 MiB ceiling need B of at least 128.02 MiB, and
    limits at every ceiling 516 MiB (arithmetic);
  - a *growth pool*, the rest.
  The budget stays today's `ByteBudget` (`session.h:12-32`) until Track C's node pools move to
  `ledger`'s capacity and permit core (`track-c.md:709-711, 720-721`), and then it is one of
  them. Track C's D15 node send budget (`track-c.md:744-747`) is a separate pool that shares
  the 256 MiB default (PT-R16, PT-R18).
- **Who gets credit.** A receiver grants a group's credit only to a node the binding (D5)
  admits as a member of that group. Windows above w0 go only to the group's known leader on the
  control lane and to an active transfer's source on the bulk lane, whose data they carry;
  group zero's go to any peer. Every grant counts against the receiver's pools and against the
  peer's share, the larger of B/8 and B over the active peers: 32 MiB at B = 256 MiB with
  eight or more (SR5).
- **Initial grants.** After the hellos, each end grants w0 = 64 KiB (a preset) on the control
  lane for every group whose leader, as its replica knows it, is on the peer, and for group zero
  on each lane, in one `Window` frame, while the initial pool has room; a group beyond it starts
  with none and asks. At the contract that is 667 followed groups and 341 connections, 63.0 MiB
  (arithmetic); at 5 members per group, 71.3 MiB, so some groups start by asking. When a
  replica learns of a new leader (`leader`, D12), it grants that leader's connection.
- **Return and growth.** The receiver returns a group's credit to its known leader or transfer
  source as its Worker admits the bytes, once half the window has been admitted, and the
  returns due on a connection go in one `Window` frame with every other group's pending return.
  No timer (PT-R2). Bytes admitted from a node that no longer leads are not returned, so its
  window drains. When a return is due, the receiver may double the window toward its maximum,
  Track C's D15 `window_bytes` of 4 MiB on the control lane (`track-c.md:733-737`) and its D18's
  16 chunks on the bulk lane (`track-c.md:777-779`), from the growth pool and within the peer's
  share.
- **Release.** A node that stops leading a group (its Worker's `RoleChanged`) or ends a transfer
  gives back the credit it holds for that group in a record's `released` field, and the
  receiver's pools take it back. A peer that never releases strands at most its share.
- **Blocked.** A sender whose next data frame exceeds its credit sends a record with the bytes
  it wants, up to the receiver's announced bound for that kind; above it is a protocol error
  (SR5).
  1. A request of at most w0 from any member the binding admits takes the initial pool, else
     the growth pool, within the peer's share. So a replica that has not yet heard from its
     leader, a joiner above all, still receives its first appends.
  2. A larger request from a node that window growth serves takes the growth pool when it fits
     the group's maximum window and the peer's share: the window grows to cover it at once.
  3. Otherwise it joins one FIFO queue per node for the reserve. The head is granted from R
     once its group's queue on that connection is empty and the reserve is free; one reserve
     grant is outstanding per node, and one unused for the frame deadline closes its connection
     (`reserve_stalled`).
  So an entry larger than any window travels, a request waits only for those ahead of it, each
  bounded by a deadline, and growth never takes the reserve (PT-R3).
- **Records.** An increment that raises a window past 2^62 is a violation, so the sender's
  arithmetic never wraps (SR5), and so is a release of more than the credit held. A node keeps
  at most one pending record per group, adding increments.
- **Control frames need no credit.** A receiver keeps at most 8 pending admission per group and
  connection, and 8 host control frames per connection, and drops the rest, counted; Raft and
  the hosts resend. Their bytes come from a per-connection pool sized by those counts and each
  kind's size, so no frame on one connection fails admission because of another's (SR6).
  Node-level control frames run at once and are never queued, so a `Window` is never dropped.
  Credit therefore always returns while the connection lives, and neither credit nor control
  frames wait on each other.
- **Control first (R19).** A sender's control frames go before any data frame, and data frames
  go as fragments interleaved with other groups' and with control frames (D9). Unsent socket
  bytes stay under 64 KiB through `TCP_NOTSENT_LOWAT` (the macOS SDK defines it,
  `netinet/tcp.h:246`); where a platform lacks it, the driver bounds the send buffer instead,
  and T6 states that platform's figure. A control frame then waits for at most one fragment, the
  unsent bytes, and the queue below TCP: 3.1 ms at 1 Gb/s with a 64 KiB queue, 0.3 ms at
  10 Gb/s (arithmetic), whatever the entry's size. `Flow::frame_bytes` (`types.h:119-127`) is
  then not R19's mechanism: it stays only if T6 measures a gain from finer pipelining, and
  otherwise goes with version 2 (Detent leaves it at 0) (PT-R16).
- **Queues.** Senders keep per-group queues (`pipeline` frames and `window_bytes` bytes, Track
  C's D15) served round-robin; receivers keep per-group queues bounded by each group's window, so
  a group waiting on its barrier holds only its own credit (Track C's R12).
- **Resets.** A closed connection resets both ends: its credit, reserve grant, and fragments in
  progress are discarded, and each end discards the frames queued for it and those received on
  it but not yet admitted, which keeps memory bounded across reconnects; Raft resends. Counting
  credit without acknowledgments rests on TCP's delivery in order or not at all, which the
  simulator models (SIM-R7, `simulator.md:233-247`).
- **Snapshots.** The snapshot channel's per-chunk `Credit` (`snapshot_transfer.h:29`; Track C's
  D18 as built at `fcc18e6`) goes: chunks spend the group's bulk-lane window, and
  `SnapshotLimits::window` becomes the chunks a source keeps in flight within it, so one
  mechanism paces snapshots (Track C's R10).

### D12. The policy: one sans-I/O class (the simulator's Q3)

`consensus::transport::PeerPolicy` decides everything the transport decides. It takes events
and the time and returns actions; it reads no clock, socket, or random source.

| Inputs | Outputs |
|---|---|
| `tick(now)`, in nanoseconds | `Dial{node, lane, endpoint}` |
| `want(node, reason)`, `release(node, reason)`: held and contact reasons from the host | `Admit{connection, node}` or `Close{connection, reason}` |
| `accepted{connection, source address}`, `dialed{connection, node}` | `Hello{connection, bytes}` |
| `handshaken{connection, certificate digest, key digest, notAfter}`: digests only (SR13) | `Body{connection, pool}` or `Close`, for each frame header |
| `hello{connection, bytes}` | `Write{connection, frame}` |
| `header{connection, length, frame header}`, `received{connection, frame}` | `Deliver{group, source, frame}` to the group's Worker |
| `writable{connection, unsent bytes}`, `closed{connection, reason}` | `Vouch{group, node, incarnation}` to the host's witness (D5) |
| `admitted{group, connection, bytes}`, `busy{group}` | `PeerState{node, up, down, or restarted}` (D13) |
| `leader{group, incarnation}`, from the Worker's `RoleChanged` (`types.h:238`) | `PeerReport{node, version, limits}` (D6, D7, D8) |
| `vouched{group, node, incarnation, answer, record version}` | `DirectoryHint{node, version}`, at most one a second (D16) |
| `send{group, source, destination, kind, bytes}` from a Worker or the host | `Reverify{connection}` (D14) |
| `directory{version, touched nodes}`; `members{group, known members}` (D5, D18) | Counters by reason (D9, D10, D11) |
| `reloaded{generation}`, `verified{connection, generation, result}` (D14) | |

- **Event order (SR13).** `handshaken` carries the two digests and the leaf's notAfter; the
  policy answers `Admit` or `Close`, and the driver reads the hello only after `Admit`, so a TLS
  client with an unpinned key reaches no decoder by construction.
- **It owns** the queues and their drop rules, control first, round-robin service of groups at
  both ends, credit, the reserve, and fragment bookkeeping (D11), dial ownership, backoff,
  handshake admission and the connection caps (D10), pin lookup (D2), hello validation and the
  version choice (D6), the binding check (D5), the keepalive detector (D13), and the directory
  hint's rate (D16).
- **The driver owns** sockets, TLS through `reactor::tls`, the two digests of the verified leaf,
  framing, timers that call `tick`, fresh connection and boot identities from `RAND_bytes`
  (`tls.h:37-44`), and posting a delivery to its group's executor. `Peers` is that driver over
  Asio. The simulator's adapter is the other, over its network (`simulator.md:226-270`), so the
  refusals by pin and by version gap its S2b counts are the policy's own.
- **Executor (PT-R12).** One policy per node, on the transport's executor, which also runs every
  session's TLS: Detent's authority executor as today (`peers.h:50-51`), and for strata a
  reactor loop of its own. A delivery posts to its group's core and a send posts back, one
  cross-core hop each way, since strata's groups live on their cores (`track-c.md:460-462`).
  T12 measures CPU per frame and per byte; if one executor does not keep up, connections split
  by node id over k executors, each with its own policy and a k-th of the budget, built only if
  T12 shows the need.
- **Determinism**, as the simulator's D2 requires (`simulator.md:152-210`): ordered containers
  wherever iteration reaches an output, ties broken by node id and then group id, no ordering by
  pointer, and time and identities as inputs.
- **Receive admission** serves groups round-robin, one frame per group per pass, and skips a
  group that answers `Busy` or `Pressure` without ending the pass, so one group waiting on its
  barrier stalls no other (today's pass ends at the first `Busy`, `peers.h:180-182`). The driver
  submits an admitted frame as `Receive{source incarnation, message}`.
- **`Peers` keeps its API for Detent** (PT-R8, PT-R9): a `Peers<Host>` is the node transport
  with one group, built from `PeerConfiguration`, which version 2 leaves untouched since Detent
  builds it by position (`peers.hh:11-17`), and `PeerOptions`, which gains only appended fields
  since Detent assigns it by name (`service.cc:305-343`). `PeerStats`, `PeerOccupancy`, and
  `AppendTimings` (`peers.h:39-49`) keep their fields; Detent plans to extend them
  (`LIBRARY_ADOPTION_ARCHITECTURE.md:92`). The preset keeps Detent's values: 2 handshakes and the
  5 s deadline for the handshake too (`peers.h:33, 37`), the flat backoff, no close at expiry,
  and no send low-water mark.
- **The relocation's evidence (PT-R9).** Before the policy is extracted, a characterization test
  in `consensus` runs three `Peers` over loopback and records their frames, `PeerStats`,
  `PeerOccupancy`, and `AppendTimings` under queue overflow, control-first order, the owner pass,
  reconnects, and append observations. It is written at the pre-relocation commit and runs
  unchanged after it, and after every step that moves behavior out of `Peers`.

### D13. The keepalive failure detector

- Each end of a peer node pair's control connection (nodes sharing a group) sends `Ping` every
  k = 500 ms and answers each `Ping` with `Pong`, keeping at most one `Pong` pending per
  connection, the newest `Ping`'s (PT-R17). A peer is down after 3k (1.5 s) without a frame
  from it, or after three `Ping`s in a row go unanswered, which catches a peer that sends but no
  longer reads. Any frame counts as hearing from it, and its first frame brings it back up
  (Track C's D12, `track-c.md:647-650`).
- Held connections send a `Ping` only after 10 s idle (D10). The detector runs on none but
  group connections: Track D2's probes and Track D5's deadlines judge the others.
- A `Pong` gives the round trip from its echoed time; a difference that is negative or above 3k
  is discarded (SR8).
- A session whose hello carries a new boot identity reports the peer `restarted`, which Track C's
  D12 uses to wake that node's groups without an election.
- Each `Ping` carries the sender's certified directory version, so a node learns its peers'
  versions without a new hello: a newer one prompts a fetch, at most once a second (D16, SR9),
  and a re-key waits for its peers' (D4).
- The detector reports and the host acts (Track C's D12 and R18): followers whose leader's node
  is down wake, and a quiescent leader whose quorum's nodes are down steps down. It decides
  nothing for a group by itself.
- Cost: 2P/k frames sent per node per second, 256 at P = 64 distinct peers (Track C's D12). The
  catalog's `cluster::LivenessTracker` reads `steady_clock` and takes a mutex, so it cannot run
  in the policy, and `cluster` is not carried forward (`simulator.md:376`). Track D2's gossip
  later shortens detection.

### D14. TLS settings, reload, and session lifetime

- Peers configure reactor's `Peer` role (`reactor_tls.h:83-85`) with `PeerOptions::tls`
  (`peers.h:28, 65`), whose `strict` is reactor's own (`reactor_tls.h:45, 70-75`): strata and
  Xapiand set it, and Detent leaves it off. No second flag exists (PT-R8).
- **TLS 1.3 for strict peers (SR12).** Under `strict`, the `Peer` role sets TLS 1.3 as its
  minimum (`SSL_CTX_set_min_proto_version`, beside `reactor_tls.h:66`), since both ends are this
  library on OpenSSL 3, and TLS 1.3 encrypts the certificates. No consumer sets `strict` with the
  `Peer` role today: Xapiand's HTTPS sets it with `Server` (`src/tls.cc:39-42` at `7e8cccb7d`),
  reactor's test with `Server` (`test/tls_test.cc:150-153`), and Detent sets no `strict`
  (`service.cc:299`). So no running configuration changes.
- **Key files (SR10, SYS-13).** The host opens each file once and refuses a key that is not a
  regular file, is a symlink, has more than one link, or has a mode beyond 0640 or a group other
  than the effective one (`PLAN.md:42-44`, the rule `pki.md:337-345` applies), reads it from the
  descriptor, and hands the bytes to reactor through an additive `Options` field for PEM in
  memory, appended and empty by default, so the checked bytes, not a path, reach OpenSSL. The
  same holds at every reload.
- **Reload.** On the host's request the driver builds a new context from the host's bytes and
  uses it for new sessions; a failure keeps the old context and logs why (TS-R12). A same-key
  renewal is a reload and changes no pin.
- **Generations (SR10).** Each context carries a generation, and a session belongs to its
  context's, resumed or not, since a new context has its own session cache and ticket keys and
  nothing resumes across a swap. After a swap that changes the bundle, the driver re-verifies the
  peer chain of every session of an older generation against the new store, including those
  whose handshake straddled the swap and became ready after it, and closes those that fail. The
  PKI helper's planned rotation (its D7) then needs no restart of peers.
- **Expiry (PT-R8).** With `PeerOptions::close_at_expiry`, appended and off in the `Peers`
  preset, a session closes when its peer leaf's notAfter passes, since TLS checks validity only
  at the handshake, and the peer reconnects with its current leaf; strata and Xapiand set it.
  Off, as Detent runs today, a voter whose leaf expires keeps its sessions until they reconnect.
- A session closes when its pin leaves the directory or its node is removed (D3, D4).
- The node logs the OpenSSL version it links at startup (SR13).
- reactor gains the two additive options above. Everything else is consensus's: the context is
  consensus's (`peers.h:466`), and `TCP_NOTSENT_LOWAT` is a socket option the driver sets.

### D15. Enrollment (Q5, decided)

- **Now, offline.** A node's key, request, and leaf come from the PKI helper (`pki request`,
  `pki sign`; its D2 and D6), and an admin registers the key's SPKI pin with `Join` (D4). That
  serves the helper's stated bound of 1,000 nodes per authority, an estimate
  (`pki.md:241-254`).
- **Online issuance is its own design**, in the security tier (PT-R19):
  - Owner: a `designer` contributor the lead assigns as workstream `enrollment`, writing
    `docs/design/enrollment.md`, reviewed by the architect and the security reviewer.
  - Trigger: it starts once this revision's check clears, and it lands before any cluster runs
    above the PKI helper's 1,000-node bound; until then offline issuance and `Join` serve.
  - Requirements: TS-R5 to TS-R8 and the shape below.
- **Shape.** A renewal with the same key travels over the node's own pinned session to an issuer,
  which signs it without an operator; the directory does not change. A join spends a single-use
  token an admin mints: a 256-bit secret, with the reservation's node id, the issuer's SPKI pin,
  and an expiry. The joiner proves the secret over TLS to the issuer's enrollment listener, which
  presents its leaf and asks for none (reactor's `Server` role); the issuer then submits `Enroll`
  and returns the leaf once it commits.
- **The root's side, defined now (PT-R6, PT-R19).** `Reserve{expiry, issuer, token digest}`, an
  admin's, assigns a node id from the counter and records the issuer's pin, the SHA-256 of the
  token's secret, and the expiry. `Enroll{node, token digest, pin}`, the issuer's, spends it
  atomically: refused when the reservation is absent, expired, or spent with another pin, or the
  pin is burned or names another node; repeated with the same pin, it returns the same answer.
  Reservations are root state, never `consensus`'s `Directory`: a reserved id authenticates
  nothing until `Enroll` adds its pin.
- **The issuer is an admin-class principal (SR3):** its own pin in the admin set with the role
  `issuer`, which may send only `Enroll`, through the admin listener (D17), never the peer
  transport.
- The issuing key is an online intermediate on an issuer host, never a data node (TS-R8), under
  the offline root. The helper's first CA generation has `pathlen:0`, so an intermediate arrives
  with a rotation to a generation that allows one (`pki.md:462-465`).
- Evicting a compromised node (TS-R5) is a directory removal; peers need no CA rotation, since
  pins decide.

### D16. Certified directory versions (PT-R7, SR2; Q1's condition)

- **Signers.** The root's voters form a signer epoch: each member's node id, incarnation, and
  SPKI DER. The root runs no witness, which keeps no application state to sign, so its signers
  and its quorum are the same voters and certifying a version needs no more than committing it.
  Epoch 0 is the seed's founding root voters, whose SPKI digests must equal the seed's pins.
  After a change to the root's voters commits, the root host proposes the next epoch, and a
  majority of the previous epoch signs it once it applies; a root voter's re-key is a new epoch
  too. The root's current members thus travel in the certified state, and no seed is edited by
  hand (SR2). Certifying needs a live majority of the epoch in force. Root changes go one voter
  at a time (Track C's D3), but a replacement's window can still leave the root a majority of
  its new membership and not of the epoch before it: three of five voters lost within one
  window commit under the new membership and cannot sign. Then new versions wait, every node
  keeps its table, and the operator re-seeds by hand, which revision 0 needed at every root
  change and is here the disaster path only.
- **Versions.** Each root entry that changes the directory makes a version. Each signer, after
  applying version v from its committed log, computes S(v), signs the version statement, and
  sends its signature to the other root members; any member holding a majority assembles the
  certificate and serves it, so a compromised leader cannot withhold it alone. Integrity then
  equals the root's commit: a version applies at a node only with a majority of its epoch's
  signatures over the digest the node computes itself.
- **S(v)** is the SHA-256 of the table's `DTDIRT01` encoding at v, which is canonical: entries in
  node-id order and fixed field widths.
- **Statements** carry a context, 64 bytes of 0x20, the string `strata directory version` or
  `strata directory signers`, and a zero byte, before the signed fields, as TLS 1.3's
  CertificateVerify does (RFC 8446 §4.4.3), so no signature made here verifies as a TLS one or
  the reverse, although it uses the node's own key. The scheme follows the key's type, which the
  PKI profile fixes (`pki.md:373-377`): ECDSA with SHA-256 for P-256, with SHA-384 for P-384,
  and RSA-PSS with SHA-256 and a 32-byte salt for RSA.
- **At a node.** It holds a verified signer chain and a certified table. A fetch returns a
  `DTDIRD01` delta from the version the node holds, when the relay keeps it, else a `DTDIRT01`
  table; the newest `DTDIRC01` certificate; and the `DTDIRE01` epochs above the node's. The node
  verifies each new epoch by a majority of the one before, applies the delta to a copy of its
  table or decodes the table, computes S(v), and checks the certificate: a majority of the named
  epoch's members, distinct, each signature verified with that member's SPKI. Then it adopts v
  and hands the touched node ids to the policy (D3). Any failure keeps the old table.
- **Relays.** Every node keeps its certified table, its signer chain, the newest certificate, and
  the changes of the last 1,024 versions (a preset); any node serves fetches. The request
  (`DTDIRF01`, a host control frame: cluster, held version, held epoch, the highest format
  magics it reads) and the reply (host data) are strata's host frames. A full table at 100,000
  nodes is about 4.2 MB (arithmetic), so strata's preset announces a host data bound of 8 MiB.
- **Hints (SR9).** Versions travel in hellos and `Ping`s (D8, D13), and Track D2 may add them to
  gossip. A node fetches from the peer that advertised the highest version, at most once a
  second whatever the hints; a reply that fails verification marks that peer's hint ignored for
  that version (`hint_ignored`), so no peer is trusted alone and a forged hint costs at most one
  fetch a second. A node that heard no newer hint for 60 s asks one peer, in turn, for its
  newest version. A removal then reaches a node within its hint path's hops, each at most a
  `Ping` interval, the one-second limit, and a fetch; T10 measures the window against 60 s at
  p99.
- **Load.** One version costs n fetches, about one served per node, so the root's own members
  serve only their peers; revision 0's fetches all landed on the root's members, which their
  handshake slots could not admit within T10's window (PT-R7). Per version, each node checks
  about 3 signatures and hashes about 4.2 MB, about 4 ms at an assumed 1 GB/s (an estimate; T12
  measures it), 22 times a day at nominal churn.
- **Formats**, strata's, fuzzed in strata (T8):

| Record | Fields | Bounds |
|---|---|---|
| `DTDIRD01` delta | Cluster (16), from version (8), to version (8), next node id at `to` (8), change count (4), changes of 48 bytes: op (1), pin kind (1), zero (6), node (8), digest (32) | Count at most 65,536 and the bytes left / 48, checked before allocation; ops 1 add a node with a pin, 2 add a pin, 3 remove a pin, 4 remove a node (kind and digest zero); `from` equals the version held, `to` above it |
| `DTDIRT01` table | Cluster (16), version (8), next node id (8), entry count (4), entries: node (8), pin count (1, 1 or 2), pins of kind (1) and digest (32) | Count at most the bytes left / 42; nodes increasing and below the next node id; invariants checked as it decodes |
| `DTDIRC01` certificate | Cluster (16), version (8), signer epoch (8), S(v) (32), signature count (2), each: signer node (8), length (2), signature | Count at most the epoch's members (at most 31, `membership.h:37`); length at most 512 |
| `DTDIRE01` signer epoch | Cluster (16), epoch (8), member count (2), each: node (8), incarnation (8), SPKI length (2), SPKI DER; then signature count (2) and signatures as above, over the SHA-256 of what precedes them | 1 to 31 members; SPKI at most 1,024 bytes; epoch 0 carries no signatures |

Each record starts with its 8-byte magic. A new change kind or field takes a new magic (D7).

### D17. The admin path (SR3)

- **Listener.** Root hosts only, on an address the operator configures (none by default), apart
  from the peer port; reactor's `Peer` role under `strict`, so both ends present certificates;
  D10's handshake caps.
- **Admins.** The admin set holds each admin's name, SPKI pin, and role (`admin`, or `issuer`
  for D15). An admin's leaf is a `client` leaf of the cluster CA, with `clientAuth` and the
  SPIFFE path `.../client/<name>` (`pki.md:406, 419-420`); the listener checks the leaf's SPKI
  digest against the set and compares the path's bytes with the name the set holds. The set is
  founded with the root by the founding command and is never empty.
- **Statements.** Each command is a statement the admin signs with its leaf's key, under a
  context as in D16 (`strata admin command`): magic `DTADMS01`, cluster, the admin's pin, a
  sequence number, and up to 256 commands. The root host checks it, proposes it as a root entry
  carrying the statement, and every root replica verifies the signature and the sequence again
  as it applies the entry: a statement that fails is a no-op the audit records. So a
  compromised root leader cannot add directory changes, since honest replicas apply only signed
  ones and signers sign only what they computed (D16).
- **Numbered and idempotent.** The root keeps each admin's last sequence: a replay or a retry of
  an applied sequence is a no-op that returns the original answer, so a retry is safe.
- **Two admins** for `AddPin`, `ReplacePin`, `AddAdmin`, and `RemoveAdmin` whenever the set has
  two or more: the same commands in statements from two distinct admins, the first held pending
  for up to an hour. These are the commands that turn one stolen key into an existing node's
  identity or into the admin set itself. `Join`, `Remove`, `Drain`, `Activate`, and `Reserve`
  need one.
- **Rate limits** per admin, presets: `Remove` and `Drain` at most 10 a minute; other commands
  at most 1,000 a minute, so a 1,000-node cluster registers in a minute. Beyond them a
  statement is refused and recorded.
- **Audit.** Every statement, applied or refused, stays in the root's state with its admin's
  pin, until the host exports it to its audit log before compaction drops it.
- **Forwarding.** A root host that is not the leader forwards statements to its leader as host
  frames (D1); the signature, not the channel, carries the authority, which resolves revision
  0's conflict between `Enroll` and "never over the peer transport".

### D18. Every replica checks membership changes (PT-R10)

- Today only the proposing leader runs `validate_change` (`core.h:613`), while followers and
  recovery decode `Configuration` entries structurally (`note_append`, `core.h:302-308`;
  `decode_membership`, `membership.h:161-194`). A compromised member that leads could then append
  an entry that places other members' incarnations on its own node, every follower holding it
  would bind them there, and it would win every later election on its own grants.
- `note_append` checks each `Configuration` entry with `validate_change(membership_at(index − 1),
  entry, limits_at(index))` (`membership.h:121-140`; `core.h:275-278, 369`); the membership
  before a joiner's first entry is empty, so that one takes `validate_membership` alone. It runs
  wherever entries enter the log: a follower's append (`core.h:881`), the leader's own proposals
  (`core.h:645`), and recovery (`core.h:83`).
- A follower checks a request's entries before it changes its log, beside the limit checks that
  already answer such an append as failed at its first entry (`core.h:853-862`): an invalid
  change is answered the same way, never stored, and counted (`invalid_membership`, reported to
  the host). A recovered log that holds one does not start, as an invalid recovered entry does
  not (`core.h:80-81`). An honest log never holds one.
- The leader's own removal of a `leaving` voter (`core.h:1021-1025`) passes: it removes one voter
  and clears `leaving`. Fixed groups have no `Configuration` entries, and Detent exposes no
  membership changes (`LIBRARY_ADOPTION_ARCHITECTURE.md:92`), so nothing changes for Detent.
- `Core::known_members()` returns every incarnation the held memberships name, with its node and
  whether a committed membership names it, and the committed `next_incarnation`, cached on
  `wire_limits()`'s key (`core.h:317-319`), and the host passes it to the policy when it
  changes. One incarnation on two nodes would mean the check above failed, so it fences the
  replica.

## Cost at the scale contract

Per node, at P = 64 distinct peers (Track C's D12 bound for placement), 1,000 groups with three
members each, and n = 100,000 nodes. Estimates say so; T12 replaces them with measurements.

| Quantity | Formula | At the contract | Basis |
|---|---|---|---|
| Group connections | 2P | 128 | D10 |
| Held connections | The host's topology, at most 256 node pairs | About 85 node pairs: 64 fan-out children, a parent, 20 monitors (an estimate) | D10 |
| Contacts and guests | At most the caches | At most 128 and 512 | D10 |
| Keepalive frames sent per second | 2P/k, group connections only | 256 | D13 |
| `Window` frames per second under load | The followed groups' write rates over half a window | About 204 at the simulator's median 10 KB/s per shard, 667 followed groups, 64 KiB windows (arithmetic) | D11; `simulator.md:82` |
| Initial grants | At most B/4 | 63.0 MiB: 667 groups and 341 connections at 64 KiB (arithmetic) | D11 |
| Receive memory | The budget plus control pools | At most 256 MiB, plus 1.7 MiB of Raft control frames (2,000 pairs × 8 × 112 bytes) and 32 KiB of host control frames per connection: 10.7 MiB at 341 connections, 32 MiB at 1,024 | D11 |
| Handshakes at a restart | 2P plus the held connections | About 213 | D10 |
| Handshakes per second at rest | Contact misses; probes ride held connections | Near 0 | D10; Q3 |
| Handshakes per second at query load | 0 over held routes between tree changes; 2 × 16,644 × q / n at random aggregators | 0.33 q at random aggregators: 33 at q = 100 (arithmetic) | D10; PT-R12 |
| Directory | About 64 bytes × n | About 6.4 MB (an estimate) | D3 |
| Directory versions per day | A `Remove` and a `Join` per lost node | 22 nominal, 220 at stress | `simulator.md:100` |
| Work per version | About 3 signature checks and S(v) over 42 bytes per node | About 4 ms at an assumed 1 GB/s (an estimate) | D16 |
| Fetches per version | One per node, from relays | About 1 served per node | D16 |

## Limits and presets

Limits name the mechanism that needs them (`docs/design/limits.md`). Presets are tuning
defaults, measured before they become defaults for good. None of Lucene, Tantivy, or Xapian has
a peer transport, so no reference maximum applies to these limits, and none lowers one
`limits.md` lists: the replicated command keeps its 64 MiB ceiling, which a node uses by sizing
its budget to D11's check (calibration row 6).

| Limit | Value | Mechanism |
|---|---|---|
| Hello length | 210 to 4,096 bytes | The fixed part, then a bounded extension list; the length is read before the rest |
| Extensions per hello | At most (length − 210) / 4 | Their smallest encoding |
| Listen endpoints per hello | 8 | Bounds the extension's decode |
| Pins per node | 2 | An overlap needs the old and the new; a third would keep a retired key valid |
| Wire versions per binary | 2 | SYS-3 |
| A frame on the wire | 1 MiB | Every kind's bound fits under it; the fragment size, a preset, stays below it |
| Host control frame | 4 KiB | Control frames take no credit, so their size and count bound their memory |
| Control frames pending | 8 per group and connection; 8 host control frames per connection | Raft and the hosts resend |
| Records per coalesced heartbeat or `Window` | At most the groups shared, plus group zero for `Window` | One per group (SR8) |
| Fragmented frames in progress | 1 per connection and group | Bounds reassembly (PT-R5) |
| Reassembled data frame | `control_frame_bound` at the receiver's limits for appends; 65,728 for a chunk; the announced bound for host data | Today's caps with the header; limits the hello announces |
| Window | At most 2^62 | The sender's arithmetic never wraps (SR5) |
| Reserve | At most B/2 | One frame at the node's own limits always fits (PT-R3) |
| Handshakes in flight | 16, and 2 per source address or IPv6 /64 | Bounded memory before authentication (SR7) |
| Connections | 1,024; held 256 nodes, contacts 128 nodes, guests 512 | Independent of n |
| Signers per epoch | At most 31 | The root's voters (`membership.h:37`) |
| Directory fetches | At most 1 a second per node | SR9 |
| Admin commands | `Remove` and `Drain` 10 a minute, others 1,000, per admin | SR3 |

The windows and w0, the budget and its pools, the per-peer share, the fragment size, the unsent
bound, the keepalive timing, the deadlines (2 s before authentication, 5 s per frame), the
backoff, the idle and linger times, the retained deltas, and the anti-entropy interval (D10,
D11, D13, D16) are presets: their values come from Track C's D15, D18, R19, and D12, and the
reviews, and measurements on `main` replace them.

## Library changes

**Additive** means nothing Detent calls today changes behavior unless Detent opts in.

| Change | Repository | Additive for Detent | When |
|---|---|---|---|
| `public_key_fingerprint`, `PinKind` (1, 2), `Pin`, `Directory` (authentication only, a counter) with its static preset, `AddressBook`, `authenticated_node` | consensus | Yes: the old functions and certificate pins are unchanged | First step 1 |
| The characterization test; `PeerPolicy`, with `Peers` driving it at today's bytes; `Kronuz/lru-cache` as a dependency | consensus | Yes: a relocation, with the wire, the API, and the observables unchanged | First step 2 |
| The keepalive detector in the policy | consensus | Yes: the `Peers` preset ignores its reports | First step 3 |
| D18's check; `Core::known_members()`; the binding and the witness hook, refusing unknown incarnations by default; admission by the quorum's limits (SR4) | consensus | Yes: fixed groups have no `Configuration` entries, Detent reports no `PeerLimits`, and the witness hook is unused by fixed groups | First step 5 |
| Reload with generations and re-verification; `close_at_expiry` | consensus | Yes: unused unless called; off in the preset | First step 6 |
| PEM in memory (appended to `Options`); TLS 1.3 minimum for the `Peer` role under `strict` | reactor | Yes: Detent sets neither, and no consumer combines `strict` with `Peer` | With step 6, in reactor first |
| Appended `PeerOptions` fields: `handshake_deadline`, `peer_ranges`, the backoff kind, `close_at_expiry`, a send low-water mark | consensus | Yes: the preset keeps today's behavior | With the steps that use them |
| Version 2's wire: `DTHL0002`, the range, `DTPROF02`, the frame header, kinds 9 to 18, fragments, `Window`, multiplexing, either-node dialing, the keepalive frames; the snapshot channel's `Credit` retired | consensus | No: it changes in the coordinated commit (Track C's R15), which Detent adopts at its restart (Q4); `Peers`' API stays | After the full design review and the security review clear it |
| The root node table, certified versions and their formats, the admin path, the witness from placement records, enrollment's root side | strata, Track D | Not applicable | Track D3 |
| The activation of format versions | strata, Track D8 | Not applicable | Track D8 |
| Peer key and certificate flags, the seed, the directory client; `ELECT_PRIMARY` and UDP discovery retired | Xapiand | Its own command line, additive | Phase 7b |

## For Detent

The lead adds these to Detent's `SHARED_LIBRARY_DECISIONS.md`:

1. **Q4, the coordinated restart.** Detent leaves version 1 by one coordinated restart into
   version 2 itself, keeping certificate pins and passing its `DTPROF01` value as the profile
   bytes (D6), never into `d7192f7` or another commit of `0307308..fc66c0e`, which would need a
   second restart. Its agreement goes in `DEPENDENCY_UPGRADES.md` before step 4 lands, as SYS-3
   asks.
2. **PT-R8's constraints.** `PeerConfiguration` stays as it is, since Detent builds it by
   position; `PeerOptions` gains appended fields only; there is no new `strict` flag
   (`PeerOptions::tls.strict` exists); closing sessions at a leaf's notAfter is opt-in, off in
   the `Peers` preset, because a voter whose leaf expires unnoticed would otherwise lose every
   session at once and reopen none.
3. **Library commits (Track C's condition 1, PT-R9).** Detent's transport suite runs at each
   additive step's commit with the pin moved in a scratch build, recorded in Detent's
   `LIBRARY_BOUNDARIES.md`; `consensus` gains a characterization test of `Peers` first.
4. **What version 2 changes for Detent** at the restart: credit per connection and group with no
   connection window, sized within the incoming budget, and a startup refusal when the largest
   frame at its limits exceeds half of it (16 MiB passes at the default RPC limits Detent keeps,
   `service.cc:266-271`); the snapshot
   channel's `Credit` replaced by bulk-lane windows; either node may dial, a simultaneous dial
   settled on the lower id's connection; the group and restore epoch folded into the group id;
   the hello carrying node limits and versions.
5. **reactor (SR10, SR12).** Two additive `Options` changes: PEM in memory, and TLS 1.3 as the
   minimum for the `Peer` role under `strict`. Neither changes Detent while it sets no `strict`;
   if Detent turns `strict` on for peers, its peers speak TLS 1.3 only. Detent may adopt the PEM
   path for its own key-file checks.
6. **consensus's core (SR4, PT-R10).** A leader admits commands within the limits a quorum of
   voters accepts rather than every reported member's, and every replica checks membership
   changes at append and at recovery. Neither changes a fixed group whose host reports no
   `PeerLimits`, which is Detent's case.
7. **Opt-ins Detent may take later:** SPKI pins (D2), the exponential backoff (D10), the
   handshake deadline and per-source cap (D10), `close_at_expiry` (D14), and the send low-water
   mark (D11).
8. **`Flow::frame_bytes`** may be retired with version 2 if T6 measures no gain from it; Detent
   leaves it at 0.

## Limits of this design

- **Directory state grows with n.** Pins need every node to hold every node's entry, about
  6.4 MB at 100,000 nodes (an estimate). Identity by a certificate name under the cluster CA
  would need no table; SYS-2 chose pins.
- **Revocation waits for dissemination.** A removed node keeps its sessions at each node until
  that node applies the removal, within the hint path's hops, never measured; T10 measures it. A
  node that reaches no relay never applies it while the partition lasts (Q6), and the gap is
  silent at the nodes still holding the old version.
- **A stale witness vouches for a former host, and the gap is one of safety.** A replica behind a
  whole add-and-remove cycle while its parent's record lags admits the removed node's real
  former incarnation, and an append from it can raise that replica's commit index
  (`core.h:884-888`). It needs both lags at once and that node compromised; Q2's option 2 is the
  path if T10 shows record lag past the compaction horizon (SR1, PT-R10).
- **A compromised parent leader vouches across its range**, about 22,700 groups
  (`simulator.md:86`), the trust placement already holds.
- **A majority of the root's voters compromised** is a cluster compromise: they sign any
  directory. **One admin** in a one-admin cluster is the cluster's identity authority.
- **Losing a signer majority inside a replacement's window** stops new directory versions until
  the voters return or the operator re-seeds (D16); existing nodes keep authenticating.
- **A leader's window shrinks only when it releases credit**, so a group that grew and went
  idle under the same leader keeps its part of the growth pool until it steps down or the
  connection resets; T6's slowed-group row checks the effect.
- **Format activation is unbuilt.** Until Track D8 exists, "every node runs N before a format
  changes" holds by procedure only. Skipping it fails closed and silently at first: an N − 1
  node refuses the new records and fences that replica.
- **A host release that skips a wire version** is refused by every older node. The counters show
  it, and the cluster runs on the nodes that still connect.
- **Without `TCP_NOTSENT_LOWAT`** on a platform, the control-latency bound becomes the send
  buffer's size; FreeBSD is unverified, and T6 states each platform's figure.
- **A stalled event loop reads as a failure.** A node whose loop stalls past 1.5 s is reported
  down by its peers' detectors, silently from its own point of view.

## Gate rows

Each row runs in `consensus` at the item's commit on Linux, macOS, and FreeBSD unless it says
otherwise; strata's rows run in strata. Performance rows (T6, T11's figures, T12) are the
lead's, measured on `main`.

| Row | Target | Procedure |
|---|---|---|
| T1 Pins | `public_key_fingerprint` equals `openssl x509 -pubkey -noout \| openssl pkey -pubin -outform DER \| openssl dgst -sha256` for P-256, P-384, and RSA 2,048 leaves, uncompressed and compressed points included, and differs from `X509_pubkey_digest`; the PKI helper's T10 fixtures are a later check, once `Kronuz/pki`'s step 1 exists (PT-R17). A same-key renewal authenticates under a `PublicKey` pin and is refused under the old `Certificate` pin, beside the renewed `Certificate` pin accepted. Refused, each beside an accepted case differing in one variable: a pinned key whose chain is outside the bundle; a leaf with explicit curve parameters (SR14); a leaf whose two digests name two nodes; a removed node; a local leaf matching no pin of its own entry; a `server`-only leaf dialing and a `client`-only leaf accepting; a dialer reaching another node's pinned leaf; a TLS 1.2 ClientHello at a strict peer, beside TLS 1.3 accepted (SR12); a session at its leaf's notAfter with `close_at_expiry` on, beside off. The row states, without a case, that a CA-minted leaf for a node's key fails only by proof of possession, since OpenSSL will not configure a mismatched key (SR14). Detent's authentication cases pass unchanged through the old functions | `test/transport/authentication_test.cc` over memory BIO pairs; `tls_certificates.cmake` gains P-256 and P-384 leaves, a compressed point, an explicit-parameter leaf, one-EKU leaves, a same-key renewal, and a second CA; CTest `consensus_transport_authentication` in CI |
| T2 Directory | Each D3 invariant and each D4 refusal, broken and refused beside the accepted case. Applying a change set closes exactly the sessions of removed nodes and of departed pins. A re-key keeps every session at every step of its overlap; `ReplacePin` closes the old pin's sessions and keeps the node's groups (PT-R15); a lost key's replacement closes the old id's sessions and refuses its key. A burned digest refused, and accepted with `allow_burned` (SR9). A node without a seed refuses to start (TS-R6). A delta across a version gap falls back to a full table; a delta that breaks an invariant at the applying node is refused and the table kept. A delta signed by a minority, or by a learner, refused, and applied with a majority's; a replayed older version ignored; a forged epoch refused (SR2). Hints at any rate cause at most one fetch a second (SR9) | `test/transport/directory_test.cc` over `PeerPolicy` with a fake clock and no sockets; the certified formats in strata's root host tests |
| T3 Versions | A test-only version 3 (one extra kind, one extra extension) runs the transport suite at (3, 3), (3, 2), and (2, 3) at the chosen version, with version-3 messages dropped and counted on version-2 sessions. A group turns a version-3 test feature off while one member's session runs version 2, and on once it runs 3 (PT-R14). A test-only version 4 offering 3 to 4 is refused by version 2 at both ends, the acceptor sending its refusal hello, both ranges logged, and the older side's log naming an upgrade. A chosen version outside the dialer's computation, above both maxima or below both minima, refused by the dialer (SR14). A `DTHL0001` hello from `b7073bb` refused and logged as version 1, and `b7073bb`'s reader refuses a `DTHL0002` hello, counting it and logging nothing. Every version-2 kind round-trips against its fixture, and a version-3 build reads a version-2 hello's range and the reverse | `test/transport/version_test.cc` with `CONSENSUS_TRANSPORT_TEST_NEXT`; fixtures in `test/fixtures/v2/`; `b7073bb`'s reader from its own worktree, as Track C's T1 (`track-c.md:804-809`) |
| T4 Policy | Over 10^6 seeded random event sequences, checked after every step: no data past credit; each end's credit equal to its grants less the data sent and the credit released, never negative; credit returned only to a group's known leader or transfer source, and a node that stops leading releasing what it holds; outstanding grants within the budget's pools, initial ones included, and each peer within its share (PT-R1, SR5); each blocked request granted, in FIFO order from the reserve, once its group's queue empties, ahead of growth, and a reserve grant or fragment sequence idle past the deadline closing its connection (PT-R3, SR5); no window past 2^62; a control frame behind at most one fragment; the reader never stops draining a connection; a group without credit delaying no other; every group with pending frames served within one round of the active groups; queue bounds held and every drop counted by reason; 16 handshakes and 2 per source; a simultaneous dial settled on the lower id's connection at both ends; Detent's flat backoff from 100 to 199 ms and the exponential one from 100 ms to 30 s (SR11); the detector reporting down at exactly 3k of silence and after three unanswered `Ping`s, never earlier, up at the first frame, `restarted` on a new boot identity, and at most one pending `Pong` per connection; the same actions for a seed on every run and platform. Each mutant caught within the budget, its first seed recorded: control first off; the credit check off; the per-peer share off; the reserve's FIFO order off; a detector threshold of 2; `Busy` ending the pass; a fragment accepted out of order | `test/transport/policy_test.cc`, each seed's action hash pinned; `tools/mutants.sh` |
| T5 The driver decides nothing | Before step 2, the characterization test records three `Peers` over loopback at the pre-relocation commit, and it passes unchanged after the relocation and after every later step that moves behavior out of `Peers` (PT-R9). A three-process loopback run of `Peers` records the policy's inputs, and replaying them through `PeerPolicy` alone gives the run's actions exactly; the simulator's adapter runs the same class | `test/transport/characterization_test.cc`; `CONSENSUS_TRANSPORT_TRACE` builds of `Peers`; `test/transport/replay_test.cc`; the simulator's adapter test |
| T6 Flow control | With 100 groups on one connection and one group's barrier delayed 10×, every other group's commit p99 within 1.5× the unslowed run (Track C's T7); likewise with one peer asking for 64 MiB on each of 100 groups (SR5). Control-frame latency p99 at most 10 ms under saturating appends at 1 Gb/s, stated per platform with and without `TCP_NOTSENT_LOWAT`, as the fragment, the unsent bytes, and the queue below TCP (PT-R17), and the unsent bound at 10 Gb/s stated (64 KiB drains in 52 µs). `Flow::frame_bytes` at 256 KiB against 0, for D11's keep-or-retire decision. Receive memory never above the budget plus the control pools, and a 64 MiB entry crossing as fragments with control frames between them, as tests | `bench/transport_flow` on Linux over a veth pair at a 1,500-byte MTU with `tc qdisc ... netem rate 1gbit limit 44` (64 KiB of queue; netem's default 1,000 packets would add 12 ms at that MTU and 524 ms at loopback's), or a real 1 Gb/s link; on macOS and FreeBSD through a dummynet pipe at 1 Gb/s with a 64 KB queue (`dnctl` with `pf` on macOS, `ipfw` on FreeBSD). Interleaved trials as BENCHMARKS' rules state; the memory and fragment checks in `test/transport/flow_test.cc` |
| T7 Forged sources (SYS-14, SR1) | On a session authenticated as node X, frames as an incarnation a committed membership binds to node Y (a vote grant, a pre-vote, a higher-term append, `TimeoutNow`, a coalesced record, `LeaderGone`) are refused before the core, counted as `forged_source`, and close the session, beside the same frames from Y's session admitted. SR1's cases: a learner's session sending as an invented incarnation, below the committed counter (forged) and above it (dropped unvouched); a removed member's old incarnation from its node after compaction (forged); an unknown incarnation the witness does not vouch for by number (dropped as `unbound_source`); an unknown incarnation admitted once the witness vouches for it on X. A same-node replacement reaching a replica that lagged past it: the new incarnation admitted once vouched, the old one refused. A replica holding an uncommitted entry that names an incarnation on X admits it from Y once vouched. A source bound only by a vouch sending `LeaderGone` or a coalesced record is ignored (PT-R11). A compromised leader's entry that moves another node's incarnation is refused by every follower and never stored, and a recovered log holding one does not start (PT-R10). A learner reporting the floor of node limits while commands at the group's limit commit (SR4). A frame for a group not hosted drops as `unhosted`, and a duplicated grant counts once. A plaintext client and a TLS client with an unpinned key reach no decoder (decode count 0). In T2's randomized harness, one node per seed sends forged-source frames at every step: none is admitted and every T2 oracle holds. The binding check is one function the policy and the harness both call (PT-R17), and the mutants with it off and with D18's check off are each caught within 400 seeds | `test/transport/admission_test.cc`; `test/membership_test.cc`; `test/simulation_test.cc` with a forging node and a lying leader; `tools/mutants.sh` |
| T8 Robustness | One CPU-hour of fuzzing for each new decoder (the hello and its extensions, the frame header with its level and class rules, `Ping` and `Pong`, `Window`'s records, the chunk and transfer-control checks, `Fragment` reassembly; in strata, the directory's four records and the admin statement), zero findings with a hang timeout, at the gate's commit, each corpus replayed as a test. A frame declaring its largest legal length, and a control frame declaring the wire maximum, allocate nothing before their decision (SR6). ASan, UBSan, and TSan over every transport test. Line and branch coverage of the changed headers at TESTING's targets (decoders at 98% and 95%, the rest at 90% and 80%), which takes up the C1 gate's ruling that the transport headers' coverage goes with C4 (`track-c-design.md:1967-1969`) | `tools/fuzz.sh` per decoder, run by the test engineer under a host hold; an allocation-counting test; `tools/coverage.sh` |
| T9 Detent | At every additive step's commit, Detent builds with the pin moved in a scratch build and passes its transport suite, recorded in its `LIBRARY_BOUNDARIES.md` (PT-R9; Track C's condition 1, `track-c-design.md:1235-1237`). At the coordinated commit, Detent builds from a clean clone and passes its suite run serially with certificate pins in a static directory and its `DTPROF01` value as the profile, and a `b7073bb` voter beside a version-2 voter refuses, logged at the version-2 end and counted at the `b7073bb` end | Detent's maintainer's Group F qualification: the clean-clone suite of `LIBRARY_BOUNDARIES.md:199`, run serially as Track C's T1 requires (`track-c.md:804-809`) |
| T10 Simulator | Tier A at 100,000 nodes with one shared directory and a version per node (PT-R14): a removed node's sessions closed at every node within 60 s at p99 of the root's commit, with fetches served per node and their peak rate stated; T15's rolling upgrade with refusals decided by `PeerPolicy` (none between N and N − 1, every session refused at a gap of two); the group-failover bound (`simulator.md:128`) with D13's detector | `sim_run --tier a` and the upgrade scenarios (`simulator.md:739`), once the simulator's S2b and adapter drive `PeerPolicy`; budgets as the simulator's rows state |
| T11 Real network | On loopback with three processes, and on the test cluster of at least 30 nodes PLAN's Track D requires (`PLAN.md:487-488`), named when it exists: a rolling restart of every node between version 2 and the test-only version 3, with no group losing quorum and no refused session; a same-key renewal and a bundle change applied without a restart, closing exactly the sessions that no longer verify, including a handshake started before the swap and completed after it (SR10); a re-key with overlap; a `ReplacePin`; a removal closing sessions everywhere; RSS per connection at 128, 512, and 1,024 connections stated | Restart and reload scripts over Phase 3R's real-node harness; RSS from `/proc/<pid>/status` on Linux and `ps -o rss` on macOS |
| T12 Scale statement | The cost table's estimates measured: directory bytes per entry at 100,000 entries, with overlaps; policy bytes per pair at 2,000 pairs; frames per second at rest at P = 64; `Window` frames per second under the median write rate (PT-R2); TLS handshakes per second per core, and handshakes per second at a stated query rate with and without held routes (PT-R12); CPU per frame and per byte on the transport's executor (PT-R12); signature checks and S(v) per version at 100,000 entries | `bench/transport_scale` over a synthetic directory and groups, on the allocation-counting build; PLAN's scale statement (`PLAN.md:189-190`) |
| T13 Bounds | 17 handshakes stalled before authentication from 9 sources: the 17th refused and the 16 dropped at 2 s; a third from one source refused while two from another proceed; a source outside the operator's ranges refused before TLS; a legitimate peer connecting while one source floods, and a pair connecting while 8 sources hold every slot of one end, by that end dialing out (SR7); the connection cap reached with guests then contacts evicted, never group or held connections; contact and guest idle closes; RSS per connection at the cap (PT-R17, as transport-security's T3, `transport-security.md:143`) | A harness over raw sockets and `openssl s_client` against a three-node loopback cluster, wall time |

## Open questions for the owner

**Decided** (the ledger, 2026-10-17; option 1 on each, with the reviewers' conditions):

- **Q1. Where the directory lives:** the root's node table (D4), with versions a majority of the
  root's voters certify and any node relays (D16; PT-R7, SR2), and an authentication-only
  `Directory` in `consensus` (D3; PT-R6).
- **Q2. Binding an unknown incarnation:** the parent authority's record as witness (D5), with
  SR1's tightenings (`Vouch{group, node, incarnation}`, the committed counter, one voice per
  node), every replica checking membership changes (D18; PT-R10), and vouches expiring with
  their record (PT-R11).
- **Q3. The gossip carrier:** host frames on this transport (D1), with hardened handshake
  admission (D10; SR7, PT-R13) and a fixed probing topology over held connections (D10).
- **Q4. Detent leaving version 1:** one coordinated restart into version 2 itself (D7); Detent's
  maintainer confirms in `DEPENDENCY_UPGRADES.md` before step 4 lands.
- **Q5. Online enrollment:** its own security-tier design, with its owner and trigger named and
  the root's side defined here (D15; PT-R19, SR3).

**Q6. Should a node whose directory is stale refuse new sessions?** (SR9; the security review asks
for the owner's answer.) A node cut off from every relay never learns of a removal, so a removed
node's key keeps authenticating there, for old sessions and new ones, while the partition lasts.

1. No deadline: the residual stays, stated in the threat model and the limits.
2. A deadline (an hour, a preset): past it, a node opens new sessions only to fetch the
   directory, and admits no group traffic on them until a fresh certified version arrives.
   Existing sessions survive, so groups that kept their connections run on; groups that lose a
   connection during a long partition cannot regain it until the partition heals.

Recommendation: 1, as the security reviewer recommends. The exposure needs the removed node and
the stale node inside one partition that reaches no relay, since every node that reaches one
applies the removal within a hop; option 2 would make every long partition pay with its new
sessions to close that narrower case.

## First steps

| Step | Starts | Waits for |
|---|---|---|
| 1. Pins, `Directory`, `AddressBook`, `authenticated_node` (D2, D3), with T1 and T2's invariant cases | Now | Code review before it lands |
| 2. The characterization test, then `PeerPolicy` extracted from `Peers` with today's rules and bytes: one group, `DTHL0001`, today's queues, drop rules, control first, backoff, and owner pass, with T4 over today's rules and T5's replay (D12) | Now, the characterization test first | Code review, and Detent's transport suite at its commit (T9), before it lands |
| 3. The keepalive detector inside the policy (D13), counting any frame as heard, with its T4 cases; `Ping` and `Pong` arrive with step 4 | Now, on step 2's branch | The `Peers` preset ignoring its reports, so it stays additive |
| 4. Version 2's wire: the hello, the range, `DTPROF02`, the frame header, kinds 9 to 18, fragments, `Window`, flow control, multiplexing, either-node dialing (D6 to D11), with T3, T6's functional checks, T8's fuzz targets, and T13 | The frame header and kinds 13, 14, and 16 may be drafted on a branch now | This revision's check for the hello, `Window`, `Fragment`, and flow control; C2's bodies for kinds 9 to 12; Detent's agreement (Q4); it lands only after the full design review and the security review clear it, since a wire format freezes once |
| 5. D18's check, `Core::known_members()`, the binding and the witness hook refusing unknown incarnations by default, and admission by the quorum's limits (D5, D8, D18), with T7 | Now, on a branch | The security reviewer's re-check of SR1 (its blocking item) and code review before it lands; Track D3's records before strata's witness |
| 6. Reload with generations, `close_at_expiry`, the key-file path (D14); reactor's PEM option and TLS 1.3 under `strict`, in reactor first | Now | Code review in each repository, and reactor's change landing before consensus uses it |
| 7. With the simulator's contributor, its adapter over `PeerPolicy` (its Q3) | After step 2 | Nothing else: Q3 is decided |
| 8. strata's side: the root node table, certified versions (D16), the admin path (D17), the witness from placement records, enrollment's root side | With Track D3 | This revision's check; the security tier's reviews of D16 and D17's code |

Steps 1, 2, 3, 5, and 6 may start now, and step 4's header and kinds 13, 14, and 16 may be
drafted; nothing in them waits on Q6. Every step but 4 is additive, and each lands as its review
tier requires.

## Status

Designed; nothing is built, run, or measured. The code facts were read at the commits in the
header; every figure is the cited code's or design's, arithmetic, or a labeled estimate.

What the revision check should look at:

1. D5 and D18: whether the binding rules, with rule 3 in place of SR1's first clause, close
   every forged-source path while a lagging replica still catches up, including across
   conflicting uncommitted entries and a removed leader's hand-off.
2. D11: whether the three pools, the FIFO reserve, and the per-peer share bound memory and leave
   no request unserved.
3. D16: whether the signer chain and the certificate give the directory the root's integrity, and
   whether the hint path meets T10's window.
4. D17: the two-admin rule's scope and the rate limits.
5. D9: whether every frozen kind's size and class rule is right, and the close-or-drop table.
6. The threat model's rows, for the security reviewer.
