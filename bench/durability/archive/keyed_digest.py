#!/usr/bin/env python3
"""durability-modes D18 (DM-R25): the verifier's keyed comparison against the architect's forgery.

F21's digest sums xxHash64 over records modulo 2^64, unkeyed (index.hh:644-700), so a leader that
writes a segment's bytes can make a planted segment's digest equal the honest one in O(1) (the
architect's r2/digest/digest_forge.py, whose records this reads). D18 compares instead a keyed sum:
SipHash-2-4 (Aumasson and Bernstein, 2012) of every record under a 128-bit key the follower draws
from the operating system's generator at process start and never sends or stores, summed modulo
the prime p = 2^64 - 59, so a record repeated d < p times shifts the sum by d times a uniform value
and no multiplicity cancels it. This probe checks the SipHash code against the reference vectors,
then sums the honest, forged, and printable-forged segments' records three ways.

Usage: python3 -I keyed_digest.py RECORDS   (lines "label hex", as digest_forge.py writes them)
"""
import os
import struct
import sys

M64 = (1 << 64) - 1
P = (1 << 64) - 59  # the largest prime below 2^64


def rotl(x, b):
    return ((x << b) | (x >> (64 - b))) & M64


def siphash24(key, msg):
    """SipHash-2-4, 64-bit output, as the paper's section 2 defines it."""
    k0, k1 = struct.unpack("<QQ", key)
    v0, v1, v2, v3 = k0 ^ 0x736F6D6570736575, k1 ^ 0x646F72616E646F6D, k0 ^ 0x6C7967656E657261, k1 ^ 0x7465646279746573

    def rounds(n):
        nonlocal v0, v1, v2, v3
        for _ in range(n):
            v0 = (v0 + v1) & M64; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32)
            v2 = (v2 + v3) & M64; v3 = rotl(v3, 16); v3 ^= v2
            v0 = (v0 + v3) & M64; v3 = rotl(v3, 21); v3 ^= v0
            v2 = (v2 + v1) & M64; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32)

    n = len(msg)
    tail = msg[n - n % 8:] + bytes(7 - n % 8) + bytes([n & 0xFF])
    blocks = msg[:n - n % 8] + tail
    for i in range(0, len(blocks), 8):
        m = struct.unpack_from("<Q", blocks, i)[0]
        v3 ^= m
        rounds(2)
        v0 ^= m
    v2 ^= 0xFF
    rounds(4)
    return v0 ^ v1 ^ v2 ^ v3


def xxh64(data):
    """xxHash64, seed 0 (F21's hash), for the unkeyed sums."""
    P1, P2, P3, P4, P5 = (11400714785074694791, 14029467366897019727, 1609587929392839161,
                          9650029242287828579, 2870177450012600261)

    def rnd(acc, x):
        return (rotl((acc + x * P2) & M64, 31) * P1) & M64

    n, p = len(data), 0
    if n >= 32:
        v = [(P1 + P2) & M64, P2, 0, (-P1) & M64]
        while n - p >= 32:
            v = [rnd(v[i], struct.unpack_from("<Q", data, p + 8 * i)[0]) for i in range(4)]
            p += 32
        h = (rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18)) & M64
        for x in v:
            h = (((h ^ rnd(0, x)) * P1) + P4) & M64
    else:
        h = P5
    h = (h + n) & M64
    while n - p >= 8:
        h = (rotl(h ^ rnd(0, struct.unpack_from("<Q", data, p)[0]), 27) * P1 + P4) & M64
        p += 8
    if n - p >= 4:
        h = (rotl(h ^ ((struct.unpack_from("<I", data, p)[0] * P1) & M64), 23) * P2 + P3) & M64
        p += 4
    while p < n:
        h = (rotl(h ^ ((data[p] * P5) & M64), 11) * P1) & M64
        p += 1
    h = ((h ^ (h >> 33)) * P2) & M64
    h = ((h ^ (h >> 29)) * P3) & M64
    return h ^ (h >> 32)


def main():
    # The reference vectors (the paper's appendix A; the reference implementation's vectors.h):
    # key 00..0f; the empty message, and the 15-byte message 00..0e.
    key = bytes(range(16))
    assert siphash24(key, b"") == 0x726FDB47DD0E0E31, hex(siphash24(key, b""))
    assert siphash24(key, bytes(range(15))) == 0xA129CA6149BE45E5, hex(siphash24(key, bytes(range(15))))
    assert xxh64(b"") == 0xEF46DB3751D8E999
    print("SipHash-2-4 matches the reference vectors (key 00..0f: the empty message, and 00..0e)")
    segs = {}
    for line in open(sys.argv[1]):
        label, hexrec = line.split()
        segs.setdefault(label, []).append(bytes.fromhex(hexrec))
    secret = os.urandom(16)  # a follower's key: drawn at start, never sent, never stored
    print(f"{'segment':<10} {'records':>7}  {'F21 (xxh64, mod 2^64)':>22}  {'keyed (SipHash-2-4, mod p)':>26}")
    sums = {}
    for label, recs in segs.items():
        plain = sum(xxh64(r) for r in recs) & M64
        keyed = sum(siphash24(secret, r) for r in recs) % P
        sums[label] = (plain, keyed)
        print(f"{label:<10} {len(recs):>7}  {plain:>22x}  {keyed:>26x}")
    h = sums["honest"]
    for label in ("forged", "printable"):
        print(f"{label}: F21 equal to the honest digest: {sums[label][0] == h[0]}; keyed equal: {sums[label][1] == h[1]}")


if __name__ == "__main__":
    main()
