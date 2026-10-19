#!/usr/bin/env python3
"""durability-modes: the arithmetic behind the design's derived figures.

Inputs are the measured medians in results/2026-10-18-t2-macos.txt (microseconds of CPU per
document, one thread) and the default timing profile (track-c.md:376-380). Everything printed is
arithmetic over them, labeled as an estimate in the design.
"""

from statistics import median

# Measured, per document (results/2026-10-18-t2-macos.txt).
apply = {"one flush (256 MiB buffer)": median([42.34, 41.74, 41.29]),
         "16 MiB buffer, 37 segments": median([128.24, 124.91, 130.10])}
install = {"one flush (256 MiB buffer)": median([0.72, 0.77]),
           "16 MiB buffer, 37 segments": median([1.62, 1.48])}
append = median([1.02, 1.10])
encode = median([0.182, 0.173]) * 1e6 / 100_000  # leader, once per batch
log_bytes = 117_917_573 / 100_000
shipped = {"one flush (256 MiB buffer)": 43_493_556 / 100_000,
           "16 MiB buffer, 37 segments": 121_481_780 / 100_000}

print(f"encode {encode:.2f} us/doc, append {append:.2f} us/doc, log {log_bytes:.0f} B/doc")
for config in apply:
    a, i = apply[config], install[config]
    doc_follower = a + append
    seg_follower = append + i
    print(f"\n{config}: apply+flush+merge {a:.2f}, install {i:.3f} us/doc")
    print(f"  follower: document {doc_follower:.2f}, segment {seg_follower:.2f} us/doc, "
          f"ratio {doc_follower / seg_follower:.1f}x")
    for rf in (3, 5):
        document = encode + rf * a + rf * append
        segment = encode + a + rf * append + (rf - 1) * i
        print(f"  rf={rf}: group CPU document {document:.1f}, segment {segment:.1f} us/doc, "
              f"segment/document {segment / document:.3f}")
    print(f"  wire per follower: document {log_bytes:.0f} B/doc, segment {log_bytes + shipped[config]:.0f} B/doc "
          f"({shipped[config]:.0f} B/doc of segments, +{100 * shipped[config] / log_bytes:.0f}%)")
    # D9: the documents a new leader replays within one election_min (1 s) on one thread.
    print(f"  replay within election_min = 1 s: {1e6 / a:,.0f} documents")

# D9's failover gap at the default profile (track-c.md:370-380, 836-838): elections drawn
# from 1,000-2,000 ms, an election round of two barriers and a round trip at b = 80 ms p99.
election_max, rounds, round_ms, barrier_ms, rtt_ms = 2000, 2, 161, 80, 1
detect = election_max
strict = detect + rounds * round_ms + rtt_ms + barrier_ms
print(f"\nfailover gap estimate, default profile: strict {strict} ms; "
      f"segment mode adds replay up to election_min (1,000 ms): {strict + 1000} ms")
quiescent = 3 * 500 + election_max + rounds * round_ms + rtt_ms + barrier_ms
print(f"quiescent group (three missed 500 ms keepalives first): {quiescent} ms")

# --- Revision 1 (DM-R12, DM-R17): the same parts, plus what revision 0 left out. ----------------
# Assumptions, labeled, none measured here (T2 measures them on the lead's hosts):
#   SHA-256 through OpenSSL's EVP: 0.5 GB/s per core without SHA extensions (the development Mac's
#     i9-9980HK has AVX2 and AES-NI but no SHA-NI), 1.5 GB/s with SHA-NI or ARMv8 SHA2. The leader
#     hashes each shipped byte once, at the Image; every follower once, whatever the source (DMS-5).
#   TLS 1.3 (AES-GCM) and the kernel's TCP path per byte moved: 0.5 to 1.0 ns to send and the same
#     to receive, so 1 to 2 ns for both ends; both modes move the log, segment mode also segments.
#   The follower's verification (D18, Q9 option 2): 1 in 64 flushes rebuilt, so A / 64 per document.
print("\n# Revision 1: group CPU with SHA-256, TLS and the stack, and the verifier (estimates)")
for config in apply:
    a, i, sb = apply[config], install[config], shipped[config]
    print(f"\n{config}: shipped {sb:.0f} B/doc of segments, {log_bytes:.0f} B/doc of log")
    for sha_gbps in (0.5, 1.5):
        h = 1e-3 / sha_gbps  # us per byte
        for t_ns in (1.0, 2.0):
            t = t_ns * 1e-3  # us per byte, both ends together
            doc_f = a + append + log_bytes * t / 2
            seg_f = append + i + sb * h + (log_bytes + sb) * t / 2
            row = [f"SHA {sha_gbps} GB/s, transport {t_ns:.0f} ns/B: follower {seg_f:.2f} vs {doc_f:.2f} us/doc "
                   f"({seg_f / doc_f:.3f}x, {(seg_f + a / 64) / doc_f:.3f}x verified)"]
            for rf in (3, 5):
                document = encode + rf * a + rf * append + (rf - 1) * log_bytes * t
                segment = encode + a + rf * append + sb * h + (rf - 1) * (i + sb * h) + (rf - 1) * (log_bytes + sb) * t
                verified = segment + (rf - 1) * a / 64
                row.append(f"rf={rf} {segment / document:.3f}x, {verified / document:.3f}x verified")
            print("  " + "; ".join(row))

# The cadence bracket (DM-R12): one large index, segment mode at the 16 MiB cadence against
# document mode at its best (one flush), at the slower assumptions.
h, t = 1e-3 / 0.5, 2.0e-3
one, small = "one flush (256 MiB buffer)", "16 MiB buffer, 37 segments"
for rf in (3, 5):
    document = encode + rf * apply[one] + rf * append + (rf - 1) * log_bytes * t
    segment = (encode + apply[small] + rf * append + shipped[small] * h + (rf - 1) * (install[small] + shipped[small] * h)
               + (rf - 1) * (log_bytes + shipped[small]) * t)
    print(f"\ncadence bracket, rf={rf}: segment mode at 16 MiB against document mode at one flush: "
          f"{segment:.1f} / {document:.1f} = {segment / document:.3f}x")

# D9 (DM-R17): a pre-vote persists nothing (core.h:764-767), so its round is a round trip; a vote
# round is two barriers and a round trip; the no-op is persisted before it is sent
# (core.h:1088-1090, 470-473), so a `quorum` first commit is two barriers and a round trip, and a
# group that allows memory acknowledgements sends at Written (D7), a round trip.
b, r = 80, 1
quorum_gap = election_max + r + (2 * b + r) + (2 * b + r)
memory_gap = election_max + r + (2 * b + r) + r
print(f"\nfailover gap, revision 1: quorum {quorum_gap} ms, memory {memory_gap} ms at b = {b} ms, r = {r} ms; "
      f"quiescent adds 1,500 ms; segment mode adds a replay of at most election_min (1,000 ms)")
# D9's replay is apply only (the flush runs on the pool, no-stalls item 1): 10 us per document to
# apply at one-second refreshes (no-stalls 2.1, 4.1, the writer's thread; an estimate here).
print(f"replay bound at 10 us/doc of apply: {1e6 / 10:,.0f} documents per election_min; "
      f"at T2's whole per-document cost: {1e6 / apply[one]:,.0f} (one flush) and {1e6 / apply[small]:,.0f} (16 MiB)")

# --- Revision 2 (DM-R21, DM-R22 items 4 and 5, DMS-11): the verifier in the bracket, T2's margin, and
# D18's detection figures, honest and against a leader that chooses. Arithmetic only.
print("\n# Revision 2: the cadence bracket with the verifier, T2's margin, D18's detection (estimates)")
for rf in (3, 5):
    document = encode + rf * apply[one] + rf * append + (rf - 1) * log_bytes * t
    segment = (encode + apply[small] + rf * append + shipped[small] * h + (rf - 1) * (install[small] + shipped[small] * h)
               + (rf - 1) * (log_bytes + shipped[small]) * t)
    verified = segment + (rf - 1) * apply[small] / 64
    print(f"cadence bracket with the verifier at 1/64 of the 16 MiB apply, rf={rf}: "
          f"{verified:.1f} / {document:.1f} = {verified / document:.3f}x (without it {segment / document:.3f}x)")

# T2's gate against EVP's rate (one flush, 2 ns/B, with the verifier): where each target fails.
a, i, sb = apply[one], install[one], shipped[one]
t = 2.0e-3
for sha_gbps in (0.25, 0.35, 0.5):
    h2 = 1e-3 / sha_gbps
    doc_f = a + append + log_bytes * t / 2
    seg_f = append + i + sb * h2 + (log_bytes + sb) * t / 2 + a / 64
    row = [f"SHA {sha_gbps} GB/s: follower {seg_f / doc_f:.3f}x (T2 target 0.12x)"]
    for rf, target in ((3, 0.45), (5, 0.33)):
        document = encode + rf * a + rf * append + (rf - 1) * log_bytes * t
        segment = (encode + a + rf * append + sb * h2 + (rf - 1) * (i + sb * h2) + (rf - 1) * (log_bytes + sb) * t
                   + (rf - 1) * a / 64)
        row.append(f"rf={rf} {segment / document:.3f}x (target {target}x)")
    print("T2 margin, one flush, 2 ns/B, verified: " + "; ".join(row))
lo, hi = 0.1, 2.0  # the follower's target's breakeven EVP rate, by bisection
for _ in range(60):
    mid = (lo + hi) / 2
    seg_f = append + i + sb * (1e-3 / mid) + (log_bytes + sb) * t / 2 + a / 64
    if seg_f / (a + append + log_bytes * t / 2) > 0.12:
        lo = mid
    else:
        hi = mid
print(f"the follower's 0.12x target fails below an EVP rate of {hi:.3f} GB/s (one flush, 2 ns/B, verified)")

# D18: one corrupt flush, each segment-mode follower sampling it with probability 1/64, independently.
p = 1 / 64
for rf in (3, 5):
    miss = (1 - p) ** (rf - 1)
    print(f"D18, rf={rf}: one corrupt flush detected {1 - miss:.1%}, missed {miss:.1%}; "
          + ", ".join(f"{n} corrupt flushes all missed {miss ** n:.2%}" for n in (16, 32, 64, 128)))
    # A leader that drives the debt (DMS-11): samples skipped at rate s.
    print("  skipped at rate s, one flush detected: " + ", ".join(
        f"s={s_:.1f} {1 - (1 - p * (1 - s_)) ** (rf - 1):.2%}" for s_ in (0.5, 0.9, 1.0))
          + "; deterministic sampling the leader can compute: 0%")
# T4's choosing mutant (one flush in 64 corrupt, under secret sampling): corrupt flushes a run needs
# before the chance that every one is missed falls below 10^-6.
import math
for rf in (3, 5):
    miss = (1 - p) ** (rf - 1)
    print(f"T4, rf={rf}: at least {math.ceil(math.log(1e-6) / math.log(miss))} corrupt flushes for P(all missed) < 1e-6")

# --- Revision 3 (DM-R25, DM-R27 items 6 and 8): the keyed comparison's cost, Q9's options priced, and
# the file list's limit. Counted: records per document of T2's corpus (results/r3/records.out).
# Assumptions, labeled, none measured here (T2 measures them):
#   SipHash-2-4 per pass over a document's 92 records: 2.01 us (Aumasson and Bernstein's section-6
#     lower bound on a core with three 64-bit units, at 3 GHz) to 5.27 us (their 2012 Table 6.1
#     rates, 124 cycles at 8 bytes; revision 3's 5.73 counted one block too many, DM-R32 item 4),
#     results/r4/records.out;
#   a segment walk's decode, and the log side's record building, 0.5 to 2 us per document each;
#   HMAC-SHA-256 instead: two SHA-256 compressions of 64 bytes per record through EVP, 0.5 to
#     1.5 GB/s, call overhead not counted.
print("\n# Revision 3: keyed comparisons, Q9's options, the file list's limit (estimates)")
records, rec_bytes = 9_200_623 / 100_000, 329_756_684 / 9_200_623
sip = (2.01, 5.27)
decode = (0.5, 2.0)
hmac = tuple(records * 2 * 64 * 1e-3 / g for g in (1.5, 0.5))
print(f"keyed pass per document: SipHash-2-4 {sip[0]:.2f} to {sip[1]:.2f} us; HMAC-SHA-256 {hmac[0]:.1f} to {hmac[1]:.1f} us "
      f"({hmac[0] / sip[1]:.1f}x to {hmac[1] / sip[0]:.1f}x SipHash's)")
final = {one: 43_493_712, small: 53_817_612}
generations = {c: shipped[c] * 100_000 / final[c] for c in apply}  # bytes written over final bytes
walks = {c: 1 + 2 * (generations[c] - 1) for c in apply}  # each flush output, then merge outputs and inputs
for c in apply:
    passes = 1 + walks[c]
    lo = passes * (sip[0] + decode[0])
    hi = passes * (sip[1] + decode[1])
    doc_f = apply[c] + append
    print(f"{c}: segment bytes written {generations[c]:.2f}x the final; keyed passes per document {passes:.2f} "
          f"(the log's and {walks[c]:.2f} segment walks); option 3 per follower {lo:.1f} to {hi:.1f} us/doc, "
          f"{lo / doc_f:.3f}x to {hi / doc_f:.3f}x a document-mode follower's {doc_f:.1f}")
# Group CPU per document under each of Q9's options, against document mode, at the slow and the fast
# ends of the revision-1 assumptions (SHA 0.5 GB/s with 2 ns/B transport; 1.5 GB/s with 1 ns/B).
for c in apply:
    a, i, sb = apply[c], install[c], shipped[c]
    passes = 1 + walks[c]
    for label, (sha, tns, opt3) in (("slow", (0.5, 2.0, passes * (sip[1] + decode[1]))),
                                    ("fast", (1.5, 1.0, passes * (sip[0] + decode[0])))):
        h, t = 1e-3 / sha, tns * 1e-3
        row = []
        for rf in (3, 5):
            document = encode + rf * a + rf * append + (rf - 1) * log_bytes * t
            segment = encode + a + rf * append + sb * h + (rf - 1) * (i + sb * h) + (rf - 1) * (log_bytes + sb) * t
            o2 = segment + (rf - 1) * (a / 64 + 2 * (sip[1] + decode[1]) / 64)
            o3 = segment + (rf - 1) * opt3
            o1 = (encode + 2 * a + rf * append + sb * h + (rf - 2) * (i + sb * h) + (rf - 1) * log_bytes * t
                  + (rf - 2) * sb * t)
            o4 = o1 + (rf - 2) * opt3
            row.append(f"rf={rf}: option 1 {o1 / document:.3f}x, 2 {o2 / document:.3f}x, 3 {o3 / document:.3f}x, "
                       f"1 with 3 {o4 / document:.3f}x")
        print(f"Q9, {c}, {label} end: " + "; ".join(row))
# DM-R27 item 6: what transfer_list_bytes (8 MiB) admits. A list entry is 60 bytes (identity 16, size 8,
# CRC-32C 4, SHA-256 32); a manifest segment entry (files.hh:256-264) is LEB128 fields around a
# 16-byte identity: about 49 bytes typical (identity 16, id 2, size 5, crc 5, counts 3 + 3 + 4, live
# ref 2 + 3 + 4, values 1, no attributes 1; revision 3 summed it as 46, DM-R32 item 5) and 116 at
# worst (every LEB128 at its widest).
limit = 8 << 20
for label, entry in (("typical", 49), ("widest", 116)):
    for files in (1, 2):
        per = files * 60 + entry
        print(f"transfer_list_bytes 8 MiB, {label} manifest entries, {files} file(s) per segment: "
              f"about {limit // per:,} segments ({per} bytes each)")

# --- Revision 4 (DM-R31, DM-R32 item 6): option 3 with the query-path checks, and where its cost lead
# over option 1 ends. Assumptions, labeled, none measured here (T2 measures them):
#   DM-R31's structural checks on each installed segment (the two dictionaries re-encoded from their
#     walks and compared, the key filter probed per key, norms, statistics, skip entries, and impacts
#     recomputed from the walk): 0.2 to 1.0 us per document of the installed segment, run once per
#     install (flush and merge outputs), not on merge inputs, which passed theirs.
print("\n# Revision 4: option 3 with DM-R31's checks; option 3 against option 1 as the walk varies (estimates)")
checks = (0.2, 1.0)
for c in apply:
    passes = 1 + walks[c]
    lo = passes * (sip[0] + decode[0]) + generations[c] * checks[0]
    hi = passes * (sip[1] + decode[1]) + generations[c] * checks[1]
    doc_f = apply[c] + append
    print(f"{c}: installs per document {generations[c]:.2f}; option 3 per follower {lo:.1f} to {hi:.1f} us/doc, "
          f"{lo / doc_f:.3f}x to {hi / doc_f:.3f}x a document-mode follower's {doc_f:.1f}")


def q9(c, rf, sha, tns, opt3):
    a, i, sb = apply[c], install[c], shipped[c]
    h, t = 1e-3 / sha, tns * 1e-3
    document = encode + rf * a + rf * append + (rf - 1) * log_bytes * t
    segment = encode + a + rf * append + sb * h + (rf - 1) * (i + sb * h) + (rf - 1) * (log_bytes + sb) * t
    o1 = encode + 2 * a + rf * append + sb * h + (rf - 2) * (i + sb * h) + (rf - 1) * log_bytes * t + (rf - 2) * sb * t
    o2 = segment + (rf - 1) * (a / 64 + 2 * (sip[1] + decode[1]) / 64)
    o3 = segment + (rf - 1) * opt3
    return o1 / document, o2 / document, o3 / document, (o1 + (rf - 2) * opt3) / document


for c in apply:
    passes = 1 + walks[c]
    for label, sha, tns, sv, dv, cv in (("slow", 0.5, 2.0, sip[1], decode[1], checks[1]), ("fast", 1.5, 1.0, sip[0], decode[0], checks[0])):
        opt3 = passes * (sv + dv) + generations[c] * cv
        row = []
        for rf in (3, 5):
            o1, o2, o3, o4 = q9(c, rf, sha, tns, opt3)
            row.append(f"rf={rf}: option 1 {o1:.3f}x, 2 {o2:.3f}x, 3 {o3:.3f}x, 1 with 3 {o4:.3f}x")
        print(f"Q9 r4, {c}, {label} end: " + "; ".join(row))
# Where option 3 meets option 1: the walk's cost per document and pass, decode and the checks
# together (charged on every pass, which overstates the checks), at each end's SipHash and transport.
for c in apply:
    passes = 1 + walks[c]
    for label, sha, tns, sv in (("slow", 0.5, 2.0, sip[1]), ("fast", 1.5, 1.0, sip[0])):
        row = []
        for rf in (3, 5):
            o1 = q9(c, rf, sha, tns, 0.0)[0]
            z = q9(c, rf, sha, tns, passes * sv)[2]
            one = q9(c, rf, sha, tns, passes * (sv + 1.0))[2]
            cross = (o1 - z) / (one - z)
            row.append(f"rf={rf}: equal at {cross:.2f} us per document and pass" if cross > 0
                       else f"rf={rf}: option 3 above option 1 at any walk cost (by {z - o1:.3f}x with a free walk)")
        print(f"Q9 r4 crossover, {c}, {label} end: " + "; ".join(row))
