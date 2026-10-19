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
                   f"({seg_f / doc_f:.3f}x)"]
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
