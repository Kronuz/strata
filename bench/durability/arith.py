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
