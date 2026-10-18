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

## Re-check

- Date: 2026-10-17, read from 17:10 to 17:25 PDT.
- Reviewed: `git log cb893e8..design/peer-transport`, six commits: `53e6f4b` (ledger),
  `f1f1d85` (the security review), `e4bfa3c` (this review, committed unchanged), `ef82b06` (the
  decisions on Q1 to Q5), `90d417e` (revision 1), and `2600377` (ledger, K1 to K9, Q6 decided).
  `git diff cb893e8 90d417e -- docs/design/peer-transport.md` read whole; the design is unchanged
  at `2600377`. Line numbers below without a file are the design's at `90d417e`.
- Checked against: PT-R1 to PT-R19 and C1 to C5 above; the security review's SR1, SR4, and SR6,
  where revision 1 departs from what they asked; the code at the commits the design's header
  names.
- Checked by:
  - Each closure read at its lines, and each new citation read at its commit: `core.h:70-100,
    1012-1020` (recovery's `note_append`, `held_back`, the removed leader's hand-off),
    `membership.h:37-56` (the ceilings; `command_bytes <= rpc_bytes` per node), `snapshot.h:51-53`
    (6,312-byte descriptors), `types.h:182, 238`, `reactor_tls.h:42-46`; reactor
    `test/tls_test.cc:146-156`, Detent `service.cc:264-272, 296-300`, and Xapiand `src/tls.cc:36-44`
    (Xapiand's own MIT file), which set `strict` only with the `Server` role or not at all;
    `git grep strict` over `consensus`, `cluster`, `http`, `reactor`, and Detent, which finds no
    consumer that sets it with the `Peer` role; `lru.h:296-342`; `simulator.md:182-184, 229-231` at
    `7d2f92b` (its design unchanged since `0054b30`); `PLAN.md:42-46, 189-190, 487-488` at
    `c5b571a`; `pki.md:337-347, 370-378, 417-421`.
  - The arithmetic probe extended (`~/.scratch/review-peer-transport/arith.py`, section
    "recheck" of `arith.out`). Revision 1's figures reproduce: initial grants of 63.0 MiB and
    71.3 MiB, R of 2,105,448 bytes, budgets of 128.02 MiB and 516 MiB, a 3.15 ms control wait,
    netem's 44 packets as 66,000 bytes, a 4.2 MB table hashed in 4.2 ms, a 0.48 s TLS 1.3 setup,
    8 sources and 8 connections a second to hold the slots, 0.333 handshakes per node per query a
    second, and 213 handshakes at a restart.
  - Not run: the last line of this section.

### Verdict

**APPROVE WITH CONDITIONS.** All nineteen findings close, and C3 to C5 close. Of the six places
revision 1 goes beyond the reviewers, one stands as written and five stand with one condition
each. Seven new items (PT-R20 to PT-R26) bind no step that may start now: two bind step 4's
flow-control code, one step 5's binding, three step 8's directory and admin code, and one is
small points. Step 4's frame header and kinds 13, 14, and 16 may be drafted now.

### The six departures, ruled

1. **One voice per node, in place of SR1's first clause** (D5's rule 3, `:374-384`). **Accepted,
   with PT-R22.** SR1's clause would refuse a node's second incarnation outright. That clause
   also refuses the honest case it did not consider: a replica that lagged past a same-node
   replacement could never admit the node's new incarnation when it leads. What SR1 feared, a
   learner speaking as an incarnation it invented, stays closed without the clause: below the
   committed counter it is forged (rule 2.3), and above it the witness answers only for the
   incarnation its record places (`Vouch{group, node, incarnation}`, `:368`). Rule 3 keeps the
   clause's intent, one vote per node per group, and closes this review's same-node window
   (PT-R10). In the window a compromised node now casts one vote, not two.
2. **The committed counter, with uncommitted bindings only admitting** (rules 2.1 to 2.4,
   `:353-373`). **Accepted as written.** SR1's second clause would read the counter from the
   newest held membership, committed or not. Then a deposed leader's uncommitted entry that took
   incarnation 5 for node A, at a replica that holds it, would raise that counter to 6. The
   replica would then forge the real incarnation 5 that the next leader gave node B, and close
   the session at every resend. With the committed counter, B's frames are unbound; the witness
   then places 5 on B, and the vouch outranks the stale entry (`:369-373`). Rule 2.3 never forges
   a current incarnation. Every incarnation below a replica's committed counter was added at or
   before that membership, so the replica's held memberships still name it unless it was removed
   before the base, which is when it is retired. The held memberships form one chain, since a
   replaced suffix drops its entries (`note_truncate`) and D18 checks each change. So an
   incarnation is named on one node across them, and rule 2.2 is well defined.
3. **Admission by the limits a quorum accepts, in place of SR4's every-member-ignored rule** (D8,
   `:508-517`). **Accepted, with PT-R26's T7 cases.** SR4 ignored every report, which lets a
   leader admit a command that no quorum can store: Track C's D2 wait (`track-c.md:153-155`),
   with nothing raised. The quorum rule admits per field the q-th largest of the voters' limits,
   within the group's and the leader's own. So no minority and no learner lowers it, and a
   quorum can always store what is admitted. The command field alone decides storage, since each
   node's `command_bytes` is at most its `rpc_bytes` (`membership.h:49`). A node that holds an
   entry can also receive it in one RPC, and the RPC fields only size batches. Say so in D8.
4. **`released` in `Window` records** (`:562, 698-700, 715-717`). **Accepted, with PT-R21.** A
   receiver cannot take back credit it granted, since data may already be in flight against it.
   So the former holder returns it, and a release of more than is held is a violation. TCP's
   order makes the accounting exact. The record now carries both directions: `increment` grants
   the other end, while `wanted` and `released` concern the sender's own credit. That is
   coherent, and T4's oracle counts releases (`:1184`).
5. **Kind 17, `TransferControl`, split from kind 16, `Chunk`, and kind 18, `Fragment`**
   (`:563-565, 576`). **Accepted, with PT-R20.** Header-first admission (`:539-545`) needs each
   kind's class from the header alone. A transfer kind that carried both chunks and `Begin`
   would need its body read first. The sizes are right: 65,728 bytes for a chunk (65,688 plus the
   header) and 6,480 for transfer control (40 plus 128 plus a 6,312-byte descriptor,
   `snapshot.h:51-53`).
6. **The root without a witness, and its stated gap** (D16, `:905-917`). **Accepted, with
   PT-R24.** A witness keeps the log but no application state, so it cannot compute S(v). A
   quorum holding one could commit versions that no majority of signers could certify. With no
   witness, the signers are the quorum, and certifying needs nothing a commit does not. The gap
   is real and honestly bounded. Losing three of five root voters inside one replacement window
   leaves the new membership a quorum and the old epoch no majority, so new versions wait and
   the operator re-seeds. Commits cannot carry the next epoch's signatures early: a signature
   made at append would certify memberships that a later leader replaces. PT-R24 narrows the
   window and closes three points the epochs leave open. Track D3's design must carry the rule
   (`:139`).

### Status

1. **PT-R1. Closed.** No connection window and no window implied by the version (`:659-664`);
   an initial pool of at most B/4, a reserve, and a growth pool (`:665-673`); credit only to bound
   members, larger windows only to the known leader or transfer source, within a per-peer share
   (`:678-683`); initial grants after the hellos (`:684-689`); the cost row (`:1039`); T4
   (`:1184`).
2. **PT-R2. Closed.** `Window` is node-level, and each record names one group, zero for
   node-level data (`:562`); returns at half a window, coalesced per connection, with no timer
   (`:690-693`); the cost row, 204 a second (`:1038`).
3. **PT-R3. Closed.** The reserve and the refusal at start (`:668-672`); requests up to the
   announced bound, then a FIFO reserve with a stall close (`:701-714`).
4. **PT-R4. Closed.** The 210-byte fixed part, frozen whole (`:483-499, 506-507`); the magic in
   every later version (`:441-443`); families 4 and 6 (`:501-505`); increasing tags (`:499`); the
   self-check (`:490`); the refusal hello (`:433-436`); `b7073bb`'s silence (`:443-446`).
5. **PT-R5. Closed.** Kinds 19 to 255 refused (`:566`); host frames node-level (`:567-568`,
   `:393-395`); fragment rules (`:577-586`); `Credit` a protocol error (`:575`); C2's bodies
   before the freeze (`:599-601`); what closes and what drops (`:588-598`).
6. **PT-R6. Closed.** Pin kinds 1 and 2 (`:223-226`); authentication only, ids from a counter,
   no tombstones, the address book (`:264-292`); placement state and burned digests at the root
   (`:296-320`); enrollment's side (`:887-893`); the formats (`:957-966`).
7. **PT-R7. Closed** (`:903-966`; threat row `:186`; T2 `:1182`; T10 `:1190`), with the
   gap ruled above and PT-R23 and PT-R24.
8. **PT-R8. Closed.** No second `strict` (`:836-838`); `close_at_expiry` opt-in (`:860-863`);
   `PeerConfiguration` untouched (`:795-802`); the preset's address book (`:281-283`). The two
   reactor options are additive as claimed: no consumer combines `strict` with `Peer` (search
   above). Detent builds `Options` by position (`service.cc:299`), so the PEM field must be
   appended, as `:848-849` says.
9. **PT-R9. Closed.** The characterization test before the relocation and after each later step
   (`:803-807`; T5 `:1185`); Detent's suite at every additive step (T9 `:1189`; K2, K7).
10. **PT-R10. Closed.** D18 at append, proposal, and recovery (`:1001-1025`; verified at
    `core.h:83, 645, 881`); rule 3 for the same-node window (`:374-378`); the wider hole stated
    (`:168-174, 185`); T7 (`:1187`).
11. **PT-R11. Closed.** Answers by incarnation from the placing assignment, else a linearizable
    read through the parent's leader, expiring with the record's version (`:396-412`);
    leader-only kinds (`:385-390`); the residual restated (`:187, 1154-1155`).
12. **PT-R12. Closed.** Held connections for fan-out and monitoring, guests at the acceptor
    (`:615-637`); the executor (`:781-787`); cost rows (`:1035, 1043`); T12 (`:1192`).
13. **PT-R13. Closed.** Two per source, sixteen in all, 2 s (`:645-652`); probes over held
    connections (`:638-640`); the residual (`:180`); T13 (`:1193`). PT-R26 adds co-located nodes.
14. **PT-R14. Closed.** "Every node runs N" defined (`:469`); the straggler's message
    (`:434-436`); directory formats by magic (`:468, 966`); feature gating in T3 (`:466, 1183`);
    Detent's candidate (`:447-449, 472`); the simulator's lanes and shared directory (`:135, 1190`).
15. **PT-R15. Closed** (`:312, 330-333`).
16. **PT-R16. Closed.** `lru-cache`'s plain `lru` (`:628-631`): its `unordered_map` is only looked
    up and the recency list orders eviction (`lru.h:298-299`). `frame_bytes` stays only on a
    measured gain (`:731-733`), and the budget moves to `ledger` (`:674-677`).
17. **PT-R17. Closed** (`:1181-1193`); T6's queue and dummynet (`:1186`). PT-R26 adds T13 on
    loopback.
18. **PT-R18. Closed** (`:126-139`; `:248-250`; `:676-677`). Applying the rows on `main` is the
    lead's and is owed.
19. **PT-R19. Closed** (`:875-899`).
20. **Conditions.** C1 partially closed: K1 to K9 match its list (`status/peer-transport.md:14-44`),
    and the lead's confirmation is owed. C2 partially closed: the parallel security review found
    PT-R7, PT-R11, and PT-R13 as SR2, SR1, and SR7, while D18 (PT-R10) and D16's epochs (PT-R24)
    still go to its re-check. C3 closed (K7; T9 `:1189`). C4 closed (T6 `:1186`). C5 closed
    (`:135`; T10 `:1190`).

### New findings

1. **PT-R20. Transfer control would size a 99 MiB control pool** (D11, `:718-722`; the cost
   table, `:1040`).
   - Control frames come from per-connection pools "sized by those counts and each kind's size"
     (`:720-721`), at 8 pending per group and connection (`:718-719`). On the bulk lane the
     largest control kind is now `TransferControl`, 6,480 bytes. That is 2,000 pairs × 8 × 6,480
     bytes, 98.9 MiB, outside the 256 MiB budget. The receive-memory row counts only 112-byte
     Raft frames, 1.7 MiB (`arith.out`).
   - **Fix.** A node takes at most `snapshot_concurrency` incoming transfers (Track C's D13,
     default 4), so pending `Begin`s are bounded per node (8, for example) and drop beyond it,
     like other control frames. `Accepted`, `Result`, and `Cancel` (at most 184 bytes) keep 8 per
     group. The pool is then 2.86 MiB (`arith.out`). The cost row states it.

2. **PT-R21. Releasing credit mid-fragment, idle grown windows, and early requests** (D11,
   `:690-714`).
   - A leader that steps down with a fragmented frame in progress and stops sending it stalls the
     receiver's reassembly, and `fragment_stalled` closes the connection (`:585-586, 593`). Every
     group the pair shares then resets because one leader changed. A sender finishes any
     fragmented frame it has begun, whatever its role, since that credit is already spent. It
     discards its queued data for the group, and only then releases the remainder.
   - An idle leader keeps a grown window until it steps down (`:1160-1162`). If many groups grow
     and go idle, they hold the growth pool, and a newly active group stays at w0 (64 KiB per
     round trip, 6.6 MB/s at 10 ms). Releasing what exceeds w0 after a stated idle time is
     sender-side and uses the existing field, so it is no wire change. T6 adds many idle grown
     groups beside one that becomes active.
   - A credit request from a node the binding does not admit yet, such as the new leader of a
     lagging replica before its vouch arrives (`:704-706`), has no stated fate. It is held until
     the vouch decides, not discarded, so the leader need not resend it.

3. **PT-R22. Rule 3 closes the session for a frame an honest node can send** (D5, `:374-375`; the
   table, `:592`). After a same-node replacement or a re-add, a frame the older incarnation sent
   before its state was removed can still be in flight. It can arrive after the receiver bound
   the newer one through the leader's append on another connection. As `forged_source`, it closes
   the pair's session and resets every group on it, against the table's own rule that only what
   an honest peer never sends closes (`:597-598`). Dropping it, counted as `superseded`, costs
   nothing in safety, since the frame is never admitted. The policy also discards a removed
   replica's queued frames.

4. **PT-R23. One compromised node can stop its neighbors' directory updates** (D16, hints,
   `:944-951`).
   - A node fetches "from the peer that advertised the highest version", once a second, and a
     failed reply ignores that peer's hint "for that version" (`:945-947`). A compromised node
     advertises a higher fake version in each hello and `Ping`, every 500 ms. Then every
     neighbor's one fetch a second goes to it, fails, and the honest version is never fetched.
     The 60 s anti-entropy fetch runs only after "no newer hint" (`:948`), which the fake hints
     always prevent. So the node keeps its own removal from reaching every node that hears it,
     and its sessions there survive the removal (D14, `:864`).
   - **Fix.** A peer whose reply fails verification, or times out, loses its hints until the node
     verifies that version or a higher one from another peer, with exponential backoff. The fetch
     target is the highest hint among peers in good standing. Anti-entropy runs every 60 s
     whatever the hints. T2 adds a node that advertises a rising fake version while a removal
     reaches its neighbors, and T10 measures that window.

5. **PT-R24. Signer epochs** (D16, `:905-938, 963-964`).
   - **Which epoch certifies a version.** A node checks "a majority of the named epoch's
     members" (`:936-938`). Neither the certificate nor the table says which epoch is in force at
     version v. S(v) covers the table only (`:924-925`), and `DTDIRE01` has no first version
     (`:964`). So a certificate that names an old epoch, signed by a majority of retired root
     voters whose keys outlived their service, verifies. Fix: `DTDIRE01` carries the first
     directory version it certifies, and a version is accepted only from the epoch whose range
     holds it.
   - **Forks under a retired majority.** A node still on epoch e accepts any e + 1 that a
     majority of e signs. Retired voters' keys can therefore fork a node that has not seen the
     real e + 1. State the residual, and the rule that retired root voters' keys are destroyed or
     re-keyed out.
   - **Root voters' keys.** Epoch 0 ties each member's SPKI to the seed's pin (`:908`). Later
     epochs tie nothing. Root voters hold `PublicKey` pins (kind 2), and each epoch member's SPKI
     digest must equal its pin at that version, checked where the epoch applies.
   - **The window.** "The root host proposes the next epoch" after the membership change commits
     (`:909-910`), a second commit that widens item 6's window. Each voter of epoch e can derive
     e + 1 from the committed membership and sign it as it applies that entry.

6. **PT-R25. Time in the root's replicated state** (D17, `:987-994`). A second admin's statement
   pending "for up to an hour" and limits of "10 a minute" are state that every root replica
   applies. Read from a replica's clock, they diverge between replicas. The time comes from the
   entry: a time the leader stamps in it, or the statement's own signed time within a stated skew.
   State which.

7. **PT-R26. Smaller points.**
   - **Co-located nodes.** Two handshakes per source (`:645-648`) also bind nodes that share an
     address: containers with host networking, and the harness's nodes on one host. T13's
     three-node loopback cluster (`:1193`) has a single source unless its nodes bind distinct
     addresses (127.0.0.x on Linux, `lo0` aliases on macOS). State the case, and the operator's
     way to list co-located peers.
   - **The startup version report** (`:469`) has no path or cost: 100,000 reports per rolling
     upgrade, 1.16 a second over 24 h (`arith.out`). Batch them into root entries, or keep them
     as soft state, and say which.
   - **SR4's cases.** T7 adds a minority of voters reporting the floor while commands at the
     group's limit commit, and a majority reporting lower limits, where admission falls to theirs
     and nothing wedges.
   - **One version per replacement.** A lost node's `Remove` and its replacement's `Join` fit one
     signed statement (`:978-980`: up to 256 commands), so one version instead of the cost
     table's two (`:1045`).
   - **The ledger.** Its state line still reads "ready for preliminary review"
     (`status/peer-transport.md:11`).

### New blocking items

None for steps 1 to 3, 5, and 6, nor for drafting step 4's header and kinds 13, 14, and 16. These
bind later code:

1. Step 4's flow-control code: PT-R20 and PT-R21.
2. Step 5's binding code: PT-R22.
3. Step 8's code (strata's D16 and D17): PT-R23, PT-R24, and PT-R25, each a security-tier point
   for the security reviewer's re-check too.

### Step 4's drafting

Yes: the frame header (`:529-547`), `Ping` (`:560`), `Pong` (`:561`), and `Chunk` (`:563`) may be
drafted now. Their bytes are settled, and no finding here touches them. Kinds 1 to 8 in the new
envelope, with today's bodies, may be drafted too. From the architect's side, the bytes of the
hello and of kinds 15, 17, and 18 are also settled, since PT-R20 and PT-R21 change behavior, not
bytes. They wait only for the security reviewer's re-check of SR5, SR6, and SR8, which own their
value rules. Kinds 9 to 12 wait for C2's bodies. Nothing lands before the full design review, the
security review, and Detent's agreement to Q4 (K8).

### Conditions

1. **C6 (PT-R20, PT-R21)** before step 4's flow-control code: the transfer-control pool bounded
   per node, and the release, idle-window, and early-request rules, with their T4 and T6 cases.
2. **C7 (PT-R22)** before step 5's code: the lower incarnation dropped, not closed.
3. **C8 (PT-R23 to PT-R25)** before step 8's code: rehabilitated hints with periodic
   anti-entropy; epochs with a first version, SPKI tied to pins, signing at apply, and the
   retired-key residual; time from the entry.
4. **C9 (PT-R26)** in the next edit of the design or the ledger.
5. **C1 and C2, carried:** the lead confirms K1 to K9, and the security reviewer's re-check covers
   D18 and D16's epochs.

Not run: no build, test, sanitizer, fuzzing, or benchmark, and no compiled probe (`quiet check`
exits 1: the `strata-trials` strict hold, now `night_gate.sh n1017` until 05:16); only the
arithmetic probe ran, in Python. No simulator, network, cluster, VM, or bare-metal host. Detent's
suite not run. The security reviewer's re-check not read (it runs separately). No Xapian or
Xapiand GPL source opened.

## Second re-check

- Date: 2026-10-17, read from 19:58 to 20:40 PDT.
- Reviewed: `git log 2598de2..design/peer-transport`, four commits: `590505e` (the lead's
  confirmation of K1 to K9, and K10), `2949082` (ledger), `6c49839` (revision 2), and `65d6f5e`
  (ledger). `git diff 90d417e 6c49839 -- docs/design/peer-transport.md` read whole, and the design
  at `6c49839` read whole (1,532 lines); it is unchanged at `65d6f5e`, and neither review file
  changed in the range. The branch equals `origin`. Line numbers below without a file are the
  design's at `6c49839`.
- Checked against:
  - PT-R20 to PT-R26 and conditions C1, C2, and C6 to C9 above; the security re-check's SR15 to
    SR18 (`peer-transport-security.md:685-770`) where revision 2 departs from them; the ledger's
    K10 and Q7 (`status/peer-transport.md:46-49, 130-133`).
  - PLAN's owner decisions at strata `main` `a62e9f9`, which moved from the branch's base
    `c5b571a` by twelve commits; only `89a7416` and `32222a7` (SYS-13) touch what this design
    cites.
    Track C's D3, D5, D13, and D17 (`track-c.md:255-266, 318-327, 669-674, 688-724`) and the C1
    gate's scale notes (`gates/track-c-c1.md:361-366`). `calibration.md` at `main`: six rows,
    applied below.
  - Detent's `SHARED_LIBRARY_DECISIONS.md` section 6 at `github/main` `2b83cf2`, read only. The
    local `18bd660` is two unpushed commits ahead (`5f4343d`, `18bd660`), which change sections 4
    and 5 only.
  - Code at consensus `fc66c0e` (`origin/main`): `membership.h:37-140, 161-194`;
    `core.h:275-320, 360-405, 600-660, 733-740, 785-800, 998-1027`; `snapshot_channel.h:33-276`;
    `snapshot_transfer.h:29-87`; `snapshot.h:35-96`; `types.h:55-127`; `worker.h:285-310`;
    `storage.h:168-172, 220-224, 263-267`. The local `ef30b4f` holds two unpushed commits
    (`c2cbf2e`, `ef30b4f`) that change `core.h` at :1141 and move `storage.h`'s membership decodes
    (PT-R32). reactor `e19afd9`, consensus's pin; reactor's `origin/main` is now `1c37def`, seven
    commits for the UDP receive loop, with `reactor_tls.h` unchanged, so D14's citations hold.
    Xapiand `design/pki` `bfc11c50c`: `pki.md` D4's key types (P-256, P-384, RSA 2,048 to 4,096).
- Checked by:
  - The designer's arithmetic rerun: `.scratch/peer-transport-r2/arith.py` under Python 3.15,
    isolated (`-I`), at nice 19. Its output equals `arith.out` byte for byte
    (`~/.scratch/review-peer-transport/designer-r2-rerun.out`).
  - A probe of this review's own, `~/.scratch/review-peer-transport/recheck2.py`, output
    `recheck2.out` beside it: transfer-control sizes from the code's constants, a slot-holding
    trickle, the `Begin` resend load, stranded credit, Q7's reads with removals, the signer
    majority against a commit quorum in a same-node window, the fetch turn against highest-first
    for 1 to 512 lying peers (a small simulation), the pools that bound T6's idle case, and
    held-request and channel-buffer memory. Every derived number below is that file's or the
    designer's.
  - `git grep snapshot_concurrency` over consensus at `fc66c0e` and `ef30b4f`: no match.
  - `quiet check` exits 0; nothing compiled, since a design needs no build.
  - Not run: the last line of this section.

### Verdict

**APPROVE WITH CONDITIONS.** PT-R20 to PT-R26 close, and C1 and C6 to C9 close; C2 is carried in
part. The seven places revision 2 departs or asks (`:1514-1532`) all stand, each ruling naming
the follow-up it leads to. Q7: the lead's provisional call stands (state the residual, and let
Track D3 decide), with option 2 restated, since as written it does not reach its own goal. Six
new items: three bind step 4's flow-control code (PT-R27 to PT-R29, the last also Track C's
transfer pool), two bind step 8 (PT-R30, PT-R31), and PT-R32 is small points. None changes a byte
that step 4 may draft now.

What holds, and reproduces:

- **The arithmetic.** The designer's reproduces byte for byte. The transfer-control sizes
  reproduce from the code's constants (`snapshot_transfer.h:37, 68-78`; `snapshot.h:51-53`): a
  `Begin` is 6,480 bytes at 255 members and 432 at three; `Accepted` and `Cancel` 168, `Result`
  184. The control pools are 15.2 MiB at 341 connections and 36.6 MiB at 1,024.
- **D18's shapes** are the two honest states of Track C's same-node replacement, and SR17's
  literal shape is neither (ruled below).
- **Rule 3** closes SR16 and its committed variant: no leader silences an honest voter by naming
  a second incarnation on its node, committed or not (`:420-424`).
- **The turn** bounds k lying candidates to k fetch slots, where highest-first takes 2, 5, 9,
  39, and 96 slots for k of 1, 2, 3, 8, and 16 (`recheck2.out`).
- **Epochs keyed to first versions** refuse an old epoch's certificate for a newer version at
  every node that holds the newer epoch, and tie each member to its kind-2 pins at the epoch's
  first version (`:1020-1029, 1065-1074`). At an odd voter count, a commit quorum holds a signer
  majority even while one node holds two voters (`recheck2.out`), so "certifying needs no more
  than committing" holds there.
- **Stamps** make the pending hour and the rate limits the same at every replica
  (`:1144-1155`), and T2 tests them at replicas whose clocks differ by an hour (`:1433`).

What binds later code: PT-R27, PT-R28, and PT-R29 (step 4's flow-control code and Track C's
transfer pool), and PT-R30 and PT-R31 (step 8).

### The departures, ruled

1. **D18's two shapes in place of SR17's** (`:1210-1224`). **Accepted.** `validate_change`
   refuses a new `leaving` unless the same entry makes exactly one voting change
   (`membership.h:139`), so `Replace` promotes the learner and marks the old voter in one entry
   (`track-c.md:262-264`). The honest states are a voting member beside its learner before
   `Replace` and the `leaving` member beside its replacement after it. SR17's "a leaving voter and
   a learner" is neither, so as written it would refuse every same-node replacement at its first
   step. Two precisions are PT-R32's: state the rule by `voting()`, since a same-node witness
   replacement holds two witnesses on one node, and note that a host replaces a failed learner on
   its node in one change. K10's "one voting member per node" reads as one besides the `leaving`
   member, as `:1222-1224` says; K10 is the lead's, so the lead amends its text on this ruling
   (C11).
2. **Rule 3 by committed memberships and vouches, never a learner, and rule 2.3 dropping**
   (`:395-424`). **Accepted.** It is stricter than SR16 asked: "committed bindings" alone would
   let a committed learner beside a voter silence it, and any leader may commit a learner. The
   residual double grant while a replacement is uncommitted (`:464-472`) adds nothing to what a
   compromised voter has, since it can grant two candidates in one term. Rule 2.3's drop costs no
   safety, because the frame is never admitted either way. A close cost a liar only a reconnect,
   and it cost an honest pair every shared group at each election timeout of a server that
   lagged past its own removal (§4.2.3). `retired_source` keeps the signal. PT-R27 follows from
   it: drops now carry data frames whose credit a close used to reset.
3. **Fetches in turn, and epochs with first versions** (`:1020-1029, 1080-1107`). **Accepted.**
   The simulation above confirms the designer's reason for the turn: under highest-first, every
   liar whose distrust just ended goes first again. Which peers count as candidates is PT-R30.
   What `epoch_conflict` halts is PT-R31.
4. **`Remove` keyed to `drained`, and the 10-minute skew** (`:1158-1175, 1144-1155`).
   **Accepted.** "Draining" would let one admin `Drain` and then `Remove` a node that still holds
   replicas, and "dead" is a detector's opinion. `drained` is placement's record that nothing is
   left there, and a dead node's removal gains nothing from coming before its re-replication.
   The cost row's "an admin" now needs two (PT-R32).
5. **The node's `Begin` pool, growth only on `wanted`, and `window_idle`** (`:767-819`).
   **Accepted.** One connection's `Begin`s can make another's drop, at the price of one 500 ms
   resend and never a close. What the pool rests on is PT-R29, and T6's idle case is PT-R32.
6. **Q7.** Ruled below.
7. **The `limits.md` and Phase 4 row** (`:160`). **Accepted.** The 256 MiB is Track C's RPC
   decoder ceiling (`track-c.md:163-166`), which never bounded host frames. The rows' mechanism
   becomes the announced host data bound, and calibration row 6 applies when Track D5 names it
   (PT-R32).

### Q7

**Option 1 now, as the lead's provisional call says, on two conditions (C13).**

- **Why.** A compromised leader already corrupts the groups it leads: it can raise followers'
  commit indexes over entries no quorum holds (`core.h:884-888`), grant two votes, and fence
  replicas, which is the compromised-voter row (`:213`). Q7's check removes none of that. It
  would make only the group's membership recoverable once the leader is evicted. The trust
  boundary is unchanged, nothing before step 8 depends on it, and Track D3's records are what
  option 2 would read. Deferring is reversible: option 2's append-path hook is additive, since
  fixed groups have no `Configuration` entries.
- **Option 2 as written does not reach its goal** (`:1480-1486`). It checks adds and promotions,
  but removals are valid single changes that every follower accepts. From {A, B, C}, a
  compromised A removes C, committed with B's honest acknowledgment, and then B, which the quorum
  of {A} commits alone (`recheck2.out`); evicting A then leaves no voter. "A leader changes a
  membership only as placement decided" needs every voting change and every `leaving` mark
  checked against the record. That is about 1,794 reads per lost node at rf = 3 in place of
  1,196, about 19,700 a day at nominal loss (`recheck2.out`), and the records carry roles and
  the target membership.
- **Who decides.** Track D3's design, in the security tier, before strata runs membership
  changes outside tests, with option 1's detection built either way: D3's reconciliation reports
  a group whose committed membership departs from its assignment.

### Status

1. **PT-R20. Closed.** The node's `Begin` pool, 2 per connection and 8 per node (`:809-819`), with
   `busy` drops (`:654`), the limit row (`:1286`), the cost row (`:1255`), and T4's case
   (`:1435`). The channel resends at `rpc_timeout` (`:840-846`; `types.h:114`), and the receiver
   answers again (`snapshot_channel.h:224-232`). The resend costs little: 248 KB/s from the 287
   sources waiting at a node that receives a whole node's 299 replicas, at three-member
   descriptors (`recheck2.out`). What the pool rests on is PT-R29.
2. **PT-R21. Closed.** A begun fragment is finished before release (`:776-780`); `window_idle`
   (`:780-785`); growth only on `wanted`, with revision 1's doubling gone (`:771-775`); requests
   held until the binding decides (`:754-760`; limit `:1287`); T4 and T6 (`:1435, 1437`). T6's
   idle case names a count the pools forbid (PT-R32). How removals meet these rules is PT-R27,
   and how held records meet D9's bound is PT-R28.
3. **PT-R22. Closed.** Rule 3 drops as `superseded` and keeps the connection (`:410-424`); a
   removed replica's queued frames are discarded (`:432-435`; D12 `:868`); the event table and
   its reason (`:654, 657-662`); T7 (`:1438`). Rule 2.3's drop is ruled above.
4. **PT-R23. Closed.** Distrust from 1 s doubling to 60 s, fetches in turn, anti-entropy on a
   timer, and the table after a failed delta (`:1080-1107`; D12 `:864, 867, 874-881`; D13
   `:929-932`); the threat row (`:220`); T2 and T10 (`:1433, 1441`). The bound holds for the
   candidates it counts; which peers count is PT-R30.
5. **PT-R24. Closed.** First versions and ranges (`:1020-1029`); kind-2 pins tied to each member
   at the first version and checked at each node (`:1012-1015, 1065-1074`); signers' SPKIs in
   `DTDIRC01` at most 1,024 bytes (`:1119`), which the PKI profile's largest key, RSA 4,096 at
   550 bytes, fits; signing at apply (`:1023-1025`); voter changes only while a majority is up
   (`:1036-1041`); the retired-key rule and `epoch_conflict` (`:1042-1050`; limits
   `:1397-1401`); T2 (`:1433`). The gate on voter changes covers the host's proposals but not the
   core's own removal of a `leaving` voter, which the sentence PT-R32 asks for covers.
   `epoch_conflict`'s scope is PT-R31.
6. **PT-R25. Closed.** The leader's stamp, the 10-minute skew, and what a compromised leader
   keeps (`:1144-1155`); `DTADMS01` and the entry that carries it (`:1188-1189`); the threat row
   and the limits (`:215, 1406-1408`); T2 with skewed clocks (`:1433`).
7. **PT-R26. Closed.** Co-located nodes with per-range caps (`:717-722`) and T13's distinct
   addresses (`:1444`); version reports batched into root entries (`:526`; cost `:1264`); D8's
   storage sentence (`:574-577`); SR4's T7 cases (`:1438`); one version per replacement
   (`:1260`); the ledger's state (`status/peer-transport.md:12`). Three precisions are
   PT-R32's: the reports' durability before a batch commits, the cap past eight nodes, and the
   cost row's admin count.
8. **Conditions.**
   - C1 closed: the lead confirmed K1 to K9 and added K10 (`status/peer-transport.md:46-49`).
   - C2 partially closed: the security re-check covered D18 (SR17,
     `peer-transport-security.md:732-757`). Revision 2's epochs (PT-R24) go to its re-check of
     revision 2, with PT-R30.
   - C6 closed (items 1 and 2); PT-R27 to PT-R29 bind the same code.
   - C7 closed (item 3).
   - C8 closed (items 4 to 6); PT-R30 and PT-R31 bind the same code.
   - C9 closed (item 7).

### New findings

1. **PT-R27. Drops and releases leave credit and fragments unaccounted** (D11 `:767-785,
   835-839`; D9 `:637-646, 654`; D5 `:432-435`).
   - **Credit a pair stops needing.** A sender releases a group's credit when it stops leading
     or ends a transfer, and above w0 when idle (`:776-785`). When the receiver's replica leaves
     the group while the leader stays, which is when the core stops replicating to it
     (`core.h:1004-1011`), nothing releases the w0 the leader keeps on that connection, and the
     receiver cannot take it back. Each such pair strands 64 KiB of the receiver's pools until
     the leader steps down. That is 18.8 MiB in the steady state at ten replicas leaving a node a
     day with 30-day leaders, and 512 strandings from one leader fill its share
     (`recheck2.out`). Locality copies that come and go (Track D3) make it routine.
   - **Dropped data frames.** Revision 2 turns two closes into drops (rules 2.3 and 3), so
     drops now carry data frames whose credit a close used to reset. Nothing says whether credit
     is checked before hosting and binding, what a dropped frame's credit does, or what becomes
     of a fragmented frame dropped at its first fragment (`:639-642`). With no frame in
     progress, its second fragment fails "each offset equals the bytes received" and closes the
     pair, so the drop becomes a close one fragment later.
   - **A retire against a begun fragment.** `retired{group}` discards a removed replica's queued
     frames (`:432-435`), while the release rule finishes a begun fragment first only on a role
     change or a transfer's end (`:776-780`). A replica the host removes mid-fragment, a fenced
     leader for one, leaves the receiver's reassembly open into `fragment_stalled`
     (`:645-646, 653`): the close PT-R21 removed for role changes returns for removals.
   - **Fix.** A sender releases a group's credit on a connection once it has no reason to send
     that group's data there: it no longer leads the group with a member on that node, nor runs a
     transfer to it. On `retired{group}` it first finishes a begun fragment, then discards and
     releases. Credit is checked from the header before hosting and binding, since an honest
     sender never exceeds it. A data frame that then drops spends its credit, its bytes return to
     the receiver's pools as they arrive, and nothing returns to the sender. A fragmented frame
     dropped at offset 0 stays in progress as a discard, its later fragments read and counted
     under the same reason. T4 adds each case, with mutants for the release on unsharing off and
     for a retire that abandons a fragment.

2. **PT-R28. Record bounds by the receiver's view close an honest connection** (D9 `:618, 622,
   630-632`; limits `:1288`; D10 `:680`).
   - A coalesced heartbeat naming "more groups than the receiver shares with the sender" is a
     protocol error (`:630-632`), and a `Window` holds at most "the groups shared plus group
     zero" (`:622, 1288`), where sharing is read from the receiver's memberships (`:680`). A
     receiver that lags the change that added the sender's node to a group counts one group fewer
     than the sender, 30 against 31 at the contract's shares (`recheck2.out`). The new leader's
     coalesced heartbeat names every follower it has on that node (Track C's D12), and its
     `Window` can carry the `wanted` that `:754-760` holds for exactly this case. Either closes
     the pair, resetting every group on it, and each reconnect repeats it until a vouch places the
     leader. A close read before the leader's first group frame starts no vouch at all.
   - **Fix.** Bound records by the groups the receiver hosts, plus group zero for `Window`, and
     judge each record by itself: one for a group not hosted drops, and one whose source the
     binding cannot place is held (a `wanted`, `:754-760`) or dropped (`not_leader`,
     `:436-441`), counted. One record per group stays the rule, and the 1 MiB wire maximum still
     bounds a frame (26,213 `Window` records). T4 or T7 adds a lagging receiver given both frames
     for a group whose leader it cannot place yet: no close. C2's D12 carries the bound for kinds
     9 and 10.

3. **PT-R29. The `Begin` cap rests on a cap consensus does not have, and the transfer slots
   have no per-source share** (D11 `:809-819, 840-846`; Track C's D13 and D17).
   - **The channel holds what the pool should.** The cap counts frames the transport holds, but
     today's channel consumes a `Begin` it answers `Busy` into its own 65,688-byte buffer and
     retries it each tick (`snapshot_channel.h:86-95, 122-129, 185`). So a node holds a pending
     `Begin` per group and peer outside the pool, and each resend of a held `Begin` takes one of
     its source's two slots. Track C's D17 allocates snapshot buffers per transfer from a
     transfer pool, not per peer (`track-c.md:719`; the C1 gate's 131 MB,
     `gates/track-c-c1.md:363-366`), so in version 2 only the pool can hold a pending `Begin`,
     and the design does not say so.
   - **The cap itself.** "A node takes at most `snapshot_concurrency` incoming transfers"
     (`:811-812`) is Track C's D13 and D17 design (`track-c.md:673-674, 715-718`), not code.
     `git grep snapshot_concurrency` at consensus `fc66c0e` and `ef30b4f` finds nothing, and
     `reserve_snapshot` reserves per Worker (`worker.h:293-310`). The first re-check's fix for
     PT-R20 stated the cap as existing; that was this review's error.
   - **One source holds every slot.** The cap has no per-source share. `reserve_snapshot` takes a
     transfer from any member at a term at or above the receiver's (`worker.h:296-299`), so a
     member of four groups with replicas on a node holds all four incoming slots with one full
     chunk every 59 s each, 4,456 bytes a second in all, which the channel's 60 s inactivity
     close never reaches (`snapshot_channel.h:131`; `recheck2.out`). Every other group's install
     at that node then waits: one node's effect crosses into groups not placed on it, which the
     threat model's flood row says the bounds prevent (`:221`). An honest slow source does the
     same to fewer groups. Revision 2 limits a source to a quarter of the pending pool; the
     active slots need the same.
   - **Fix.** In version 2 the channel takes a frame only when it can process it (`Busy`
     consumes nothing), so pending `Begin`s live in the pool, and the pool keeps one pending
     `Begin` per connection and group, a resend replacing it. D11 names the incoming-transfer
     cap's owner and order: Track C's transfer pool, with or before step 4's caps. That pool
     gives one source node at most half its incoming slots, 2 of 4, a row in "Changes to other
     documents" for Track C's D17. T4 adds a resend that replaces a pending `Begin`, and Track C's
     tests add a source that holds slots by trickling.

4. **PT-R30. One admin key fields enough liars to void the turn's bound** (D16 `:1080-1107`; D10
   `:687-688`; D17 `:1166-1167, 1176-1178`; threat rows `:217, 220`).
   - Hints are the versions in hellos and `Ping`s from any session (`:1080-1083`), guests'
     included, and the turn bounds the delay to one slot per lying candidate (`:1098-1100`).
     `Join` takes one admin at up to 1,000 a minute (`:1166-1167, 1176-1178`), and a joined node
     authenticates at once, since `Join` adds its pin (`:338`). So one stolen admin key registers
     1,000 identities a minute, each able to sit in any node's guest slots (512, `:687-688`)
     advertising a version it cannot certify. With 512 lying guests the honest version waits up
     to 512 slots, about 8.5 minutes, against T10's 60 s at p99 (`recheck2.out`). The threat row
     for one admin key lists "a registered node that placement never uses" (`:217`), not an
     authenticated peer. Cleanup is slower than the attack: `Remove` is 10 a minute per admin
     (`:1176-1177`).
   - **Fix.** Hints count only from sessions the node holds for its own reasons (group, held, and
     contact connections, D10); a guest's hello version is no hint. A joined node then lies only to
     nodes that chose it, which takes placement (`Activate`, two admins) or the host's topology,
     and Track D2's monitoring topology admits a node only once activated (a row for D2). A hint
     is the sender's own held version, never a relayed claim, so D2's gossip, if it carries
     versions, never gets an honest relay distrusted. Removing a node that never left `joining`
     sits outside `Remove`'s rate as it sits outside the two-admin rule, so a revoked admin's
     registrations go as fast as they came. The threat row says what a one-admin `Join` buys: an
     authenticated peer. T2 adds 512 guests advertising rising versions: no fetch to any of them.

5. **PT-R31. `epoch_conflict` halts nodes the forged epoch cannot fork** (D16 `:1046-1050`).
   - A node "offered two different epochs with one number, each signed by a majority of the epoch
     before" applies no version until an operator re-seeds it. A node past epoch e + 1 holds the
     real one and checks every later version against its own chain, so a forged e + 1 signed by
     e's retired keys cannot fork it; as written it halts anyway. A retired majority then halts
     every node it can reach, where the stated residual is the nodes still on e (`:1397-1401`),
     and each halted node needs an operator's re-seed: at 100,000 nodes, a cluster-wide outage of
     directory updates.
   - **Fix.** A node verifies offered epochs only above the one it holds; one at or below is
     ignored, counted (`stale_epoch`), and logged with its signers. A node on e offered two
     different e + 1 halts as stated, since it cannot choose. T2 adds a forged e + 1 offered to a
     node on e + 3: counted, no halt.

6. **PT-R32. Smaller points.**
   1. D18's node rule by `voting()`: a same-node witness replacement holds two witnesses on one
      node in its window (`membership.h:108-114`), which "the voter that replaced it"
      (`:1213-1214`) leaves out. With two learners on one node refused (`:1214-1215`), a host
      replaces a failed learner on its node in one change, a row for Track D3.
   2. D14's key-file rule says "a group other than the effective one" (`:952-954`). SYS-13 as
      amended on `main` (`32222a7`, half an hour before revision 2) is the group the process runs
      as, the one it drops to for a root start (`PLAN.md:42-45` at `a62e9f9`); K5's tests follow
      it. PLAN's later lines moved by one (SYS-14 is now :46-47).
   3. Version reports (`:526`): a member's unbatched reports die with it, so "entries, not soft
      state, so a leader change loses none" holds only once a batch commits. A report is
      acknowledged when its batch commits and resent otherwise, to the next epoch member when its
      own is down.
   4. Fetches (`:1084-1086`): a slot is spent when the request is sent and fetches overlap, so a
      peer that never answers costs one slot, the threat row's "about a second" (`:220`), not
      its 5 s deadline. The deadline runs to the reply's first byte, with D9's fragment rule after
      it, since a table reply is up to 8 MiB.
   5. T6's idle case (`:1437`): at B = 256 MiB the share holds 8 windows of 4 MiB per peer (the
      designer's own `arith.out`) and the growth pool 46 node-wide (`recheck2.out`), not 100;
      the row states the count the pools allow.
   6. D10 (`:719-721`): "a cap of 2k ... still under the 16 in all" holds for k ≤ 8; past it the
      total decides. Say that an entry for one address does not narrow the ranges (the most
      specific entry gives the cap, and `any` stays unless removed), and let an entry set the
      prefix that defines a source, since IPv6 pod networks that allocate small blocks put many
      nodes in one /64.
   7. The cost row for directory versions (`:1260`): a lost node is not `drained` when its
      `Remove` would share a statement with its replacement's `Join`, so that statement takes two
      admins (`:1158-1168`); otherwise the `Remove` waits for `drained` and the versions are two.
      Either way it is at most 22 a day at nominal loss.
   8. The receive-memory row (`:1255`) is the transport's. Beside it sit held requests, at most
      32 bytes per hosted group per connection (32 KB at 1,000 groups, 31.2 MiB at 1,024
      connections, an estimate), and the snapshot channel's per-peer buffers, 131 MB at 1,000
      groups until Track C's D17 moves them (`gates/track-c-c1.md:363-366`; `recheck2.out`).
      Naming both keeps the row from being read as the node's total.
   9. The header's "every line cited holds there too" (`:22-23`) misses `storage.h`: the local
      `ef30b4f` moves the decodes D18 cites (`:1217-1218`) from :170, 222, and 265 to :69, 229,
      and 272. Cite them at `origin/main` or by function.
   10. When Track D5 names the host data bound its shard RPC needs (`:160`), the `limits.md` rows
       take calibration row 6's comparison with the references.
   11. D16's liveness bullet (`:1036-1041`) gates the host's voter changes; the core's own removal
       of a `leaving` voter (`core.h:1021-1025`) is not one of them. It needs a commit quorum,
       which at an odd voter count holds a signer majority even while one node holds two voters
       (`recheck2.out`). One sentence says so.

### New blocking items

None for steps 1 to 3, 5, and 6, nor for drafting step 4's frame header, hello, and kinds 1 to 8
and 13 to 18 (`:1499`): no new item changes a byte. These bind later code:

1. Step 4's flow-control code: PT-R27, PT-R28 (with C2's bound for kinds 9 and 10), and
   PT-R29's channel and pool rules.
2. Track C's D17 transfer pool: PT-R29's per-source share.
3. Step 8's code, including the policy's hint rules that land with it (`:1324`): PT-R30 and
   PT-R31, each a security-tier point for the security reviewer's re-check of revision 2.

### Conditions

1. **C10 (PT-R27 to PT-R29)** before step 4's flow-control code: the release on unsharing, a
   dropped frame's credit and fragments, a retire that finishes its fragment, records bounded by
   hosted groups, a channel that consumes no `Busy` frame, one pending `Begin` per connection and
   group, and the incoming-transfer cap's owner named, each with its T4 or T7 case; Track C's D17
   row for the per-source share.
2. **C11 (K10)**: the lead amends K10's "one voting member per node" to "one voting member per
   node besides a same-node replacement's `leaving` member", on this ruling.
3. **C12 (PT-R30, PT-R31)** before step 8's code: hints only from chosen connections, joining
   nodes removed outside `Remove`'s rate, the threat row, and epochs verified only above the held
   one, with their T2 cases.
4. **C13 (Q7)**: Q7's option 2 and the ledger's Q7 line cover every voting change and `leaving`
   mark; Track D3 decides Q7 in the security tier before strata runs membership changes outside
   tests, and builds option 1's reconciliation report either way.
5. **C14 (PT-R32)** in the next edit of the design.
6. **C15 (for Detent)**: the design's "For Detent" item 4 gains C10's rules once they fold. The
   lead's update of Detent's section 6 starts from Detent's local `18bd660`, which is two
   unpushed commits ahead of `github/main`.
7. **C2, carried**: the security reviewer's re-check of revision 2 covers D16's epochs (PT-R24)
   and PT-R30.

### For calibration (the lead's to append)

1. Missed by this review's first re-check: PT-R20's fix named `snapshot_concurrency` as a cap
   nodes enforce, and it is Track C's design, absent from consensus (PT-R29). The check: for every
   mechanism a fix relies on, find it in the code at the cited commit, or name it as owed, with
   its owner.
2. Missed by this review's preliminary review and first re-check: D9's record bound counts the
   receiver's view, which an honest sender exceeds while the receiver lags (PT-R28), and credit a
   pair stops needing has no release (PT-R27); both were in revision 1. The check: for every close
   rule and every grant, construct the honest peer whose view lags the receiver's (a membership
   change, a removal, a reconnect), and name the event that ends each grant.

Not run: no build, test, sanitizer, fuzzing, or benchmark, and no compiled probe, since a design
needs none (`quiet check` exits 0); only the designer's arithmetic and this review's Python probe
ran, at nice 19. No simulator tier, network, cluster, VM, or bare-metal host. Detent's suite not
run; Detent read in section 6 and its git state only. The security reviewer's re-check of
revision 2 not read (it runs separately). No Xapian or Xapiand GPL source opened.
