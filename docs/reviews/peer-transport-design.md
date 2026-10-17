# strata review: the peer transport (preliminary)

- Date: 2026-10-17, read from 15:05 to 15:50 PDT.
- Reviewed: `docs/design/peer-transport.md` revision 0 at `8a1f840` (unchanged at the branch
  head `3015cab`, which only records the commit in the ledger) and `docs/status/peer-transport.md`
  at `3015cab`, on `design/peer-transport`, which is up to date with `origin`.
- Checked against:
  - PLAN on `main`: the process rule, the owner decisions SYS-2, SYS-3, SYS-14 (`PLAN.md:22-49`),
    Goal 3, "Every gate", Track C (C4, its ordering against Detent's split), Track D (D2, D3, D8).
  - `docs/design/track-c.md` (D1, D2, D5, D12, D15 to D18, the format table, the site table,
    T1, T7) and every check in `docs/reviews/track-c-design.md` (R10, R12, R19, conditions 1
    and 22, the C1 gate's ruling on the transport headers' coverage).
  - `docs/reviews/system-2026-10-17.md` (SYS-2, SYS-3, SYS-12, SYS-14, SYS-16's calibration
    check) and `docs/DISTRIBUTION.md`.
  - The simulator, `origin/design/simulator` at `a6c1f4e` (its design unchanged since
    `0054b30`), and its review `docs/reviews/simulator-design.md` there (SIM-R6, SIM-R7, Q3).
  - Xapiand `master` `7e8cccb7d`: `docs/design/transport-security.md` (deferred D2 to D8, the
    answers) and `docs/reviews/transport-security-design.md` (TS-R2 to TS-R8, TS-R12 to TS-R17).
  - Xapiand `design/pki` at `bfc11c50c` (local only, no `origin`; `pki.md` unchanged since
    `d05ed6bab`), its ledger with the owner's decisions, and both PKI reviews.
  - Code: `consensus` at `fc66c0e` (`origin/main`; the local `ef30b4f` adds two unpushed
    commits that change no transport header and touch `core.h` only at line 1138), its
    `transport/` headers, `core.h`, `membership.h`, `types.h`, `worker.h`,
    `snapshot_channel.h`, `snapshot_transfer.h`, its tests and fixtures; `b7073bb` and
    `d7192f7` where Detent pins or names them; `reactor` at `e19afd9` (`reactor_tls.h`, equal to
    `origin`); `Kronuz/lru-cache` (`lru.h`).
  - Detent at `deae42d`, read only: `SHARED_LIBRARY_DECISIONS.md`, `AUTHORITY_TRANSPORT.md`,
    `PERSISTENT_TRANSPORT_ARCHITECTURE.md`, `LIBRARY_ADOPTION_ARCHITECTURE.md`,
    `DEPENDENCY_UPGRADES.md`, `LIBRARY_BOUNDARIES.md`, `cmake/dependencies.json`, and how it
    builds `PeerConfiguration` and `PeerOptions` (`src/detent_authority/peers.hh`,
    `service.cc`, `tests/detent_authority/test_transport_peers.cc`).
  - `docs/reviews/calibration.md` on `main` at `bcf89c8`: five rows. Two landed during this
    review (`bcf89c8`, 15:36); all five are applied below.
- Checked by:
  - Reading every file:line the design cites at the commit its header names. All but the ones
    named in the findings match.
  - An arithmetic probe, `~/.scratch/review-peer-transport/arith.py`, output in `arith.out`
    beside it: the hello's and frames' sizes, the windows against the budget, control-frame
    waits, `Window` frame rates, handshake slots, the directory's bytes and fetch rates, and
    query fan-out. Every derived number below is that file's.
  - CI through `gh`: `consensus` run 39051586055 at `fc66c0e`, 8 of 8 jobs green
    (linux-asan-ubsan, linux-tsan, linux-x86_64-gcc and -clang, linux-arm64-gcc, macos-x86_64,
    macos-arm64, freebsd-x86_64). The design branch is docs only, which CI skips (`5691ec7`).
  - `HEAD` against `origin` for every repository read (calibration row 5): only `consensus`
    holds unpushed commits, named above.
  - Not run: the last line of this review.

Citations: `PT:N` is `docs/design/peer-transport.md` at `8a1f840`; `TC:N` is
`docs/design/track-c.md` at `5691ec7`; `SIM:N` is `simulator.md` at `a6c1f4e`; `PKI:N` is
`pki.md` at `bfc11c50c`; `TS:N` is `transport-security.md` at `7e8cccb7d`. Bare `consensus`
paths are at `fc66c0e`, `reactor_tls.h` at `e19afd9`, and Detent's files at `deae42d`.
`PLAN.md` lines are the branch's (`5691ec7`); `main` has since added four lines at :44.

## Verdict

**APPROVE WITH CONDITIONS: the shape holds, and the additive steps 1 to 3, 5, and 6 proceed;
version 2's frozen parts (step 4) and the directory's dissemination wait for PT-R1 to PT-R9 and
a revision check.**

What holds, and reproduces:

- **Identity.** SPKI pins beside certificate pins, after the chain verifies, keep Detent's
  configuration working and make same-key renewals free (PT:146-175). The digest is the one the
  PKI helper prints and SR12 requires: SHA-256 of `i2d_X509_PUBKEY(X509_get_X509_PUBKEY(leaf))`
  (PKI:598-606), not `X509_pubkey_digest`. Dropping the constant-time scan is right: pins hash
  public data, and a scan at 10^5 nodes reads 3.2 MB per handshake (`arith.out`).
- **Admission.** Binding each frame's source incarnation to the session's node (D5) closes the
  hole the starting point names, and the hole is wider than it says (PT-R10): a forged append
  sets the follower's commit index (`core.h:884-888`). Against honest memberships, a forged
  sender cannot cast another node's vote. A named incarnation from the wrong node is refused
  (PT:264-269); an unknown one, even vouched, is never counted, since a candidate counts
  pre-votes and votes only from voters of its membership in force (`core.h:814, 818-819`), and
  every membership in force is one the replica holds. PT-R10 and PT-R11 name where that rests
  on assumptions the code does not enforce.
- **Versions.** A per-session range with the acceptor's choice checked by the dialer (D6), a
  profile without the wire version (Track C's D2 put it in, which would make N and N − 1 refuse
  each other: PT:318-323), a two-version codec, and N + 1 only after every node runs N (D7)
  match the simulator's upgrade model (SIM:109, 122, 739) and SYS-3.
- **The header's arithmetic.** The 40-byte header turns `control_frame_bound` from
  `76 + 32·entries + bytes` (`codec.h:10-15`) into `104 + 32·entries + bytes`; the largest Raft
  control frame is 112 bytes; the hello's fixed part is 208 bytes (`arith.out`).
- **Control first.** Fragments close the gap Track C's D16 left: an entry above 256 KiB went
  alone (`types.h:119-127`) and held control frames 0.54 s at 1 Gb/s for a 64 MiB command. With
  fragments, a control frame waits for 320 KiB, 2.6 ms at 1 Gb/s (`arith.out`).
- **One policy.** Every decision in a sans-I/O class that `Peers` and the simulator both drive
  (D12), with T5's replay of a real run through the class alone, answers the simulator's Q3 and
  SIM-R6 in full (`simulator-design.md:485-487`).
- **The starting point** is accurate (PT:89-107): `tls.h:12-66`, `hello.h:11-54`,
  `profile.h:7-37`, `peers.h:14-38, 90-94, 125-206, 329-331, 343-346, 368-394, 466`,
  `session.h:62-72, 126-171`, `codec.h:10-15, 22, 121-122, 163-165`, `core.h:733-736, 843-846`,
  `worker.h:667-676`, `membership.h:128, 132`, `types.h:214-217`, and Detent's pin at `b7073bb`.

What blocks the frozen parts:

- PT-R1: the windows the wire version fixes are grants the receiver cannot withhold, and they
  exceed its own budget.
- PT-R2: `Window` cannot name the connection, and its 10 ms timer sends 5 to 52 times the
  heartbeat frames.
- PT-R3: a frame can exceed every grant the receiver may make, which wedges its group.
- PT-R4, PT-R5, PT-R6: the hello's layout, the tag space, and the directory's schema have gaps
  that freeze.
- PT-R7: each node trusts the one root member it fetches from, and the fetch load meets the
  handshake cap.
- PT-R8, PT-R9: one "additive" change duplicates an existing option, one changes Detent's
  behavior, and the relocation has no behavior test in `consensus`.

## What freezes

| Element | Where | State | Findings |
|---|---|---|---|
| The hello, `DTHL0002` | D8, PT:338-371 | Changes required: length, prefix, endpoints, refusal | PT-R4 |
| The frame header (kind 8, group 16, source 8, destination 8) | D9, PT:375-387 | Sound | PT-R5 for host frames |
| The tag space (1 to 17; 256 to 511; 512 and up) | D9, PT:389-403 | Changes required | PT-R2, PT-R5 |
| Windows "fixed by the wire version" | D11, PT:440-441 | Wrong as written | PT-R1, PT-R3 |
| The pin form (a kind and a SHA-256 of the DER or the SPKI DER) | D2, PT:148-152 | Sound; the kind's byte values are owed | PT-R6 |
| The directory's schema, in `consensus` | D3, PT:179-203 | Changes required | PT-R6 |
| Where it lives, its delta, and its fetch | D4, PT:210-257 | Home sound; dissemination wrong | PT-R7 |
| Changes called additive in `consensus` | PT:618-633 | One redundant, one not additive, one unproven | PT-R8, PT-R9 |
| No change in `reactor` | PT:552-553, 630 | Verified: `strict` exists (`reactor_tls.h:45, 70-75`), the Peer role (`:83-85`), and peers call no `bind_hostname` | None |

## Rulings on D1 to D15

| Decision | Ruling | Changes |
|---|---|---|
| D1 One transport for every node-to-node message | Sound with changes | Host frames' binding (PT-R5); query fan-out off the contact cache (PT-R12); TS-R3 and TS-R4 answered only under Q3's option 1 (PT-R18) |
| D2 Identity: SPKI pins beside certificate pins | Sound | The kind's byte values (PT-R6); a joiner's self-check runs after its first fetch (PT-R18) |
| D3 The node directory | Sound with changes | Authentication fields only in `consensus`; where addresses come from; the delta's format; enrollment's entries; an atomic pin replacement for an exposed key (PT-R6, PT-R15) |
| D4 Where the directory lives | Home sound; dissemination wrong | Quorum-signed versions that any node relays, in place of trusting the serving member (PT-R7) |
| D5 Admission | Sound with changes | Followers validate every membership change; the same-node window; the witness's answer, integrity, and expiry (PT-R10, PT-R11) |
| D6 Wire versions | Sound with changes | The hello's magic fixed for every later version; the development `DTC2` conditional on Detent (PT-R4, PT-R14) |
| D7 What one version back promises | Sound with changes | "Every node runs N" with nodes down; the delta under N − 1; feature gating tested (PT-R14) |
| D8 The hello | Sound with changes | PT-R4 |
| D9 Frames and tags | Sound with changes | PT-R2, PT-R5 |
| D10 Connections | Sound with changes | Handshakes at query load; slots as a liveness lever (PT-R12, PT-R13) |
| D11 Flow control | Mechanism sound; wrong as written | Explicit per-group grants within the budget, no window implied by the version, no connection window; frames within grants; no per-group timer (PT-R1 to PT-R3) |
| D12 The policy | Sound with changes | The relocation's evidence; its threads; reuse (PT-R9, PT-R12, PT-R16) |
| D13 The keepalive detector | Sound | One pending `Pong` per connection (PT-R17, T4) |
| D14 TLS settings, reload, lifetime | Sound with changes | No new `PeerOptions::strict`; the close at notAfter opt-in for Detent (PT-R8) |
| D15 Enrollment | Sound as a deferred shape | An owner, a trigger, and the directory's side now (PT-R19) |

## Findings

### What freezes

1. **PT-R1. The windows the wire version fixes are grants the receiver cannot withhold, and they
   exceed its budget** (D11, PT:440-448; the cost table, PT:591).
   - D11 fixes 1 MiB per connection and 64 KiB per group on the control lane "by the wire
     version" (PT:440-441), and says the receiver "never grants past the budget" of 256 MiB
     (PT:446-448). It does not say when connection credit returns. Either reading breaks the
     budget:
     - **Returned on admission.** A group waiting on its barrier then holds connection credit,
       which R12 forbids (`track-c-design.md:682-686`) and PT:468-470 says cannot happen. The
       connection windows alone grant 256 MiB over 128 peer and 128 contact sessions, the whole
       budget, before any window grows; 272 MiB at the connection cap (`arith.out`).
     - **Returned on receipt.** Memory is then bounded by the group windows. Their implicit
       64 KiB over 2,000 connection and group pairs is 125 MiB, plus 16 MiB for node-level data
       if group zero takes a group's 64 KiB on each session (PT:438, 440-441): 141 MiB before
       growth, leaving 115 MiB, room for 28 groups at D15's 4 MiB. At five members per group
       the implicit grants alone are 250 MiB; past 4,096 pairs they exceed the budget
       (`arith.out`). DISTRIBUTION's contract is "at least" 1,000 groups per node
       (`DISTRIBUTION.md:16`).
   - The cost table's "Initial grants: P × the connection window = 64 MiB" (PT:591) counts one
     lane of peer sessions and no contacts or group windows.
   - Why it matters: a malicious authenticated peer may send up to every implicit window at
     once, so the threat model's "Credit for data" (PT:123) holds only if implicit grants fit
     the budget, and a window fixed by the wire version cannot shrink without version 3.
   - **Fix.** The wire fixes no window. After the hello, each receiver sends one coalesced grant
     naming every group it shares with the peer (it knows them, D5) and node-level data, sized
     within its budget. Credit is per group only; a receiver keeps the sum of its outstanding
     grants within the budget and grows windows only from what is left. The connection window
     goes: once the reader always drains the socket into credited queues, it only repeats TCP's
     own window. That is one credit mechanism instead of two, and the initial sizes become
     presets measured on `main` rather than frozen bytes. D11, the cost table, and T4 follow.

2. **PT-R2. `Window` cannot name the connection, and its timer outnumbers heartbeats** (D9's
   kind 15, PT:396; D11, PT:438, 442-443).
   - A node-level `Window` (group zero) names two windows: the connection's and node-level
     data's, which "spends a window of its own, as group zero" (PT:438). The body (increment,
     wanted) has no field that tells them apart.
   - "One `Window` per group when half its window is consumed or 10 ms have passed" (PT:443)
     turns every sparse append into its own frame. A node follows 666 of its 1,000 groups at
     `rf = 3`; at 10 appends a second each that is 6,660 `Window` frames a second, and 66,600 at
     100, against Track C's bound of 1,280 coalesced heartbeat frames and 256 keepalives
     (`arith.out`; TC:651-667). The half-window rule alone sends 203 a second at the
     simulator's median of 10 KB/s per shard (SIM:82). The cost table has no row for them.
   - **Fix.** With PT-R1's per-group credit, a `Window` always names a group, zero for
     node-level data, and the ambiguity goes. Drop the 10 ms clause and return credit at half
     the window. Carry increments for several groups in one node-level frame, as coalesced
     heartbeats do, so the post-hello grant of PT-R1 is one frame. The cost table states
     `Window` frames per node per second under load.

3. **PT-R3. A frame can exceed every grant, and a blocked request can starve** (D11,
   PT:444-452; the limits table, PT:610).
   - A fragmented frame needs its whole credit before its first fragment (PT:462). One 64 MiB
     command, the decoder ceiling (TC:163-166), is a 67,109,000-byte append frame: 136 bytes
     above the 64 MiB the connection window may reach (PT:446). At the ceilings a node may set
     (`rpc_bytes` 256 MiB, `rpc_entries` 65,536) one append frame is 258 MiB against a 256 MiB
     budget (`arith.out`). Nothing ties node limits to the window maxima or the budget, so a
     group that holds such an entry never sends it and never commits past it, the wedge class
     of Track C's M4 and M8.
   - "The receiver grants at least that once the group's queue is empty, within ... the budget"
     (PT:450-452) states no order. While growth grants to small groups keep the budget short of
     a large request, the request waits without bound.
   - **Fix.** A node refuses at startup any node limits whose `control_frame_bound` exceeds the
     largest grant it can make (a stated share of its budget); the leader already holds back
     entries above a peer's limits (`core.h:339-349`), which the hello now reports (D8). A
     blocked request reserves budget as grants return, ahead of growth. T4 checks both.

4. **PT-R4. The hello's layout** (D8, PT:338-371).
   - **Length.** The fixed part is 208 bytes and the extension list starts with a 2-byte count
     (PT:360), so the shortest hello is 210 bytes; PT:346 and PT:604 say 208. Fix the minimum at
     210, or make the count absent at 208.
   - **What stays.** PT:362-363 promises the first 16 bytes to every later version, yet the
     acceptor's chosen version sits at offset 16, and "later versions add fields as extensions
     only" freezes all 208. Say so. The magic must stay `DTHL0002` in every later version: a
     version-3 hello under `DTHL0003` would be refused by version 2 before its range is read
     (PT:345).
   - **Endpoints.** Tag 1's "family" (PT:360) needs wire values: `AF_INET6` is 10 on Linux, 30
     on macOS, and 28 on FreeBSD. Fix 4 and 6, and refuse duplicate or unordered extension
     tags.
   - **Checks.** Today a hello naming the local node is refused (`hello.h:50`); the check column
     omits it.
   - **Refusal.** For both ends to "log both ranges" (PT:305-307), the acceptor sends its hello
     with its range and a chosen version of 0 before closing. "Each side refuses the other's
     hello by its magic and logs why" (PT:312) holds for version 2 only: `b7073bb` counts a
     rejection and logs nothing (`peers.h:342-345` at `b7073bb`).

5. **PT-R5. The frame header and the tag space** (D9, PT:375-410).
   - **Reserved kinds.** Kinds 18 to 255 are neither assigned nor reserved. Since a kind added
     later is version 3 (PT:402-403), version 2 refuses them, and the table says so.
   - **Host frames and D5.** Host kinds "Group or node" (PT:399-400) meet D5's rule 2, which
     drops a source no membership binds (PT:264-267). A coordinator's shard request has no
     incarnation, so a group-level host frame is always unbound. Either host frames are
     node-level, with the group in the body, or D5 excludes host kinds and the host authorizes
     them.
   - **Fragments.** Binding, hosting, and credit must be checked on the first fragment, before
     reassembly allocates; at most one fragmented frame is in progress per group and connection;
     the inner kind must be a data kind, and its total is bounded by that kind's bound. Host data
     has no reassembled bound (PT:610).
   - **`Transfer`.** A `Credit` kind received at version 2 (`snapshot_transfer.h:29`) is a
     protocol error, not only "never sent" (PT:397).
   - **C2's bodies.** Kinds 9 to 12 are Track C's D12 in prose (TC:629-645). Version 2 cannot
     freeze until C2 gives their bytes, as the ledger notes (`status/peer-transport.md:36-37`).
   - **What closes a shared session.** A forged source (D5), an unsupported message (D6), a
     credit violation (D11), and today any decode failure (`peers.h:414-417`) close the session,
     and on a multiplexed connection each close resets every group the node pair shares. Track
     C's condition 22 noted that cost (`track-c-design.md:2136-2149`). List what closes and what
     only drops: unhosted, unbound, busy, and a single group's limit refusal drop the frame; an
     oversize frame from a sender that learned the receiver's limits from the hello is a
     violation.

6. **PT-R6. The pin form and the directory's schema in `consensus`** (D2, D3; PT:179-208).
   - **Pins.** 33 bytes each (PT:183), but the kind's byte values are unstated. Fix them (for
     example 1 certificate, 2 public key, 0 refused).
   - **Placement data in an authentication table.** The entry carries `Joining`, `Active`,
     `Draining`, and a 4-byte opaque failure domain (PT:182-184), which "placement reads"; the
     transport needs only the id, the pins, and removed or not. In a versioned table every
     `Drain` and `Activate` is a cluster-wide wave of fetches: two waves per lost node (its
     `Remove` with the new node's `Join`, then the `Activate`) and up to four per drained one,
     instead of one; 2.2 million fetches a day at nominal churn instead of 1.1 million
     (`arith.out`). A 4-byte domain also freezes in a library Detent links, while
     DISTRIBUTION's domains are a hierarchy (node, rack, zone, region; `DISTRIBUTION.md:21`).
     Keep placement fields in strata's root table; `consensus`'s `Directory` holds
     authentication only. It also removes the false "one batched entry" per replaced node
     (PT:29, 215): `Join` precedes the node's start and `Activate` follows it (PT:231-234).
   - **Addresses.** D3 says addresses are not entries (PT:194-195); D10 opens peer connections
     "through the directory" (PT:417-418); the static preset builds the directory from
     `PeerOptions::addresses` (PT:201). State where a dialer gets a member's endpoint in each
     phase (configuration until Track D2, then the member view, whose 32 bytes per member must
     hold it, SIM:134) and count it in the cost table.
   - **The delta.** T8 fuzzes "the directory delta" (PT:672), but no decision gives its bytes,
     the fetch request and reply, how far back the root keeps deltas, or the full fetch for a
     node older than the oldest delta. Name the repository that owns the format.
   - **Enrollment's side.** Q5 says this design fixes reserved `Joining` entries and `Enroll`'s
     atomic spending of a token (PT:736-737); neither D3's fields nor D4's commands
     (PT:222-229) hold a token. Add them, or state that the enrollment design adds them as a
     versioned change to the root's state.
   - **Tombstones.** "Never reused" by keeping tombstones (PT:181) costs 4,000 entries a year at
     4% node loss, 0.96 MB after ten years (`arith.out`). A counter the root assigns at `Join`
     gives the same invariant with nothing kept. Small, and simpler.

7. **PT-R7. Each node trusts the one root member it fetches from** (D4, PT:243-257; Q1).
   - **Integrity.** A node takes the delta "from a root member, voter or learner, over its own
     authenticated session" (PT:246-247). That member is trusted alone: a compromised follower
     or learner can serve a delta that adds its own key to any node's entry, and every node
     that fetched from it then authenticates the attacker as that node, with no root commit.
     The quorum decides identities; one member serves them. The threat model has no row for it
     (PT:115-126).
   - **Load.** One change makes 100,000 fetches over about 45 s, 2,222 a second (PT:595). A
     fetch from a node with no session to the member costs a handshake. Through 16 slots
     (PT:429), with the root spread over regions (SIM:79) and two round trips per handshake, a
     member admits about 133 handshakes a second from another region at the p50 round trip and
     33 at p99; five members then take about 150 s, against T10's 60 s (PT:674; `arith.out`).
     Fetching from a member in one's own region would avoid most of it; D4 does not say how a
     fetcher chooses.
   - **Discovery.** Existing nodes know the root's members from their seed (PT:232-233), which
     admins update at each root membership change (PT:281-282). Nothing tells them the current
     members once the seed is stale.
   - **Fix.** Make D4's fallback (PT:255-257) the design. Root voters sign each version's digest
     after applying it, a quorum per version; any node relays signed versions as gossip hints
     do; a fetcher checks the quorum against the voters the previous version names, chained
     from the seed. Integrity then equals the root's commit, the load spreads over every node,
     and the current voters travel in the state. The cost is about three signatures per
     version, 11 to 33 versions a day, and about three signature checks per fetch. It lives in
     strata's root host, not in `consensus`. It is the mechanism Q2's option 2 needs too
     (signatures by a quorum of the previous membership), so built once it serves both.

8. **PT-R8. Changes called additive, against the code Detent links** (PT:618-633).
   - **`PeerOptions::strict` duplicates an option that exists** (D14, PT:539-542; PT:626).
     `PeerOptions::tls` is a `reactor::tls::Options` (`peers.h:28`), which has `strict`
     (`reactor_tls.h:45` at `e19afd9`, the commit `consensus` pins, `CMakeLists.txt:23`), and
     `Peers` passes it to `configure` (`peers.h:65`). A host sets `tls.strict` today. A second
     flag is one mechanism twice; drop it.
   - **The close at notAfter is not additive** (PT:549-550). Detent's sessions outlive their
     leaf until they reconnect. A voter whose leaf expires unnoticed would lose every session
     at once and reopen none. Make it opt-in in the `Peers` preset, or list it with version 2's
     changes for Detent's agreement.
   - **`PeerConfiguration` is built by position.** Detent brace-initializes it
     (`src/detent_authority/peers.hh:11-17`), so version 2 inserts and reorders no member; the
     group fold (PT:368-371) happens inside `Peers`. `PeerOptions` is assigned by name
     (`service.cc:305-340`; `test_transport_peers.cc:99-107`), so an appended field is safe.
   - **The static preset.** "From `PeerOptions::addresses` and `pins`" (PT:201) contradicts
     PT:194. The preset builds the `Directory` from the pins and an address book from the
     addresses.
   - **Verified additive:** `public_key_fingerprint`, `PinKind`, `Pin`, `Directory`, and
     `authenticated_node` are new names beside unchanged `tls.h:12-66`; `Core::known_members()`
     is a const query beside `membership()` and `committed_membership()` (`core.h:185-186`),
     cacheable on `membership_version_` (`core.h:307`), which fixed groups reach only with their
     founding voters (`membership.h:69-75`); reload and re-verification are unused until called.
     Either-node dialing, the admission pass, and `Credit`'s retirement belong to version 2, as
     stated.

9. **PT-R9. The relocation has no behavior test in `consensus`** (first step 2, PT:751-753;
   T5, T9).
   - `consensus`'s only `Peers` test instantiates it over a host whose members abort
     (`test/transport/peers_test.cc:9-16`): it compiles and runs nothing. The behavior step 2
     must keep (queue bounds and drops, control first, the owner pass, the backoff, append
     observations) runs only in Detent's `test_transport_peers` and its process tests
     (`LIBRARY_BOUNDARIES.md:251`).
   - "The API unchanged" leaves out the observables Detent can read: `PeerStats`,
     `PeerOccupancy`, and `AppendTimings` (`peers.h:39-49`), which Detent plans to extend
     (`LIBRARY_ADOPTION_ARCHITECTURE.md:92`).
   - T9 says "At Detent's current pin nothing changes" (PT:673), which is true of any commit;
     Track C's condition 1 requires Detent's build for every library commit
     (`track-c-design.md:1235-1237`).
   - **Fix.** Before step 2, a characterization test in `consensus` (three `Peers` over
     loopback, recording frames and stats) written at the pre-relocation commit and rerun after
     it. T9 runs Detent's transport suite at each additive step's commit, with the pin moved in
     a scratch build, and the record goes to `LIBRARY_BOUNDARIES.md`.

### The design

10. **PT-R10. D5's premise is enforced only where a leader proposes** (D5, PT:259-291).
    - "The binding is well defined because an incarnation is never reused and never moves
      (`membership.h:128, 132`)" (PT:275-276). Those checks are in `validate_change`, which
      only the proposing leader runs (`core.h:613`). Followers decode `Configuration` entries
      structurally (`note_append`, `core.h:302-308`; `decode_membership` checks order, counts,
      and roles, `membership.h:161-194`), and so does recovery. A compromised member that leads
      can append an entry that places other members' incarnations on itself; every follower
      that holds it then binds them to the attacker (rule 2, PT:264-266), and the attacker wins
      every later election on its own grants (two of three). The residual "its own
      incarnations" (PT:122) then fails, and the attacker cannot be voted out.
    - **Fix.** Followers and recovery check each `Configuration` entry against the membership
      before it (`membership_at(index − 1)` and the limits at its position) and refuse a moved
      or reused incarnation. The binding refuses an incarnation that two held memberships place
      on different nodes. The leader's automatic removal of a `leaving` member
      (`core.h:1021-1025`) passes the same check, so honest groups see no change, and fixed
      groups have no `Configuration` entries.
    - **The same-node window.** During Track C's same-node replacement (TC:324-327), a
      membership names both the leaving and the new voting incarnation on one node, so "a
      node's vote counts once" (PT:289-290) holds only for an honest node. Admit from a node
      only its non-leaving incarnation when two voting ones name it, or state the residual.
    - **The hole is wider than the starting point says** (PT:109-113). A forged append at a
      term at or above the follower's, past log matching, raises the follower's commit index to
      what it just appended (`core.h:884-888`), so the follower applies entries no leader
      committed; it also sets a witness's truncation bound (`core.h:887`). A conflicting one
      fences (`core.h:835-846`). The stale-witness residual (PT:643-645) is therefore one of
      safety, not availability.

11. **PT-R11. The witness: its answer, its integrity, and its expiry** (D5, PT:279-296; Q2).
    - **The answer.** The host's witness vouches (PT:266-267) and the policy emits `Vouch{group,
      node}` (PT:492), but no decision says where the answer comes from, how long it takes, or
      what it costs. Every member added after founding is a joiner with no membership
      (TC:310-312), so each new replica's first frames wait on a vouch: about 300 per lost
      node, and about 1.5 million over a zone's re-replication (SIM:80, 85, 131).
    - **Integrity.** If any placement member answers, a compromised one vouches for any node
      across its range, about 22,700 groups (SIM:86), not "only the groups placed on it"
      (PT:692-693). That matches the trust placement already holds (its leader decides
      membership for its range) only when a vouch has a placement decision's integrity: the
      host's own committed assignment, which placed the replica, or a read through the placement
      leader. Say which, and restate the residual.
    - **Expiry.** A vouch is keyed by group and node (PT:492-493) and gives way only to a
      membership that later names the incarnation (PT:284-285). A node moved off the group stays
      vouched at that replica when no membership ever names it again. It expires when the record
      changes.
    - **Kinds.** Appends and installs from a vouched incarnation must pass, since catch-up needs
      them. Coalesced records, `LeaderGone`, and `TimeoutNow` act only from the replica's known
      leader (`TimeoutNow` already does, `core.h:809`), so a stale vouch cannot raise commit
      indexes without log matching (TC:629-634) or start campaigns. C2's D12 carries the rule.

12. **PT-R12. Contacts at query load, and the policy's threads** (D10, PT:412-431; D12; the cost
    table).
    - D10 puts query fan-out on contact connections in a 128-entry cache (PT:419-421), which a
      random node hits 0.128% of the time at 10^5 nodes. A query to a 16,384-shard index at
      fan-in 64 (SIM:133) touches 4 + 256 + 16,384 = 16,644 node pairs (`arith.out`). With
      aggregators and replicas drawn at random, nearly every hop is a new handshake, and
      `strict` (D14) turns off resumption, so each is a full one. The cost table counts gossip's
      2 to 4 handshakes a second only (PT:593). Detent's own record: a 10,000-agent run failed on
      a client that opened a connection per request (`PERSISTENT_TRANSPORT_ARCHITECTURE.md:6`).
    - The acceptor's side is unstated: a shard node that reaches 128 contacts from coordinators
      either refuses the next or evicts the least recent.
    - One policy per node (PT:482) with deliveries posted to each group's executor
      (PT:501-504) costs a cross-core hop per frame each way for strata's groups, which live on
      their cores (TC:460-462). The executor that runs the policy and the sessions' TLS is
      unstated.
    - **Fix.** D5's fan-out uses peer connections or a stable aggregator topology, and the cost
      table states handshakes per second per node at a stated query rate (the simulator's T7
      states a count of queries, not a rate, SIM:731); or contacts are sized per role with that
      rate measured in T12. State the policy's executor; T12 measures CPU per frame and per
      byte.

13. **PT-R13. Handshake slots are a liveness lever under Q3's option 1** (threat model, PT:119;
    D10, PT:429; Q3).
    - 16 slots at 5 s let 3.2 stalled connections a second hold every slot (PT:119). Under
      option 1 every probe to a node outside the cache needs a new session (PT:708-712). A
      keyless attacker who holds a node's slots makes every probe to it fail; SWIM suspects it
      within 5 s and Lifeguard's timeout declares it dead about 20 s later (SIM:123-124). About
      80 connections over 25 s per victim (`arith.out`), and placement re-replicates the node's
      replicas after its hold. Each ClientHello that reaches a slot also costs the node a
      signature. The threat model lists traffic analysis only.
    - Mitigations to weigh, with the security reviewer: a per-source handshake cap; a short
      deadline for the first flight (at 1 s the attacker needs 16 connections a second); and
      probes over persistent connections, a fixed monitoring topology (Rapid's observers,
      HyParView's active view), so a node's monitors hold sessions opened before the attack.

14. **PT-R14. One version back, against Track D8 and the simulator** (D6, D7; PT:301-336).
    - **"Every node runs N"** (PT:333) is undefined with nodes down: about 35 are down at once at
      nominal churn (SIM:99), and one down across a whole upgrade returns at N − 1 among peers at
      N + 1. Define it (every directory entry not removed has reported N in a hello or `Ping`),
      and refuse a straggler with a reason that says to upgrade, so it does not read as a network
      fault.
    - **The delta and the activation record** cross versions too; state their N − 1 rule.
    - **Feature gating** (PT:331) is a host contract that each version-N feature implements. T3
      tests that messages drop (PT:667), not that a group turns a feature off while a member's
      session runs N − 1. Add it.
    - **"Never a supported version"** (PT:312-314) is a request to Detent, not a fact. Detent's
      ledger names candidate `d7192f7` (`DEPENDENCY_UPGRADES.md:41`), which lies in
      `0307308..fc66c0e` and writes `DTC2` frames without the header under `DTHL0001`, and its
      blueprint adopts "latest consensus" with the peer contract frozen
      (`LIBRARY_ADOPTION_ARCHITECTURE.md:86`). If Detent ships it, leaving it takes a second
      restart. Q4's answer carries the condition.
    - **The simulator.** Its connection has four lanes and one reset drops them all
      (SIM:233-242); the transport's two lanes are separate TCP connections that reset apart, plus
      contacts. And tier A cannot hold a 7.2 MB directory per simulated node: 720 GB at 10^5. The
      directory is a versioned log, so one shared table and a version per node (8 bytes) are
      exact, as the member view's shared table is (SIM:443-457). PT:81's row says so.

15. **PT-R15. An exposed key costs a node's data** (D3, PT:192-193; D4, PT:239-240).
    - A node whose key is "lost or exposed" is removed and rejoins under a new id, so every
      replica on it is a new incarnation and copies again: about 891 GB per node (SIM:85).
    - An exposed key on a machine still trusted needs only an atomic pin replacement, `AddPin`
      and `RemovePin` in one root entry with no overlap. Peers that have not fetched refuse the
      node for one dissemination window, about 45 s, and nothing re-replicates. Removal stays
      the rule for a compromised machine, whose state cannot be trusted.

16. **PT-R16. Reuse** (calibration row 1).
    - `Kronuz/lru-cache` exists. Its aging path reads `steady_clock` (`lru.h:209, 227, 242, 253,
      673`), so it cannot run inside the sans-I/O policy as is. Reuse its plain `lru` for the
      contact cache's capacity and expire idle contacts from the policy's `tick`, or add a
      time-taking overload to it (additive). The design names neither.
    - `Flow::frame_bytes` (`c752657`; `types.h:119-127`) and `Fragment` (PT:459-462) both bound
      a control frame's wait, and D11 names `frame_bytes` as the fragment size (PT:460). With
      fragments, `frame_bytes` is not R19's mechanism: state it as a pipelining setting or retire
      it, and make the fragment size a transport preset.
    - The incoming budget is a `ByteBudget` today (`session.h:12-32`). Track C puts node pools on
      `ledger`'s capacity and permit core (TC:709-711, 720-721); the incoming budget moves with
      them.

17. **PT-R17. The gate rows** (PT:658-676).

    | Row | Target | Procedure | Change |
    |---|---|---|---|
    | T1 | Sound | Sound | PKI's T10 fixtures exist only after `Kronuz/pki`'s step 1 (PKI ledger, Q1), so the `openssl` pipeline is the target and PKI's fixtures a later check. Add: a leaf with only `serverAuth`, or only `clientAuth`, refused in the other direction (either node dials, so peers need PKI's `peer` role, PKI:406); a dialer reaching another node's pinned leaf; a session closed at its leaf's notAfter |
    | T2 | Sound | Sound | Add D4's bootstrap refusal without a seed (TS-R6); a delta with a version gap (full fetch); a delta that breaks an invariant at the applying node (refused, table kept) |
    | T3 | Sound | Sound | Add a chosen version outside the dialer's computation; the acceptor's hello on refusal; feature gating (PT-R14) |
    | T4 | Sound | Sound | Add: grants within the budget, initial ones included (PT-R1); each blocked request granted once its group's queue empties, ahead of growth (PT-R3); the reader never stops draining a connection; at most one pending `Pong` per connection |
    | T5 | Sound | Sound | The characterization test before step 2 (PT-R9) |
    | T6 | Sound | Unmeasurable as written | netem's default queue is 1,000 packets: 12 ms at 1,500-byte packets and 524 ms at loopback's 64 KiB packets, at 1 Gb/s, above the 10 ms target, and `TCP_NOTSENT_LOWAT` bounds only unsent bytes (`arith.out`). State netem's limit in bytes, or a real 1 Gb/s link, and state the bound as fragment, unsent bytes, and the queue below TCP. macOS and FreeBSD over loopback have no 1 Gb/s bottleneck: rate-limit them with dummynet (`dnctl` on macOS, `ipfw` on FreeBSD). State the unsent bound at 10 Gb/s, where 64 KiB drains in 52 µs |
    | T7 | Sound | Incomplete | T2's harness drives cores, not the policy (TC:810-830): the binding check is one function the policy and the harness both call, or the "binding off" mutant never reaches T2. Add a compromised leader's entry that moves another node's incarnation, refused (PT-R10); an unknown incarnation's `LeaderGone` and coalesced record ignored (PT-R11) |
    | T8 | Sound | Sound | Matches TESTING's targets (`TESTING.md:35-37`) and the C1 gate's ruling (`track-c-design.md:1967-1969`); add the delta's decoder once PT-R6 specifies it |
    | T9 | Weak | Incomplete | Detent's suite at every additive step's commit (PT-R9); the `b7073bb` end logs no reason (PT-R4) |
    | T10 | Sound | Sound | The fetch path's capacity (PT-R7); tier A's shared directory (PT-R14) |
    | T11 | Sound | Sound | The 30-node cluster is PLAN's Track D rule (`PLAN.md:479-480`) and does not exist yet; the row names its host when it does |
    | T12 | Incomplete | Sound | Add `Window` frames per second under load (PT-R2), handshakes per second at query load and CPU per frame and per byte (PT-R12), and directory bytes with tombstones and overlaps |
    | Missing | | | D10's bounds, as transport-security's T3 has (TS:143): 17 stalled handshakes (the 17th refused, the 16 dropped at 5 s), the connection cap, contact eviction, RSS per connection |

18. **PT-R18. The text changes asked of other documents** (PT:69-85), each checked.
    - **Track C.** D1 (TC:212-215): also that addresses leave the directory, not only the pin's
      form. D2 (TC:138-140): right; and TC:42-45 says "the transport's hello (`DTPROF02`)" carries
      limits, where the hello is `DTHL0002` and `DTPROF02` the profile. D5 (TC:305-309): right,
      and the site table's rows that restate D5's rule (TC:236, 244, 251) take the binding too.
      D12 (TC:629-650): right. The format table (TC:182-200): add `DTHL0002`; kind 16 replaces the
      "snapshot chunk frame (group, transfer, offset, final flag)" row (TC:199); `Ping` and `Pong`
      move from C2 to this design (TC:196). D16 and D18: right.
    - **DISTRIBUTION.** :44, :49, :71, and :129-135 as asked; :22 (cluster-state memory per node)
      also gains the directory's 72 bytes per member.
    - **transport-security.** D2 is answered from Phase 7b, when Xapiand adopts the transport;
      until then its node-to-node legs stay in clear (TS:32-34). D3 is answered only if Q3
      chooses option 1; under option 2, TS-R3 and TS-R4 return. D4 and D8 are answered. D5 only in
      part: TS-R5 by removal and TS-R6 by the seed, while TS-R7 waits for Q5's design and TS-R8 is
      the PKI helper's (PKI D8, D13). Answer 2 (TS:153-154): superseded, as asked.
    - **The simulator.** PT:81 with the shared directory, and SIM:233-242's lanes (PT-R14).
    - **The requirements table** (PT:52). SYS-12's recovery bound
      (`system-2026-10-17.md:521-525`) stays Track C's to carry; D9 applies the same rule to wire
      decoders only.
    - **Smaller points.** A joiner checks its own leaf against its entry after its first fetch,
      not at start (PT:163-164, 231-234). The incoming budget and D15's node send budget are two
      pools that share a number (PT:446-447).

19. **PT-R19. Enrollment is deferred a third time** (D15, Q5; PT:555-575, 733-742).
    - Transport-security deferred it (TS:128); the PKI design sent it here, and the owner decided
      so (PKI ledger, Q6; `pki-design.md:622-623`); Q5 now proposes a design of its own. The
      system review already counted two deferrals (`system-2026-10-17.md:224-225`).
    - Option 1 is right on the merits (an unauthenticated listener deserves its own security
      review), on three conditions: an owner; a trigger (before any deployment past the PKI
      helper's 1,000-node bound, PKI:241-254); and the directory's side in D3 and D4 now
      (PT-R6).

## The open questions

- **Q1, where the directory lives: option 1, the root's node table,** on two conditions: versions
  are signed by a quorum of root voters and relayed by any node (PT-R7), and `consensus`'s
  `Directory` holds authentication fields only (PT-R6). Option 2 adds an authority to seed and
  run; option 3 makes each node trust and reach many.
- **Q2, binding an unknown incarnation: option 1, the parent authority's record,** on three
  conditions: followers and recovery validate every membership change (PT-R10); a vouch comes from
  a committed placement record, the host's own assignment or a read through the placement leader,
  and expires when the record changes (PT-R11); and the residual is restated as the vouching
  authority's range, the trust placement already holds. Option 2 stays the escalation, and built
  with PT-R7's signatures it is one mechanism for both. Option 3 is ruled out, as the design says.
- **Q3, the gossip carrier: option 1, host frames on this transport,** which is also what PLAN's
  Track D2 already says ("SWIM-style gossip over Detent's peer transport", `PLAN.md:458-460`);
  options 2 and 3 would amend PLAN. Conditions: Track D2 prefers a fixed monitoring topology over
  persistent contacts, so probes need no handshake each, and handshake admission resists slot
  exhaustion (PT-R13); query fan-out does not ride the contact cache (PT-R12).
- **Q4, Detent leaving version 1: option 1, one coordinated restart into version 2,** on the
  condition that Detent's Group F adopts version 2 itself and not `d7192f7` or any commit in
  `0307308..fc66c0e` (PT-R14), with Detent's agreement recorded in `DEPENDENCY_UPGRADES.md` before
  step 4 lands, and T9 at every additive step (PT-R9). Option 2 is two wire paths, which Track C's
  D2 rules out (TC:135-137).
- **Q5, online enrollment: option 1, a design of its own in the security tier,** with an owner, a
  trigger, and the directory's side here (PT-R19).

## Which work starts when

| Step | Starts | Waits for |
|---|---|---|
| 1. Pins and `Directory` | Now | PT-R6's authentication-only shape and the pin kind values, before it lands |
| 2. `PeerPolicy` relocated | Now | PT-R9's characterization test first; Detent's transport suite before it lands |
| 3. The detector | Now | Its reports ignored by the `Peers` preset, so it stays additive |
| 4. Version 2's wire | The frame header and the decoders of kinds 13, 14, and 16 may be drafted on a branch | PT-R1 to PT-R5 (kinds 15 and 17 and the hello wait for them), PT-R7's fetch protocol, C2's bodies for kinds 9 to 12, and the revision check; it lands after the full design review and the security review |
| 5. The binding | Now | PT-R10 in the same series; PT-R11 before the strata side of the witness |
| 6. Reload | Now | The close at notAfter opt-in (PT-R8) |
| 7. The simulator's adapter | After step 2 | Q3 |

## Blocking items / Required changes

Before version 2's frozen parts are built (step 4) and before the revision check:

1. PT-R1: the wire fixes no window; explicit per-group grants within the budget; no connection
   window; the cost table's grants recomputed.
2. PT-R2: `Window` names one group (zero for node-level data); no 10 ms clause; coalesced
   increments; a cost row.
3. PT-R3: node limits checked against the largest grant; blocked requests ahead of growth.
4. PT-R4: the hello's minimum length, frozen fixed part and magic, endpoint encoding,
   self-connection check, and refusal exchange.
5. PT-R5: kinds 18 to 255 refused; host frames outside D5 or node-level; fragments checked before
   reassembly and bounded; `Credit` a protocol error; C2's bodies; the list of what closes a
   shared session.
6. PT-R6: pin kind values; authentication-only `Directory`; the address source; the delta's
   format and owner; enrollment's entries; the id counter.
7. PT-R7: quorum-signed versions relayed by any node, with root discovery in the signed state; a
   threat-model row for a compromised serving member.
8. PT-R8: no new `PeerOptions::strict`; the close at notAfter opt-in; `PeerConfiguration`
   untouched; the preset's address book.
9. PT-R9: the characterization test; T9 at every additive step.

Before the steps they name: PT-R10 with step 5; PT-R11 before the strata witness; PT-R12 and
PT-R13 before Track D2 and D5 build on contacts. In the revision: PT-R14 to PT-R19.

## Conditions

1. **C1. Done criteria.** The ledger has none (`status/peer-transport.md:11-12`). The lead writes
   them before step 1 starts. Proposed: K1, step 1 with T1 and T2's invariant cases; K2, step 2
   with PT-R9's characterization test, T4 over today's rules, and T5; K3, step 3 with its T4 cases;
   K4, step 5 with T7 and PT-R10's check and its mutant; K5, step 6 with its tests; K6, ASan,
   UBSan, and TSan over every transport test and coverage at TESTING's targets; K7, CI green on all
   eight jobs and Detent's suite at each step's commit; K8, step 4 only after the full design
   review and the security review clear it.
2. **C2. For the security reviewer,** through the lead: PT-R7's single serving member, PT-R10's
   rebinding, PT-R11's vouch integrity, and PT-R13's slots, each a threat-model row this review
   found missing.
3. **C3. Library commits** (Track C's condition 1, carried): each `consensus` commit of this
   design records Detent's build and its `LIBRARY_BOUNDARIES.md` entry.
4. **C4. T6's procedure** states netem's limit and dummynet on macOS and FreeBSD before its first
   run.
5. **C5. The simulator's tier A** shares one directory with a version per node.

## Verdict and required changes, in one line each

**APPROVE WITH CONDITIONS.** Steps 1 to 3, 5, and 6 proceed under C1 to C3; step 4's frozen parts
wait for PT-R1 to PT-R9 and a revision check.

1. PT-R1: implicit windows exceed the budget; explicit per-group grants, no connection window.
2. PT-R2: `Window` is ambiguous at group zero and its timer floods; one group per window, no
   timer.
3. PT-R3: frames can exceed every grant; limits checked, blocked requests reserved.
4. PT-R4: the hello's length, prefix, magic, endpoints, self-check, and refusal.
5. PT-R5: reserved kinds, host frames, fragments, `Credit`, C2's bodies, and session closes.
6. PT-R6: pin kinds, placement data out, addresses, the delta, enrollment, ids.
7. PT-R7: one serving member is trusted alone; quorum-signed relayed versions.
8. PT-R8: `strict` twice, the notAfter close, positional `PeerConfiguration`, the preset.
9. PT-R9: no behavior test for the relocation; T9 at every step.
10. PT-R10: followers do not validate membership changes; the same-node window; the wider hole.
11. PT-R11: the witness's source, integrity, expiry, and frame kinds.
12. PT-R12: fan-out handshakes and the policy's threads.
13. PT-R13: handshake slots let a keyless attacker kill a node in gossip.
14. PT-R14: stragglers, the delta across versions, feature gating, Detent's candidate, the
    simulator.
15. PT-R15: an atomic pin replacement for an exposed key.
16. PT-R16: `lru-cache`, `frame_bytes` against fragments, the budget on `ledger`.
17. PT-R17: the gate rows' additions, T6's procedure, and a bounds row.
18. PT-R18: the text owed to Track C, DISTRIBUTION, transport-security, and the simulator.
19. PT-R19: enrollment's owner, trigger, and directory side.

Not run: no build, test, sanitizer, fuzzing, or benchmark, and no compiled probe (`quiet check`
exits 1: the `strata-trials` strict hold, `night_gate.sh n1017`, until 01:17); only the arithmetic
probe ran, in Python. No simulator tier, no real network, no multi-node cluster, and no VM or
bare-metal host. Detent's suite not run; Detent read from its documents and three source files
only. The security reviewer's review of this design not read (it runs separately). No Xapian or
Xapiand GPL source opened.
