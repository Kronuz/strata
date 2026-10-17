# Peer transport: identity, versions, and flow control

PLAN's owner decisions SYS-2 and SYS-3 (`docs/PLAN.md:26-33`) put peer identity and wire
versions into one peer transport design, Track C's D16 extended, and every design that names
peer identity defers to it: Track C, the simulator's network, Xapiand's deferred transport (its
D2 to D8), and the PKI helper. This is that design. It covers the identity check, the node
directory that holds the pins and where it lives, version negotiation and what one version
back promises, multiplexing and flow control (Track C's C4), the send policy as a sans-I/O
class with the keepalive failure detector (the simulator's Q3), and the admission rule that
keeps a forged vote out of every group (SYS-14). The code lands in `Kronuz/consensus`
(`consensus::transport`), strata supplies the directory's home, `reactor` does not change, and
Xapiand adopts the transport at Phase 7b.

Read at: consensus `fc66c0e` (`origin/main`; the local `ef30b4f` changes no transport header),
reactor `e19afd9` (consensus's pin), strata `main` `5691ec7`, `design/simulator` `a6c1f4e`
(its design at `0054b30`), Xapiand `master` `7e8cccb7d` and `design/pki` `1fe226809` (its
design at `d05ed6bab`), and Detent `deae42d`, read only. Citations into consensus and reactor
are at those commits; `simulator.md` and `pki.md` are those branches' designs. Nothing here is
built or measured. Every figure comes from the cited code or designs, or is arithmetic or an
estimate and says so.

## Summary

- **Identity.** A peer authenticates by the SHA-256 of its leaf's SubjectPublicKeyInfo (SPKI),
  looked up in a node directory, after the certificate chain verifies as it does today.
  Certificate pins stay valid beside it, so Detent's configuration works unchanged, and a
  renewal that keeps the key changes no pin.
- **The directory** is strata's root authority's node table (Q1). Joins, re-keys, and removals
  change it; a replaced node is one batched entry, about 11 a day at the simulator's nominal
  churn (an estimate). Addresses are hints, since the pin binds the node whatever its address.
- **Admission.** Every frame names its group and its source and destination incarnations
  (per-group replica ids). A receiver admits a frame only when the source incarnation is bound
  to the session's node, by a membership the replica holds or by the group's parent authority.
  A node then speaks only as its own incarnations, so no node can cast another's vote.
- **Versions.** Each binary speaks wire versions N − 1 and N, and a session runs at the highest
  version both speak. Version 2 is the first that negotiates; leaving version 1, which Detent
  pins today, takes one coordinated restart, which Detent already plans.
- **Flow control.** One connection per node pair and lane carries every group, with connection
  and group windows granted by the receiver, control frames sent first, and large frames
  fragmented, so a control frame waits behind at most 320 KiB.
- **One policy.** A sans-I/O class decides everything the transport decides. `Peers` drives it
  over sockets and the simulator drives it over its network.

## What it answers, and what it changes

### Requirements carried

| Requirement | From | Answered by |
|---|---|---|
| One identity model; the directory and its home; the discovery transport; enrollment; the sans-I/O send policy | SYS-2 (`system-2026-10-17.md:193-246`) | D2, D3, D4, D1 with Q3, D15, D12 |
| A version range in place of the hello's profile equality | SYS-3 (`:248-286`) | D6, D7, D8 |
| Lengths and counts bounded before allocation; identities a simulation can supply; no dependence on reactor's UDP loop | SYS-12 (`:514-541`) | D9, D12, D1 |
| No forged vote | SYS-14 (`:560-585`); the owner's ruling (`PLAN.md:45-46`) | D5, D1 |
| The dialer checks the node it meant; the acceptor takes identity from the certificate; one key names one node | TS-R2 | D2, D3 |
| Replay across restarts; the sealed frame's header, nonce, and size | TS-R3, TS-R4 | No datagram path exists (D1); TLS sessions are fresh; boot identities mark restarts (D8) |
| Evicting a compromised node; explicit bootstrap; enrollment; the authority off data nodes | TS-R5 to TS-R8 | D4, D15, Q5; the PKI helper's D3 and D13 |
| Reload without restart, trust bundles, revoked sessions | TS-R12 | D14 |
| No half-secured cluster | TS-R13 | D1 |
| The certificate profile; the SPKI digest's exact bytes | PKI D5, D10, SR12 | D2 |
| Enrollment, decided to belong here | PKI Q6 (the `pki` ledger, 2026-10-17) | D15, Q5 |
| `Peers`' queues, drop rule, control-first order, backoff, and the keepalive detector as one sans-I/O class | Simulator Q3 (`simulator.md:762-773`), SIM-R6 | D12, D13 |
| Connections in order or not at all; no partition oracle on send | SIM-R7 (`simulator.md:233-247`) | D11, D12 |
| A key identity per node, refusals by pin and by version gap | Simulator D4 (`simulator.md:249-261`) | D2, D3, D6 |
| Per-group receive credit; a group's appends on one lane; authentication at scale | Track C's R12 | D5, D10, D11 |
| Control frames ahead of data, a frame cap, bounded unsent bytes, and T7's latency row | Track C's R19 | D11, T6 |
| The hello carries node limits | `track-c.md:42-45` | D8 |
| SPKI pins beside certificate pins; N and N − 1 | Detent's `SHARED_LIBRARY_DECISIONS.md`, sections 1 and 2 | D2, D6, D7 |

### Changes to other documents

Each is the owning document's to make; the lead applies the ones on `main`.

| Document | Change |
|---|---|
| `track-c.md` D1 (`:212-215`) | The directory's home is the root's node table (D4), and a pin is an SPKI or a certificate pin (D2) |
| `track-c.md` D2 (`:138-140`) | `DTPROF02` leaves the wire version out, since the session negotiates it (D6) |
| `track-c.md` D5 (`:305-309`) | "Any incarnation of the group whose node the directory authenticates" becomes D5's binding rule |
| `track-c.md` D12 (`:629-650`) | A coalesced heartbeat's record carries its source incarnation (D5); the detector is D13 here |
| `track-c.md` D16, D18, and the format table (`:182-200`) | D16 and D18's transport points to this design; D18's per-chunk `Credit` goes (D11); tags 13 to 17 and the host ranges join the table (D9) |
| `DISTRIBUTION.md:44, 49, 71, 129-135` | The root holds the node table; the transport row names this design; open decision 2 follows Q3 |
| `simulator.md:134` (cluster state per node) | The per-member state gains the directory's entry, about 72 bytes per member (an estimate, D3) beside the member view's 32 |
| `simulator.md:249-261, 369, 762-773` | Refusals by pin and by version gap are the policy's (D12); `Peers` has its seam once first step 2 lands |
| Xapiand `transport-security.md:118-134, 153-154` | Deferred D2 to D5 and D8 are answered here; answer 2 ("leaf pins: no") is superseded for peers by SYS-2 |
| `pki.md:255-271` | Enrollment, reload, and pins that change without a restart are D15, D14, and D4 |
| Detent `LIBRARY_BOUNDARIES.md:187`, `AUTHORITY_TRANSPORT.md:9, 47` | `DTHL0001` and `DTC1` change only with this versioned change; the profile's role (D6); the configuration as a static directory (D3) |

## Starting point

| Fact | Where |
|---|---|
| One `Peers` per group: `PeerConfiguration` holds one `FixedConfiguration`, its limits, the group's cluster, group, and epoch identities, and the profile | `transport/peers.h:14-19, 27-38` |
| A peer's identity is the SHA-256 of its leaf's DER (`X509_digest`): one pin per voter, unique, the local leaf matching its own pin; the verified leaf is compared with every pin by `CRYPTO_memcmp` | `transport/tls.h:12-20, 21-36, 45-66` |
| Chains verify under reactor's `Peer` role (`verify_peer`, `verify_fail_if_no_peer_cert`) with a TLS 1.2 floor, no renegotiation or early data, and a 64 KiB certificate-list cap; `load_verify_file` trusts every certificate in the bundle | reactor `reactor_tls.h:56-86` |
| The hello `DTHL0001` is 120 bytes: node, lane, cluster, group, epoch, a fresh connection id, and the profile; every field but the connection id must be equal | `transport/hello.h:11-17, 24, 48-54` |
| `DTPROF01` hashes the application's bytes, the cluster and configuration ids, the sorted voters, and nine consensus limits | `transport/profile.h:7-37` |
| Only the lower node id dials; the dialer requires the node it dialed; an acceptor refuses a dialer whose id is not lower, and a second connection per peer and lane | `transport/peers.h:90-94, 343-346` |
| A frame is a 4-byte length bounded before allocation and charged to a byte budget first, read under one deadline from its first byte | `transport/session.h:62-72, 149-171` |
| The codec writes `DTC2` with tags 1 to 8 and no group header, bounding counts and lengths before it allocates | `transport/codec.h:22, 33-50, 121-122, 163-165` |
| Send queues per connection: 4 control frames, appends with entries apart (4 frames, 4 MiB), control first; a full queue drops the frame and counts it | `transport/peers.h:33-36, 125-156, 422-428` |
| The reader holds one decoded event per connection until the Worker admits it, so a busy group stops its connection; the owner pass ends at the first `Busy` | `transport/peers.h:180-182, 368-394` |
| Time comes from `steady_clock` inline; the reconnect backoff is `100 + (peer·17 + lane·31 + reconnects·13) mod 100` ms | `transport/peers.h:171, 190, 329-331` |
| The core admits a `Receive` from any nonzero incarnation but its own; terms decide | `core.h:733-736` |
| A follower asked to replace a committed entry throws, and the Worker fences the replica | `core.h:843-846`; `worker.h:667-676` |
| An incarnation is never reused and never moves to another node | `membership.h:128, 132` |
| `PeerLimits` reports a member's node limits; until the hello carries them, a member nobody reported is taken to match the leader's node | `types.h:214-217`; `track-c.md:42-45` |
| Detent pins consensus at `b7073bb`: `DTC1`, `DTHL0001`, certificate pins | Detent `cmake/dependencies.json` |
| In consensus, `Peers` has one test, an instantiation over a minimal host; Detent keeps the tests that run it | `test/transport/peers_test.cc`; Detent `LIBRARY_BOUNDARIES.md:251` |

With one group per connection, the transport hands the core the authenticated node as the
sender (`peers.h:378-379`), so a fixed voter's identity holds. Once frames name their
incarnations (D9), the transport has to bind them: otherwise any authenticated node can send a
higher-term append into any group on any node it reaches (`core.h:733-736`), and a conflicting
one fences the replica (`core.h:843-846`). D5 is that binding.

## Threat model

| Actor | Capability | Stopped by | Residual |
|---|---|---|---|
| On the network, holding no key | Read, tamper, replay, inject, impersonate | TLS with both chains verified to the cluster bundle and the leaf pinned (D2); fresh connection ids; no plaintext or datagram path (D1) | Traffic analysis. There are 16 handshake slots of 5 s each, so about 3.2 new stalled connections a second keep every slot busy (arithmetic) while peers retry with backoff |
| Holder of a leaf the bundle verifies and the directory does not pin: a `client` leaf, a leaf from a compromised or retired CA | Present it as a node | Pins (D2): no entry, no session | None for peers; a CA alone names no node |
| Holder of a removed node's key | Reconnect | Removed ids are tombstones (D3); sessions close when the removal arrives (D4, D14) | D4's window, at each node that has not applied the removal |
| One compromised node | Act as itself; speak as another node's incarnation; inject into groups not placed on it; forge gossip | Binding (D5) admits only its own incarnations; the parent authority vouches for unknown ones; a candidate counts one grant per incarnation per term; every membership message names its authenticated sender (D1, Q3) | Its own incarnations in the groups placed on it. Raft assumes members that may crash but never lie, so it can vote, append, or fence replicas there as itself until it is removed |
| An authenticated peer flooding | Exhaust memory, starve other groups | Credit for data (D11); counted bounds for control frames; caps checked before allocation; per-group queues | CPU for the frames it sends within those bounds |
| A forger of addresses (gossip, configuration) | Point node n's address elsewhere | The pin binds the node whatever the address (D2, TS-R2) | A failed dial, which costs availability only |
| Operator error | A duplicate pin, a reused node id, a re-key out of order | The directory's invariants (D3); the overlap rule (D4) | A key registered under the wrong node id authenticates as that node |
| Version downgrade | Push a session to an older version | Hellos travel inside TLS; the range has a floor of N − 1 (D6) | A compromised node can offer only N − 1, which is supported, so it costs features only |

## Decisions

### D1. One transport for every node-to-node message

- Everything nodes send each other rides `consensus::transport` sessions: Raft for every group
  kind (the root, placement groups, shard groups, and Detent's fixed groups), snapshot
  transfers, the transport's own control frames, and host frames (D9), which carry Track D2's
  membership gossip and Track D5's shard RPC. Nothing travels in plaintext or as a datagram,
  and no second authenticated channel exists.
- A half-secured cluster (TS-R13) therefore cannot be configured. The replay and nonce rules
  that sealed datagrams needed (TS-R3, TS-R4) do not arise, since each TLS session has its own
  keys and keeps its frames in order. The transport also needs nothing from reactor's UDP loop
  (SYS-12).
- The sans-I/O policy decides (D12) and drivers do I/O: `Peers` over Asio and reactor's TLS,
  and the simulator's adapter over its network. The core and the Worker gain one query (D5).
- Hosts: Detent, one fixed group per process as today; strata, about 1,000 groups per node;
  Xapiand, strata's groups from Phase 7b.

### D2. Identity: SPKI pins beside certificate pins (SYS-2)

- A pin is a kind and a SHA-256 digest. `Certificate` pins hash the leaf's DER, as
  `certificate_fingerprint` does (`tls.h:12-20`). `PublicKey` pins hash the leaf's
  SubjectPublicKeyInfo DER, `i2d_X509_PUBKEY(X509_get_X509_PUBKEY(leaf))`, the digest the PKI
  helper prints (`pki.md:598-606`). `X509_pubkey_digest` hashes only the key's bit string and
  gives another value (SR12), so nothing uses it.
- The chain still verifies first (`reactor_tls.h:83-85`): a pinned key whose chain fails to
  verify to the bundle is refused. The pin then binds the verified leaf to one node id.
- Authentication computes both digests of the verified leaf and looks each up in the directory
  (D3). One match names the node; matches under two different node ids refuse the session as
  ambiguous; no match, or a match on a removed node, refuses it.
- The dialer refuses a session whose node is not the one it dialed (today's rule,
  `peers.h:344`). The acceptor takes the node from the pin, never from the address, and the
  hello's node must equal it (`hello.h:50`). Peers check no host name, so the CN fallback the
  PKI security review found in `bind_hostname` (SR1; `reactor_tls.h:88-101`) never reaches
  them.
- A node refuses to start when its own leaf matches no pin of its own entry (today's local
  check, `tls.h:33-35`).
- Lookup is an ordered index from digest to node, confirmed by `CRYPTO_memcmp`. The
  constant-time scan over every pin (`tls.h:56-62`) is not kept: a pin hashes a public key, so
  a lookup's timing tells an attacker nothing it lacks, and at 100,000 nodes a scan would read
  3.2 MB of digests per handshake (arithmetic).
- Additive: `certificate_fingerprint`, `validate_peer_identity`, and `authenticated_voter` keep
  their signatures and behavior. New: `public_key_fingerprint`, `PinKind`, `Pin`, `Directory`
  (D3), and `authenticated_node(SSL*, const Directory&)`.
- A renewal made with the same key (`pki request --key`, `pki.md:515-516`) leaves a `PublicKey`
  pin as it is, so renewals, 274 a day at 100,000 nodes on 365-day leaves by the PKI helper's
  estimate (`pki.md:257`), never touch the directory. The identity URI in the leaf stays
  advisory for peers (PKI D5); no peer check reads it.

### D3. The node directory

| Field | Bytes | Rule |
|---|---|---|
| Node id | 8 | Nonzero; never reused, since a removed id stays as a tombstone |
| State | 1 | `Joining`, `Active`, `Draining`, or `Removed`; every state but `Removed` authenticates |
| Pins | 33 each | One; two only during an overlap (D4); none once removed |
| Failure domain | 4 | Opaque to the transport; placement reads it |
| Changed at | 8 | The directory version that last changed the entry |

- Invariants, checked by the directory's authority and again by every node that applies a
  change:
  1. A digest of one kind names at most one node id.
  2. A node id is never reused.
  3. An entry holds one or two pins until it is removed, and none after.
- A node whose key is lost or exposed is removed, and its machine rejoins under a new node id
  with a new key. Only a node that still holds its key re-keys in place (D4).
- Addresses are not entries. A node announces its listen endpoints in its hello (D8), and hints
  may come from gossip or configuration; a wrong one costs a failed dial (TS-R2).
- Size: in sorted vectors, a 24-byte entry (id, state, domain, version) and a 48-byte index
  record per pin (digest, kind, node id), so about 72 bytes per member: 7.2 MB at 100,000
  nodes. An estimate from these fields; T12 measures it.
- In consensus, `Directory` is a value type with a version, changed by deltas that report the
  node ids they touch; the policy closes the sessions of removed nodes and of pins that left
  (D14). A static preset builds one from `PeerOptions::addresses` and `pins` (`peers.h:30-31`),
  which is how Detent's configuration file (`AUTHORITY_TRANSPORT.md:47`) supplies it:
  certificate pins, version 0, no deltas.
- Two pins per entry also serve a host that builds its `Directory` directly, as Detent may:
  listing a voter's old and renewed certificate pins together removes the restart order that
  one pin per voter forces (4 of 6 orders lose quorum, PKI-R2), and listing a certificate pin
  beside a `PublicKey` pin is how a voter switches kinds. `PeerOptions::pins` and
  `validate_peer_identity` keep their one pin per voter.

### D4. Where the directory lives, and how it changes

- **Home: strata's root authority**, as a node table beside the placement-group map. The root
  places placement groups on nodes, so it needs the nodes and their failure domains anyway, and
  the table's change rate fits the root's budget of one entry a second (`simulator.md:132`): a
  replaced node is one batched entry, 11 a day at nominal churn and 110 at stress
  (`simulator.md:100`), while restarts and renewals change nothing. DISTRIBUTION's root row
  (`DISTRIBUTION.md:44`) changes to match. Q1 holds the choice.
- **Writers.** Directory commands are root commands, which the root's host accepts only from an
  admin on its admin interface, never over the peer transport: a client whose SPKI pin is in
  the root's admin set, fixed when the root is founded and changed by admins.

| Command | Effect | Refused when |
|---|---|---|
| `Join{node, pin, domain}` | A `Joining` entry | The id was ever used, or the pin names another node |
| `Activate{node}` | `Active`; placement may use the node | The node is not `Joining` |
| `AddPin{node, pin}` | A second pin, for an overlap | The entry has two, the pin names another node, or the node is removed |
| `RemovePin{node, pin}` | Back to one pin | It is the entry's only pin |
| `Drain{node}` | `Draining`; placement moves its replicas away | The node is removed |
| `Remove{node}` | A tombstone; its pins dropped | Never |

- **Join.** An admin registers the node before it starts (D15). It starts with its key, its
  leaf, the CA bundle, and the root's seed: the cluster identity and the root's founding
  members' node ids, pins, and endpoints. It fetches the directory from a root member, and an
  admin activates it once it runs.
- **Renewal** with the same key changes nothing.
- **Re-key**, with the old key still held: `AddPin`; wait until every peer reports holding that
  version (each `Ping` carries it, D13) and the dissemination window below has passed; switch
  the node to its new key and leaf; then `RemovePin` the old one. A node that switches early
  is refused only by the nodes still on the older version, until they fetch. A node whose key
  is lost or exposed is removed instead (D3).
- **Removal** is revocation for peers, as the PKI helper says (`pki.md:580-597`, D9): each node
  that applies it closes its sessions with that node and refuses new ones (D14).
- **Dissemination.** Every node holds the whole table, since host protocols reach any node
  (D1): gossip probes random members, and any node may coordinate a query
  (`DISTRIBUTION.md:64-65`), so shard nodes take fan-out from every node. The directory
  version travels in every hello (D8) and as a gossip hint; a node that sees a newer version
  fetches the delta from a root member, voter or learner, over its own authenticated session.
  A forged hint costs one fetch. A removal takes effect within the hint's dissemination plus
  one fetch. The simulator bounds the spread of one node's death at 45 s at p99 for 99.9% of
  live nodes (`simulator.md:124`), and T10 measures the same window for directory changes.
- A node that cannot reach the root keeps its table: existing nodes keep authenticating, while
  joins and removals wait.
- **Bootstrap.** The root is founded by an explicit command with its seed. A node without a
  seed refuses to start, so starting nodes without one never makes separate clusters (TS-R6).
- If T10 shows the root's fetch load or the removal window missing its target, root voters
  sign each version by quorum and gossip relays it, replacing direct fetches. That changes D4
  and gets its own review.

### D5. Admission: an incarnation speaks only from its own node (SYS-14)

- A frame of a group kind names its group, its source incarnation, and its destination
  incarnation (D9). On a session authenticated as node n, a receiver admits it only when:
  1. it hosts a replica of the group holding the destination incarnation (Track C's D5); and
  2. the source incarnation is bound to n: a membership the replica holds (the founding
     membership, the base membership, or any `Configuration` entry in its retained log,
     committed or not) names it on n, or, when none names it, the host's witness vouches that n
     hosts a replica of the group.
- A source bound to another node is forged: the frame is refused before the core sees it, the
  session closes, and `forged_source` counts it. An unbound source is dropped and counted as
  `unbound_source`, and the policy asks the host's witness, so the frames Raft resends pass once
  it answers. A frame for a group the receiver does not host is dropped as `unhosted`.
- Node-level frames (group zero) are the node's own. Each record of a coalesced heartbeat names
  a group and passes rule 2 by itself, so Track C's D12 record gains its source incarnation,
  8 bytes beside the group, term, destination, and commit (`track-c.md:629-634`).
- The binding is well defined because an incarnation is never reused and never moves
  (`membership.h:128, 132`). The core provides it as `Core::known_members()`, every member that
  a membership the replica holds names (additive), which the host passes to the policy whenever
  the replica's memberships change.
- **The witness** is the group's parent authority, the chain DISTRIBUTION already draws: a shard
  group's placement group records which nodes host its replicas, the root's map records the
  placement groups' replicas, and the root's own members are in its seed, which admins update
  with each root membership change. Fixed groups never consult it, since their members are
  their founding voters, each its own node (`types.h:102`). Until Track D3 exists, strata's
  host passes a static witness from its configuration. A binding the witness vouched for gives
  way to a membership that later names the incarnation, and a candidate checks every grant
  against its own memberships, so a vouched node cannot vote as an incarnation the candidate
  binds elsewhere.
- **Votes.** A candidate counts at most one grant per incarnation per term (the core keys grants
  by incarnation), and a node holds at most one incarnation of a group (Track C's D5). So a
  node's vote counts once, and only the node holding the incarnation can cast it; no vote
  travels any other way (D1).
- **Why a witness.** A replica that lagged past a membership change does not know the new
  leader's incarnation. Refusing it would leave the replica unable to catch up while the group
  runs on its other members; Raft grants votes without consulting the receiver's configuration
  for the same reason (§4.1). Admitting any authenticated node, as Track C's D5 reads today
  (`track-c.md:305-309`), leaves the hole the starting point describes. Q2 holds the choice.
- Xapiand's `ELECT_PRIMARY` votes travel over unauthenticated UDP until Phase 7b retires them
  (SYS-14); the owner's ruling and the failover branch's security review cover that path
  (`PLAN.md:45-46`).

### D6. Wire versions: a range, negotiated per session (SYS-3)

- A wire version is a number naming the hello and the codec's frames, tags, and fields. Every
  binary speaks two: its own N and N − 1.
- The dialer's hello offers its range. The acceptor chooses v, the smaller of the two maxima,
  when v is at least the larger minimum; otherwise both ends close, count `version_refused`,
  and log both ranges. The dialer checks the choice against its own computation. A session
  then encodes and decodes at v: the policy drops a message newer than v and counts it as
  `unsupported`, and receiving one is a protocol error that closes the session.
- **Version 2 is the first that negotiates:** the `DTHL0002` hello (D8) and `DTC2`'s frame
  header with tags 1 to 17 (D9). Version 1 is `b7073bb`'s `DTHL0001` and `DTC1`, which cannot
  negotiate: each side refuses the other's hello by its magic and logs why. The frames consensus
  `main` writes from `0307308` to `fc66c0e` (`DTC2` tags 1 to 8 without the header) are a
  development state that no host deploys, never a supported version.
- So leaving version 1 is one coordinated restart, the "homogeneous DTC2 restart" Detent's
  Group F plans (`LIBRARY_ADOPTION_ARCHITECTURE.md:19, 86`); strata and Xapiand never run
  version 1. Q4 holds Detent's choice.
- **`DTPROF02`** is the SHA-256 of `DTPROF02`, the cluster identity, and the host's profile
  bytes. It leaves the wire version out (Track C's D2 put it in, `track-c.md:138-140`), since
  two versions sharing one fingerprint is what lets them connect. A host keeps its profile bytes
  equal across a rolling upgrade, and anything that differs between versions belongs to D7's
  activation. Detent may keep passing its `DTPROF01` value as the profile bytes, which keeps its
  rule that a different quorum or limit fails admission (`AUTHORITY_TRANSPORT.md:9`).

### D7. What one version back promises

| Promise | Rule |
|---|---|
| Connection | Two binaries whose ranges overlap connect at the highest common version, so a cluster whose nodes run N − 1 or N is fully connected |
| Messages | Every message of N − 1 flows on every session; a message new in N flows only on sessions at N |
| Group features | A feature that needs every member to understand a version-N message (C2's quiescence needs `Wake` and `LeaderGone`) is used by a group only while its leader's sessions to all its members run at N or later; the policy reports each peer node's version to the host |
| Formats | A wire version changes no storage, log entry, or snapshot format. A format change (a new entry kind, membership field, or descriptor) activates only after every node runs N, through an active version the cluster records durably (strata: the root; Detent: its configuration). Before activation a node may roll back to N − 1; after it, an N − 1 binary refuses the new formats, as Track C's readers refuse unknown magics (`track-c.md:130-134`), and never misreads them. Track D8 builds the activation |
| Cadence | N + 1 starts only once every node runs N, and a host release moves its consensus pin by at most one wire version. A gap of two is refused at both ends and counted, which the simulator's T15 checks (`simulator.md:739`) |
| Code | The codec keeps N − 1's branches for one version and N + 1 deletes them, so one implementation carries at most two versions |
| Host frames | Kinds 256 and above (D9) carry the hosts' protocols, which keep the same two-version rule by their own numbers |
| Not promised | Skipping a version, three versions at once, or rolling back below an activated version |

### D8. The hello, `DTHL0002`

Each end sends one inside the TLS session, the dialer first as today (`session.h:126-139`),
under the handshake's deadline. Integers are little-endian, as the codec's `put64` writes them.

| Offset | Bytes | Field | Check |
|---|---|---|---|
| 0 | 8 | `DTHL0002` | Anything else is refused; `DTHL0001` is version 1 |
| 8 | 4 | The hello's whole length | 208 to 4,096, checked before the rest is read |
| 12 | 2 | Lowest wire version | At least 2 |
| 14 | 2 | Highest wire version | At least the lowest |
| 16 | 2 | Chosen version | 0 from the dialer; the acceptor's choice (D6) |
| 18 | 1 | Lane | 1 control, 2 bulk |
| 19 | 5 | Reserved | Zero |
| 24 | 8 | Node id | Equals the pinned node (D2) |
| 32 | 16 | Cluster identity | Equal at both ends |
| 48 | 16 | Connection identity | Fresh, nonzero, and different at the two ends (`tls.h:37-44`) |
| 64 | 16 | Boot identity | Fresh at each process start; a new one tells peers the node restarted (D13) |
| 80 | 32 | Profile (`DTPROF02`, D6) | Equal at both ends |
| 112 | 80 | Node limits: the ten `Limits` fields in declaration order (`types.h:55-67`) | `validate_limits` holds; reported to the core as `PeerLimits` for each of the node's incarnations (`types.h:214-217`) |
| 192 | 8 | Directory version held | 0 for a static directory (D4) |
| 200 | 8 | Reserved | Zero |
| 208 | rest | Extensions: a 2-byte count, then a 2-byte tag, a 2-byte length, and the bytes of each | Within the length; unknown tags skipped. Tag 1: listen endpoints (family, 16-byte address, port), at most 8 |

- The first 16 bytes stay as they are in every later version, so any binary reads any other's
  range; later versions add fields as extensions only.
- Carrying node limits closes Track C's owed item (`track-c.md:42-45`): a member's limits reach
  the core once its node connects.
- The group and epoch identities leave the hello (`hello.h:11-16`) and the configuration
  identity leaves the profile (`profile.h:22-23`), since one connection carries many groups and
  frames name their group (D9). Detent's preset folds its group and restore epoch into the
  16-byte group id, the first 16 bytes of SHA-256 over `DTGROUP1`, the group, and the epoch, so
  a voter of another epoch names another group and its frames drop as `unhosted`, as
  `DTHL0001`'s equality check refused it before.

### D9. Frames and tags: `DTC2`, version 2

After the session's 4-byte length (`session.h:152-167`), every frame starts with:

| Offset | Bytes | Field |
|---|---|---|
| 0 | 8 | Kind |
| 8 | 16 | Group identity; zero for node-level kinds |
| 24 | 8 | Source incarnation; zero for node-level kinds |
| 32 | 8 | Destination incarnation; zero for node-level kinds |
| 40 | rest | Body |

The 4-byte `DTC2` magic leaves the frame, since the session's version names the codec, and
`control_frame_bound` grows from `76 + 32·rpc_entries + rpc_bytes` (`codec.h:10-15`) to
`104 + 32·rpc_entries + rpc_bytes`.

| Kind | Name | Level | Lane | Class | Body | Owner |
|---|---|---|---|---|---|---|
| 1 to 8 | Votes, appends, snapshot responses, pre-votes, `TimeoutNow` | Group | Control | Control; appends with entries are data | Today's, after the kind (`codec.h:51-112`) | C1, fixed |
| 9, 10 | Coalesced heartbeat and reply | Node | Control | Control | Track C's D12, each record with its source incarnation (D5) | C2 |
| 11, 12 | `Wake`, `LeaderGone` | Group | Control | Control | Track C's D12 | C2 |
| 13 | `Ping` | Node | Control | Control | Sequence (8), sender's monotonic time in ns (8), directory version held (8) | D13 |
| 14 | `Pong` | Node | Control | Control | The `Ping`'s sequence and time, echoed | D13 |
| 15 | `Window` | Group or node | Either | Control | Increment (8); wanted (8), nonzero when the sender of data is blocked | D11 |
| 16 | `Transfer` | Group | Bulk | Data | One `RFTXFR01` transfer frame (`snapshot_transfer.h`), whose envelope names the header's incarnations; its `Credit` kind is never sent at version 2 (D11) | C4, D18 |
| 17 | `Fragment` | Group or node | Either | Data | The fragmented frame's kind (8), total length (8), offset (8), then bytes | D11 |
| 256 to 511 | Host control | Group or node | Control | Control | The host's, undecoded, at most 4 KiB | Host |
| 512 and up | Host data | Group or node | Either | Data | The host's, undecoded | Host |

- Every tag here is fixed before the coordinated commit, C2's bodies included, since a tag added
  later is version 3 (`track-c.md:177-180`).
- Every new decoder bounds each length and count before it allocates, as the codec does today
  (`codec.h:121-122, 163-165`) and as Detent asks of the library's recovery
  (`LIBRARY_ADOPTION_ARCHITECTURE.md:90`; SYS-12).
- The snapshot channel keeps its sessions, one per bulk connection and group
  (`snapshot_channel.h:33-35`).
- The C1 frame fixture (`test/fixtures/c1/frame-timeoutnow`) is regenerated in version 2's
  envelope with its body unchanged.

### D10. Connections

- One connection per node pair and lane (Track C's D16). The control lane carries every group's
  Raft frames, keepalives, windows, and host control frames; the bulk lane carries transfers
  and host data. A group's appends never split across lanes (R12).
- **Peer connections** join nodes that share a group: the union of the local groups' members,
  through the directory. They open on demand and close 60 s after no group is shared.
- **Contact connections** carry host frames to nodes that share no group: gossip probes (Q3),
  directory fetches, and query fan-out. They live in a least-recently-used cache of at most 128,
  on the control lane unless a host data frame needs the bulk lane, and close after 30 s idle.
  So neither kind grows with the cluster: placement bounds peers at 64 (Track C's D12), and the
  cache bounds contacts.
- Peer and contact are reasons to keep a connection. A node pair has at most one connection per
  lane whatever its reasons, and a contact that comes to share a group becomes a peer.
- Either node may dial. When both dial at once, the connection the lower node id dialed stays
  and the other closes, the same choice at both ends. Today only the lower id dials
  (`peers.h:90-94, 344`), which would leave a higher id unable to reach a contact.
- Caps per node: 16 handshakes in flight (today 2, `peers.h:33`, while a restart brings up to
  128 redials at once), each under today's 5 s deadline (`peers.h:37`), and 2·64 + 128 + 16
  connections.

### D11. Flow control (C4)

- **Credit** counts the bytes of data frames: appends with entries, `Transfer`, `Fragment`, and
  host data. Each connection has a window and each group on it a window of its own (Track C's
  D16; RFC 9113 §5.2 and RFC 9000 §4 do the same), granted by the receiver in `Window` frames;
  node-level data spends a window of its own, as group zero. A sender never sends data past
  either window, and a receiver closes a connection that does and counts `credit_violation`.
- **Initial windows**, fixed by the wire version: 1 MiB per connection; per group, 64 KiB on the
  control lane and none on the bulk lane, where a transfer asks first.
- **Growth.** The receiver returns a group's credit as the group's Worker admits the bytes,
  coalesced into one `Window` per group when half its window is consumed or 10 ms have passed.
  It raises an active group's window toward its maximum (D15's `window_bytes`, 4 MiB, on the
  control lane; D18's 16 chunks of 64 KiB on the bulk lane, `track-c.md:733-737, 777-779`), and
  a connection's toward 64 MiB, while the node's outstanding grants stay within its incoming
  budget: 256 MiB, D15's node send budget (`track-c.md:744-747`). It never grants past the
  budget.
- **Blocked.** A sender whose next data frame exceeds its credit sends `Window` with increment 0
  and the bytes it wants. The receiver grants at least that once the group's queue is empty,
  within the node ceilings (`track-c.md:163-166`: commands up to 64 MiB) and the budget. That is
  how an entry larger than a window travels.
- **Control frames need no credit.** A receiver keeps at most 8 pending admission per group and
  connection, and 16 host control frames per connection, and drops the rest, counted; Raft and
  the hosts resend. Node-level control frames run at once and are never queued, so a `Window` is
  never dropped, and a sender keeps at most one pending `Window` per group, adding increments.
  Credit therefore always returns while the connection lives, and neither credit nor control
  frames wait on each other.
- **Control first (R19).** A sender's control frames go before any data frame. A data frame over
  256 KiB (`Flow::frame_bytes`, `types.h:123-127`) goes as 256 KiB `Fragment`s interleaved with
  other groups' fragments and with control frames, reassembled per group and connection, with
  the whole frame's credit held from its first fragment. Unsent socket bytes stay under 64 KiB
  through `TCP_NOTSENT_LOWAT` (the macOS SDK defines it, `netinet/tcp.h:246`; where a platform
  lacks it, the driver bounds the send buffer instead, and T6 states that platform's figure). A
  control frame then waits for at most one fragment and 64 KiB, 320 KiB or about 2.6 ms at
  1 Gb/s (arithmetic), whatever the entry's size. Without fragments, a 64 MiB entry would hold
  it about 0.54 s.
- **Queues.** Senders keep per-group queues (`pipeline` frames and `window_bytes` bytes, D15)
  served round-robin; receivers keep per-group queues bounded by each group's window, so a group
  waiting on its barrier holds only its own credit (Track C's R12).
- **Resets.** A closed connection resets both ends: windows return to their initial values, and
  each end discards the frames queued for it and those received on it but not yet admitted,
  which keeps memory bounded across reconnects; Raft resends them. Counting credit without
  acknowledgments rests on TCP's delivery in order or not at all, which the simulator models
  (SIM-R7, `simulator.md:233-247`).
- **Snapshots.** The snapshot channel's per-chunk `Credit` (`snapshot_transfer.h:29`; D18 as
  built at `fcc18e6`) goes, and chunks spend the group's bulk-lane window, so one mechanism paces
  snapshots (Track C's R10).

### D12. The policy: one sans-I/O class (the simulator's Q3)

`consensus::transport::PeerPolicy` decides everything the transport decides. It takes events
and the time and returns actions; it reads no clock, socket, or random source.

| Inputs | Outputs |
|---|---|
| `tick(now)`, in nanoseconds | `Dial{node, lane, endpoint}` |
| `want(node, why)`, `release(node)`: the host's peers and contacts | `Close{connection, reason}` |
| `dialed`, `accepted`, `handshaken{connection, certificate digest, key digest, hello}`, `closed{connection, reason}` | `Hello{connection, bytes}` |
| `received{connection, header, body size}` | `Write{connection, frame}` |
| `writable{connection, unsent bytes}` | `Deliver{group, source, frame}` to the group's Worker |
| `admitted{group, connection, bytes}`, `busy{group}` | `Vouch{group, node}` to the host's witness (D5) |
| `vouched{group, node, answer}` | `PeerState{node, up, down, or restarted}` (D13) |
| `send{group, source, destination, kind, bytes}` from a Worker or the host | `PeerReport{node, version, limits}` (D6, D7, D8) |
| `directory(delta)`; `members(group, known members)` (D5) | Counters by reason: drops, refusals, `forged_source`, `unbound_source`, `unhosted`, `version_refused`, `unsupported`, `credit_violation` |

- **It owns** the queues and their drop rules, control first, round-robin service of groups at
  both ends, credit and fragment bookkeeping (D11), dial ownership, today's backoff schedule
  (`peers.h:329-331`), the handshake and connection caps, pin lookup (D2), hello validation and
  the version choice (D6), the binding check (D5), and the keepalive detector (D13).
- **The driver owns** sockets, TLS through `reactor::tls`, the two digests of the verified
  leaf, framing, timers that call `tick`, fresh connection and boot identities from `RAND_bytes`
  (`tls.h:37-44`), and posting a delivery to its group's executor. `Peers` is that driver over
  Asio. The simulator's adapter is the other, over its network (`simulator.md:226-270`), so the
  refusals by pin and by version gap its S2b counts are the policy's own.
- **Determinism**, as the simulator's D2 requires (`simulator.md:152-210`): ordered containers
  wherever iteration reaches an output, ties broken by node id and then group id, no ordering by
  pointer, and time and identities as inputs.
- **Receive admission** serves groups round-robin, one frame per group per pass, and skips a
  group that answers `Busy` or `Pressure` without ending the pass, so one group waiting on its
  barrier stalls no other (today's pass ends at the first `Busy`, `peers.h:180-182`). The driver
  submits an admitted frame as `Receive{source incarnation, message}`.
- `Peers` keeps its API for Detent: a `Peers<Host>` is the node transport with one group, built
  from `PeerConfiguration` and `PeerOptions`, with the static directory of D3 and the group id
  of D8.

### D13. The keepalive failure detector

- Each end of a node pair's control connection sends `Ping` every k = 500 ms and answers every
  `Ping` with `Pong`. A peer is down after 3k (1.5 s) without a frame from it, or after three
  `Ping`s in a row go unanswered, which catches a peer that sends but no longer reads. Any frame
  counts as hearing from it, and its first frame brings it back up (Track C's D12,
  `track-c.md:647-650`).
- A session whose hello carries a new boot identity reports the peer `restarted`, which Track C's
  D12 uses to wake that node's groups without an election.
- Each `Ping` carries the directory version its sender holds, so a node learns its peers'
  versions without a new hello: a newer one prompts a fetch (D4), and a re-key waits for its
  peers' (D4).
- The detector reports and the host acts (Track C's D12 and R18): followers whose leader's node
  is down wake, and a quiescent leader whose quorum's nodes are down steps down. It decides
  nothing for a group by itself.
- Cost: 2P/k frames sent per node per second, 256 at P = 64 distinct peers (Track C's D12). The
  catalog's `cluster::LivenessTracker` reads `steady_clock` and takes a mutex, so it cannot run
  in the policy, and `cluster` is not carried forward (`simulator.md:376`). Track D2's gossip
  later shortens detection.

### D14. TLS settings, reload, and session lifetime

- Peers use reactor's `Peer` role (`reactor_tls.h:83-85`) and `strict` (forward-secret AEAD
  suites in TLS 1.2, no tickets or session cache, `reactor_tls.h:70-75`) when the host sets a
  new `PeerOptions::strict`, which defaults to off so Detent's contexts stay as they are; strata
  and Xapiand set it.
- **Reload.** On the host's request the driver builds a new context from the host's files and
  uses it for new sessions; a failure keeps the old context and logs why (TS-R12). A same-key
  renewal is a reload and changes no pin.
- When the trust bundle changes, each session's peer chain verifies again against the new
  store, and a session that fails closes (TS-R12). The PKI helper's planned rotation
  (`pki.md`, D7) then needs no restart of peers: its bundle and leaf steps are reloads.
- A session closes when its peer leaf's notAfter passes, since TLS checks validity only at the
  handshake, and the peer reconnects with its current leaf.
- A session closes when its pin leaves the directory or its node is removed (D3, D4).
- Nothing changes in reactor: the context belongs to consensus (`peers.h:466`), and
  `TCP_NOTSENT_LOWAT` is a socket option the driver sets.

### D15. Enrollment

- **Now, offline.** A node's key, request, and leaf come from the PKI helper (`pki request`,
  `pki sign`; its D2 and D6), and an admin registers the key's SPKI pin with `Join` (D4). That
  serves the helper's stated bound of 1,000 nodes per authority, an estimate (`pki.md:241-254`).
- **At 100,000 nodes** offline issuance does not hold (`pki.md:255-271`), so online issuance
  follows as its own design, in the security tier, after this one's preliminary review, with
  TS-R5 to TS-R8 as its requirements (Q5). Its shape, which D4's commands already serve:
  - A renewal with the same key travels over the node's own pinned session to an issuer, which
    signs it without an operator; the directory does not change.
  - A join spends a single-use token an admin mints for a reserved `Joining` entry. The token
    binds the node id, the issuer's SPKI pin, an expiry, and a 256-bit secret; the joiner proves
    the secret over a TLS session to the issuer's enrollment listener, which presents its leaf
    and asks for none (reactor's `Server` role); and the root commits `Enroll{token, node, pin}`,
    which spends the token atomically and repeats the same answer for the same pin, before the
    issuer returns the leaf.
  - The issuing key is an online intermediate on an issuer host, never a data node (TS-R8),
    under the offline root. The helper's first CA generation has `pathlen:0`, so an intermediate
    arrives with a rotation to a generation that allows one (`pki.md:462-465`).
  - Evicting a compromised node (TS-R5) is a directory removal; peers need no CA rotation, since
    pins decide.

## Cost at the scale contract

Per node, at P = 64 distinct peers (Track C's D12 bound for placement), 1,000 groups with three
members each, and n = 100,000 nodes. Estimates say so; T12 replaces them with measurements.

| Quantity | Formula | At the contract | Basis |
|---|---|---|---|
| Peer connections | 2P | 128 | D10 |
| Contact connections | At most the cache | 128 | D10 |
| Keepalive frames sent per second | 2P/k | 256 | D13 |
| Directory | About 72 bytes × n | About 7.2 MB | Estimate (D3) |
| Connection and group pairs | The local groups' other members | About 2,000 | Arithmetic |
| Policy state per pair | About 64 bytes; queues allocate only while they hold frames | About 128 KB | Estimate |
| Receive memory | Incoming budget, plus control frames pending | At most 256 MiB, plus 1.8 MB of Raft control frames (2,000 pairs × 8 × 112 bytes, the largest being an append response with its compacted boundary) and 16 MiB of host control frames (256 connections × 16 × 4 KiB) | D11 |
| Initial grants | P × the connection window | 64 MiB | D11 |
| Handshakes at a restart | 2P, at the restarting node | 128 | D10 |
| Handshakes per second at rest | Contact misses | About 2, one probe sent and one received, if gossip probes ride contacts and every probe misses the cache; up to 4 while indirect probes run | Estimate from SWIM's 1 s period (`simulator.md:123`; Q3) |
| Directory entries per day | One per replaced node | 11 nominal, 110 at stress | `simulator.md:100` |
| Fetches per directory entry | One per node, over the hint's spread | 100,000 over about 45 s | `simulator.md:124` |

## Limits and presets

Limits name the mechanism that needs them (`docs/design/limits.md`). Presets are tuning
defaults, measured before they become defaults for good.

| Limit | Value | Mechanism |
|---|---|---|
| Hello length | 208 to 4,096 bytes | Read in two steps, so a peer not yet admitted allocates at most 4 KiB |
| Pins per node | 2 | An overlap needs the old and the new; a third would keep a retired key valid |
| Wire versions per binary | 2 | SYS-3 |
| Listen endpoints per hello | 8 | Bounds the extension's decode |
| Host control frame | 4 KiB | Control frames take no credit, so their size and count bound their memory |
| Control frames pending | 8 per group and connection; 16 host control frames per connection | Raft and the hosts resend |
| Data frame | `control_frame_bound` on the control lane; `maximum_transfer_bytes` plus 40 on the bulk lane | Today's caps (`peers.h:350-352`) with the header |
| Contact connections | 128 | Keeps connections independent of n |
| Handshakes in flight | 16 | A restart's 128 redials within a bounded memory |

The windows, the incoming budget, the fragment size, the unsent bound, the keepalive timing,
the linger and idle times, and `Window`'s coalescing (D10, D11, D13) are presets: their
values come from D15, D18, R19, and Track C's D12, and measurements on `main` replace them.

## Library changes

**Additive** means nothing Detent calls today changes behavior unless Detent opts in.

| Change | Repository | Additive for Detent | When |
|---|---|---|---|
| `public_key_fingerprint`, `PinKind`, `Pin`, `Directory` with its static preset, `authenticated_node` | consensus | Yes: the old functions and certificate pins are unchanged | First step 1 |
| `PeerPolicy`, with `Peers` driving it at today's bytes | consensus | Yes: a relocation, with the wire and the API unchanged | First step 2 |
| `PeerOptions::strict` and a send low-water mark | consensus | Yes: off by default in the `Peers` preset | With step 4 |
| Version 2's wire: `DTHL0002`, the range, `DTPROF02`, the frame header, tags 9 to 17, fragments, `Window`, `Transfer`, multiplexing, the keepalive frames and their settings; the snapshot channel's `Credit` retired | consensus | No: the wire changes in the coordinated `DTC2` commit (Track C's R15), which Detent adopts at its planned restart (Q4); `Peers`' API stays | After the full design review and the security review clear it |
| `Core::known_members()`, and the witness hook, which refuses unknown incarnations by default | consensus | Yes: a query and a hook that fixed groups never reach | First step 5 |
| Context reload and re-verification | consensus | Yes: unused unless called | First step 6 |
| Nothing: the context is consensus's, and socket options are the driver's | reactor | Not applicable | Never |
| The root's node table and its commands, the directory client, and the witness from placement records | strata, Track D | Not applicable | Q1, Q2, Track D3 |
| The activation of format versions | strata, Track D8 | Not applicable | Track D8 |
| Peer key and certificate flags, the seed, the directory client; `ELECT_PRIMARY` and UDP discovery retired | Xapiand | Its own command line, additive | Phase 7b |

## Limits of this design

- **Directory state grows with n.** Pins need every node to hold every node's entry, about
  7.2 MB at 100,000 nodes (an estimate). Identity by a certificate name under the cluster CA
  would need no table; SYS-2 chose pins. The threat model states what a compromised node keeps.
- **Revocation waits for dissemination.** A removed node keeps its sessions at each node until
  that node applies the removal: about 45 s at p99 plus a fetch, by the simulator's bound, never
  measured. The gap is silent at the nodes still holding the old version.
- **A stale witness vouches for a former host.** A placement record that lags a move vouches for
  the node that left, which matters only if that node is compromised, for as long as the record
  lags.
- **Format activation is unbuilt.** Until Track D8 exists, "every node runs N before a format
  changes" holds by procedure only. Skipping it fails closed and silently at first: an N − 1
  node refuses the new records and fences that replica.
- **A host release that skips a wire version** is refused by every older node. The counters show
  it, and the cluster runs on the nodes that still connect.
- **Without `TCP_NOTSENT_LOWAT`** on a platform, the control-latency bound becomes the send
  buffer's size; FreeBSD is unverified, and T6 states each platform's figure.
- **Probe handshakes.** If gossip rides contact connections (Q3), each probe outside the cache
  costs a TLS handshake, whose CPU cost no run here measures.
- **A stalled event loop reads as a failure.** A node whose loop stalls past 1.5 s is reported
  down by its peers' detectors, silently from its own point of view.

## Gate rows

Each row runs in `consensus` at the item's commit on Linux, macOS, and FreeBSD unless it says
otherwise. Performance rows (T6, T11's figures, T12) are the lead's, measured on `main`.

| Row | Target | Procedure |
|---|---|---|
| T1 Pins | `public_key_fingerprint` equals `openssl x509 -pubkey -noout \| openssl pkey -pubin -outform DER \| openssl dgst -sha256` for P-256, P-384, and RSA 2,048 leaves, differs from `X509_pubkey_digest`, and equals the PKI helper's fingerprints on its T10 fixtures. A same-key renewal authenticates under a `PublicKey` pin and is refused under the old `Certificate` pin, beside the renewed `Certificate` pin accepted. Refused, each beside an accepted case differing in one variable: a pinned key whose chain is outside the bundle; a leaf whose two digests name two nodes; a removed node; a local leaf matching no pin of its own entry. Detent's authentication cases pass unchanged through the old functions | `test/transport/authentication_test.cc` over memory BIO pairs; `tls_certificates.cmake` gains P-256 and P-384 leaves, a same-key renewal, and a second CA; CTest `consensus_transport_authentication` in CI |
| T2 Directory | Each D3 invariant and each D4 refusal, broken and refused beside the accepted case. Applying a delta closes exactly the sessions of removed nodes and of departed pins, and no other. A re-key keeps every session at every step of its overlap. A lost key's replacement (removal, then a join under a new id) closes the old id's sessions and refuses its key | `test/transport/directory_test.cc` over `PeerPolicy` with a fake clock and no sockets |
| T3 Versions | A test-only version 3 (one extra tag, one extra trailing field) runs the transport suite at (3, 3), (3, 2), and (2, 3) at the chosen version, with version-3 messages dropped and counted on version-2 sessions. A test-only version 4 offering 3 to 4 is refused by version 2 at both ends, logged with both ranges, and counted. A `DTHL0001` hello from `b7073bb` is refused with its reason, and `b7073bb`'s reader refuses a `DTHL0002` hello. Every version-2 tag round-trips against its fixture, and a version-3 build reads a version-2 hello's range and the reverse | `test/transport/version_test.cc` with `CONSENSUS_TRANSPORT_TEST_NEXT`; fixtures in `test/fixtures/v2/`; `b7073bb`'s reader from its own worktree, as Track C's T1 (`track-c.md:804-809`) |
| T4 Policy | Over 10^6 seeded random event sequences, checked after every step: no data past credit; each end's credit equal to its grants less the data sent, never negative; a control frame behind at most one fragment; a group without credit delaying no other; every group with pending frames served within one round of the active groups; queue bounds held, every drop counted by reason; a simultaneous dial settled on the lower id's connection at both ends; backoff from 100 to 199 ms; the detector reporting down at exactly 3k of silence and after three unanswered `Ping`s, never earlier, up at the first frame, and `restarted` on a new boot identity; the same actions for a seed on every run and platform. Each mutant caught within the budget, its first seed recorded: control first off; the credit check off; a detector threshold of 2; `Busy` ending the pass; a fragment accepted out of order | `test/transport/policy_test.cc`, each seed's action hash pinned; `tools/mutants.sh` |
| T5 The driver decides nothing | A three-process loopback run of `Peers` records the policy's inputs, and replaying them through `PeerPolicy` alone gives the run's actions exactly; the simulator's adapter runs the same class | `CONSENSUS_TRANSPORT_TRACE` builds of `Peers`; `test/transport/replay_test.cc`; the simulator's adapter test |
| T6 Flow control | With 100 groups on one connection and one group's barrier delayed 10×, every other group's commit p99 within 1.5× the unslowed run (Track C's T7). Control-frame latency p99 at most 10 ms under saturating appends on a 1 Gb/s link, stated per platform with and without `TCP_NOTSENT_LOWAT` (R19's owed row). Receive memory never above the incoming budget plus the control bounds, and a 64 MiB entry crossing as fragments with control frames between them, as tests | `bench/transport_flow` on Linux under `tc qdisc ... netem rate 1gbit`, and on macOS and FreeBSD over loopback, interleaved trials as BENCHMARKS' rules state; the memory and fragment checks in `test/transport/flow_test.cc` |
| T7 Forged sources (SYS-14) | On a session authenticated as node X, frames as an incarnation the replica binds to node Y (a vote grant, a pre-vote, a higher-term append, `TimeoutNow`, a coalesced record, `LeaderGone`) are refused before the core, counted as `forged_source`, and close the session, beside the same frames from Y's session admitted. An unknown incarnation is admitted once the witness vouches for X, and dropped as `unbound_source` when it does not. A frame for a group not hosted drops as `unhosted`, and a duplicated grant counts once. A plaintext client and a TLS client with an unpinned key reach no decoder (decode count 0). In T2's randomized harness, one node per seed sends forged-source frames at every step: none is admitted and every T2 oracle holds; the mutant with the binding check off is caught within 400 seeds | `test/transport/admission_test.cc`; `test/simulation_test.cc` with a forging node; `tools/mutants.sh` |
| T8 Robustness | One CPU-hour of fuzzing for each new decoder (the hello and its extensions, the frame header, `Ping` and `Pong`, `Window`, the `Transfer` header check, `Fragment` reassembly, and the directory delta), zero findings with a hang timeout, at the gate's commit, each corpus replayed as a test. A hello or frame declaring its largest legal length allocates nothing before its credit. ASan, UBSan, and TSan over every transport test. Line and branch coverage of the changed headers at TESTING's targets (decoders at 98% and 95%, the rest at 90% and 80%), which takes up the C1 gate's ruling that the transport headers' coverage goes with C4 (`track-c-design.md:1967-1969`) | `tools/fuzz.sh` per decoder, run by the test engineer under a host hold; an allocation-counting test; `tools/coverage.sh` |
| T9 Detent | At Detent's current pin nothing changes. At the coordinated commit, Detent builds from a clean clone and passes its suite run serially with certificate pins in a static directory and its `DTPROF01` value as the profile, and a `b7073bb` voter beside a version-2 voter refuses with a logged reason | Detent's maintainer's Group F qualification: the clean-clone suite of `LIBRARY_BOUNDARIES.md:199`, run serially as Track C's T1 requires (`track-c.md:804-809`) |
| T10 Simulator | Tier A at 100,000 nodes: a removed node's sessions closed at every node within 60 s at p99 of the root's commit, with the root's fetches per entry and their peak rate stated; T15's rolling upgrade with refusals decided by `PeerPolicy` (none between N and N − 1, every session refused at a gap of two); the group-failover bound (`simulator.md:128`) with D13's detector | `sim_run --tier a` and the upgrade scenarios (`simulator.md:739`), once the simulator's S2b and adapter drive `PeerPolicy`; budgets as the simulator's rows state |
| T11 Real network | On loopback with three processes and on a test cluster of at least 30 nodes (PLAN's Track D rule): a rolling restart of every node between version 2 and the test-only version 3, with no group losing quorum and no refused session; a same-key renewal and a bundle change applied without a restart, closing exactly the sessions that no longer verify; a re-key with overlap; a removal closing sessions everywhere; RSS per connection at 128 and 256 connections stated | Restart and reload scripts over Phase 3R's real-node harness; RSS from `/proc/<pid>/status` on Linux and `ps -o rss` on macOS |
| T12 Scale statement | The cost table's estimates measured: directory bytes per entry at 100,000 entries, policy bytes per pair at 2,000 pairs, frames per second at rest at P = 64, and TLS handshakes per second per core | `bench/transport_scale` over a synthetic directory and groups, on the allocation-counting build; PLAN's scale statement (`PLAN.md:181-182`) |

## Open questions for the owner

**Q1. Where does the node directory live?**

1. The root authority's node table, beside the placement-group map (D4).
2. A directory group of its own, a strict-consensus group beside the root.
3. Spread across placement groups by node-id range.

Recommendation: 1. The root places placement groups, so it needs the nodes and their domains
anyway, and the table's tens of changes a day fit its budget. Option 2 adds an authority to
seed and run; option 3 makes each node fetch from many authorities with no load to justify it.

**Q2. How does a replica bind an incarnation that no membership it holds names?**

1. The parent authority's placement record, through the host's witness (D5). A compromised node
   affects only the groups placed on it.
2. Membership certificates: each change to a group's membership signed by a quorum of the
   previous membership's nodes with their node keys, chained back to a membership the replica
   holds. A compromised node then affects only groups where it holds a quorum, and no host
   lookup is needed, at about three signatures per change for five voters (arithmetic) and
   new signing code in consensus.
3. Track C's D5 as written: any authenticated node. One compromised node can append into any
   group on any node it reaches and fence its replicas.

Recommendation: 1. It reuses the chain of authorities DISTRIBUTION already draws and adds no
cryptography to consensus. Option 2 is the path if a stale record's window proves too wide;
SYS-14's reasoning rules out option 3.

**Q3. What carries membership gossip (Track D2; DISTRIBUTION's open decision 2)?**

1. Host control frames on contact connections of this transport (D1, D10): one identity
   mechanism, every message attributed to its sender, and no replay or nonce rules of its own.
   It costs a TLS handshake for each probe outside the cache, about 2 a second per node and up
   to 4 (an estimate), and probe targets are dialed one period ahead so a probe's timeout
   counts only the peer's answer.
2. A datagram path beside the transport, signed or MACed per sender: cheaper per probe, but a
   second authentication mechanism, with TS-R3's freshness and TS-R4's nonce rules to design.
3. Datagrams sealed under one cluster key, as transport-security's deferred D3 proposed: the key
   shows that a datagram came from some member and never which one (TS-R5), so any node can
   forge another's suspicion.

Recommendation: 1, with T12's handshake rate measured before Track D2 builds on it.

**Q4. How does Detent leave version 1?**

1. One coordinated restart into version 2, as its Group F plans
   (`LIBRARY_ADOPTION_ARCHITECTURE.md:19, 86`), keeping certificate pins and its `DTPROF01`
   value.
2. A dual-codec transition, the alternative Detent names (`DEPENDENCY_UPGRADES.md:41`): the new
   transport also speaks `DTHL0001` and `DTC1` to old voters, two wire paths, which Track C's D2
   rules out (`track-c.md:135-137`).

Recommendation: 1. Every later move is a rolling one (D6, D7). Detent's maintainer decides, as
SYS-3 asks (`SHARED_LIBRARY_DECISIONS.md`, section 2).

**Q5. Is online enrollment designed here or on its own?**

1. On its own, in the security tier, after this design's preliminary review, with D15's shape
   and TS-R5 to TS-R8 as requirements. This design fixes only the directory's side: reserved
   `Joining` entries and `Enroll`'s atomic spending of a token.
2. Here, in this design's next revision.

Recommendation: 1. The PKI decision sent enrollment here (its Q6), and D15 carries its shape,
but its listener serves nodes that hold no certificate yet, an attack surface that deserves a
review of its own, and nothing below the helper's 1,000-node bound needs it (PKI D2).

## First steps

None waits on a question. Every step but 4 is additive, and each lands as its review tier
requires.

1. `public_key_fingerprint`, `PinKind`, `Pin`, `Directory` with its invariants and its static
   preset, and `authenticated_node`, with T1 and T2's invariant cases.
2. `PeerPolicy` extracted from `Peers` with today's rules and bytes: one group, `DTHL0001`,
   today's queues, drop rules, control first, backoff, and owner pass, with `Peers` driving it,
   T4 over today's rules, and T5's replay. A relocation; neither the wire nor the API changes.
3. The keepalive detector inside the policy, counting any frame as heard (an active group's
   heartbeats arrive every 100 ms), with its T4 cases; `Ping` and `Pong` arrive with step 4.
4. Version 2's wire: the hello, the range, `DTPROF02`, the frame header, tags 9 to 17,
   fragments, `Window`, `Transfer`, and multiplexing, with T3, T6's functional checks, and T8's
   fuzz targets. It is built ahead on a branch and lands only after the full design review and
   the security review clear it, since a wire format freezes once.
5. The binding rule: `Core::known_members()` and the witness hook, refusing unknown incarnations
   by default, with T7.
6. Reload and re-verification (D14).
7. With the simulator's contributor, its adapter over `PeerPolicy` (its Q3).

Waiting on the questions: strata's node table and directory client (Q1), the placement witness
(Q2, Track D3), the gossip carrier (Q3, Track D2), Detent's adoption (Q4), and online enrollment
(Q5).

## Status

Designed; nothing is built, run, or measured. The code facts were read at the commits in the
header; every figure is the cited code's or design's, arithmetic, or a labeled estimate.

What review should look at:

1. D5 and Q2: whether the binding rule closes every forged-source path without costing a
   lagging replica its catch-up.
2. D4: whether dissemination by hint and fetch, and its removal window, hold at 100,000 nodes.
3. D7: whether the promise is enough for Track D8 and the simulator's T15.
4. D11: whether credit, control frames, and `Blocked` requests can deadlock or starve.
5. D12: whether anything that decides is left in the driver.
6. The threat model's rows, for the security reviewer.
