#!/usr/bin/env python3
"""Byte model of the manifest log's tail rule (docs/design/manifest-log.md D2, D6, revision 1).

The framing is journal's turn-log framing (include/journal/turn_log.h at journal:bfad97f: Emitter
:107-160, scan_segment :175-221) at the manifest log's parameters: 512-byte blocks, no block
manifest, epoch 1, the segment number the snapshot's generation. The framing model is the
architect's (.scratch/mlogrev/tail_probe.py at strata:057c47e); the classifiers are D6's.

Checks:
  0. The model reads journal's own RFTW0001 fixtures (32 KiB blocks): headers and fragments verify.
  1. Honest crashes: every append of every test log, every subset of its 512-byte sectors
     persisted, every size a crash can leave (E, E+n, each 512-byte multiple between): never
     damage, and the generation is the last whole record's.
  2. Every single-bit flip over every byte of every test log: none reads as torn or clean with
     fewer records (silent); flips in bytes no reader reads (block padding) are counted apart.
  3. The same flips under revision 0's wording of D6 (the review's ML-R1, reproduced).
  4. Two faults: each torn image of the last append plus each single-bit flip in the last
     acknowledged record's first 256 bytes: none silent (revision 1; the review's refined rule
     for contrast).
  5. Two edits in flight (ML-R9): honest images that read as damage, so the writer keeps one.
  6. A short write left in place, no truncate-back (D4): sizes E+k for every k, every byte written.

Usage: tail_model.py [journal-fixture-dir]   (journal's test/fixtures/crc32c/turn-log)
"""
import functools
import itertools
import os
import random
import sys

_T = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
    _T.append(c)


@functools.lru_cache(maxsize=None)
def crc32c(b):
    c = 0xFFFFFFFF
    for x in b:
        c = _T[(c ^ x) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def p16(v): return v.to_bytes(2, "little")
def p32(v): return v.to_bytes(4, "little")
def p64(v): return v.to_bytes(8, "little")
def g16(b, o): return int.from_bytes(b[o:o + 2], "little")
def g32(b, o): return int.from_bytes(b[o:o + 4], "little")
def g64(b, o): return int.from_bytes(b[o:o + 8], "little")


HEADER_FIXED, FRAG, NO_MANIFEST = 58, 24, 0xFFFF
FULL, FIRST, MIDDLE, LAST = 1, 2, 3, 4
HB = HEADER_FIXED + 4  # a block header without a manifest: 62 bytes
MAGIC, IDENT, SEG, EPOCH = b"STRLOG01", bytes(range(1, 17)), 7, 1


class Emitter:
    """turn_log.h's Emitter with no block manifest: blocks entered as the stream reaches them, a
    block tail too short for a fragment padded by the record that follows."""

    def __init__(self, block):
        self.block, self.pos, self.out = block, 0, bytearray()

    def _enter(self):
        if self.pos % self.block:
            return
        h = bytearray(MAGIC + IDENT + p64(SEG) + p32(self.pos // self.block) + p64(EPOCH))
        h += p64(0) + p32(0) + p16(NO_MANIFEST)
        h += p32(crc32c(bytes(h)))
        self.out += h
        self.pos += len(h)

    def record(self, logical):
        rest, first = bytes(logical), True
        while rest:
            self._enter()
            room = self.block - self.pos % self.block
            if room < FRAG + 1:
                self.out += bytes(room)
                self.pos += room
                continue
            n = min(len(rest), room - FRAG)
            last = n == len(rest)
            t = (FULL if last else FIRST) if first else (LAST if last else MIDDLE)
            hdr = p16(n) + bytes([t, 0]) + p64(SEG) + p64(EPOCH)
            self.out += p32(crc32c(hdr + rest[:n])) + hdr + rest[:n]
            self.pos += FRAG + n
            rest, first = rest[n:], False


def header_ok(img, boff, block):
    h = bytes(img[boff:boff + HB])
    return (h[:8] == MAGIC and h[8:24] == IDENT and g64(h, 24) == SEG and g32(h, 32) == boff // block
            and g64(h, 36) == EPOCH and g16(h, 56) == NO_MANIFEST and g32(h, HB - 4) == crc32c(h[:HB - 4]))


def fragment_at(img, off, block):
    """('ok', type, payload) | ('cut', ...) | ('bad', ...): a fragment whose declared extent passes the
    end of file is cut; one that fails its structure or CRC is bad."""
    eof = len(img)
    room = block - off % block
    if off + FRAG > eof:
        return "cut", 0, b""
    fh = bytes(img[off:off + FRAG])
    length, t = g16(fh, 4), fh[6]
    if not (length > 0 and FRAG + length <= room and FULL <= t <= LAST and fh[7] == 0
            and g64(fh, 8) == SEG and g64(fh, 16) == EPOCH):
        return "bad", t, b""
    if off + FRAG + length > eof:
        return "cut", t, b""
    payload = bytes(img[off + FRAG:off + FRAG + length])
    return ("ok" if crc32c(fh[4:] + payload) == g32(fh, 0) else "bad"), t, payload


def scan(img, block):
    """Whole records in order, then the stop: (records, stop) with stop None (clean end), or
    (kind, at) where kind is 'cut' or 'bad' at offset `at`, or 'eof' with a record incomplete."""
    eof, records, assembling = len(img), [], None
    pos = 0
    while pos < eof:
        if pos % block == 0:
            if pos + HB > eof:
                return records, ("cut", pos)
            if not header_ok(img, pos, block):
                return records, ("bad", pos)
            pos += HB
        if block - pos % block < FRAG + 1:  # padding: never read
            pos += block - pos % block
            continue
        if pos >= eof:
            break
        kind, t, payload = fragment_at(img, pos, block)
        if kind != "ok":
            return records, (kind, pos)
        if t in (FULL, FIRST):
            if assembling is not None:
                return records, ("bad", pos)
            assembling = bytearray(payload)
        else:
            if assembling is None:
                return records, ("bad", pos)
            assembling += payload
        if t in (FULL, LAST):
            records.append(bytes(assembling))
            assembling = None
        pos += FRAG + len(payload)
    if assembling is not None:
        return records, ("eof", eof)
    return records, None


def whole_record_after(img, at, block):
    """Whether a whole record starts in a block after the one holding `at`, resynchronizing at
    block starts as scan_segment does after a gap."""
    eof, assembling = len(img), False
    for boff in range((at // block + 1) * block, eof, block):
        if boff + HB > eof or not header_ok(img, boff, block):
            assembling = False
            continue
        pos = boff + HB
        while block - pos % block >= FRAG + 1 and pos < eof:
            kind, t, payload = fragment_at(img, pos, block)
            if kind != "ok":
                assembling = False
                break
            if t == FULL or (t == LAST and assembling):
                return True
            assembling = t in (FIRST, MIDDLE) and (t == FIRST or assembling)
            pos += FRAG + len(payload)
    return False


def classify_r1(img, block=512):
    """D6, revision 1. At the first fragment or block header that does not verify: cut by the end
    of file is damage; all zeros from there to its block's end (an unwritten sector, since no
    fragment crosses a 512-byte boundary) is torn; anything else is damage. A record whose
    fragments all verify but which has no last one is torn when the file ends at a 512-byte
    multiple. A torn verdict with a whole record after it is damage; so is any problem in the
    snapshot (the first record), which was synced before its rename."""
    records, stop = scan(img, block)
    if stop is None:
        return "clean", len(records)
    kind, at = stop
    if not records:
        return "damage", 0
    if kind == "eof":
        torn = len(img) % 512 == 0
    elif kind == "cut":
        torn = False
    else:
        end = min((at // block + 1) * block, len(img))
        torn = not any(img[at:end])
    if torn and whole_record_after(img, at, block):
        torn = False
    return ("torn" if torn else "damage"), len(records)


def record_start(img, at, block):
    """The first fragment of the record the scan stopped in (for revision 0's piece rule)."""
    # Walk again, remembering where the current record began.
    eof, pos, start, assembling = len(img), 0, None, False
    while pos < eof and pos < at:
        if pos % block == 0:
            pos += HB
        if block - pos % block < FRAG + 1:
            pos += block - pos % block
            continue
        kind, t, payload = fragment_at(img, pos, block)
        if kind != "ok":
            break
        if t in (FULL, FIRST):
            start, assembling = pos, t == FIRST
        elif t == LAST:
            assembling = False
        pos += FRAG + len(payload)
    return start if assembling else at


def classify_r0(img, block=512, refined=False):
    """D6 as revision 0 worded it: torn when the record runs past the end of file or one of its
    512-byte pieces (from its first fragment, past padding) reads as all zeros, scanning to the
    end of file; `refined` is the review's ML-R1 rule (a cut is torn only at a 512-byte multiple)."""
    records, stop = scan(img, block)
    if stop is None:
        return "clean", len(records)
    kind, at = stop
    if not records:
        return "damage", 0
    start = record_start(img, at, block)
    if whole_record_after(img, start, block):
        return "damage", len(records)
    eof, zero, a = len(img), False, start
    while a < eof:
        b = min((a // 512 + 1) * 512, eof)
        room = block - a % block
        if not any(img[a:b]) and not (room < FRAG + 1 and b - a == room):
            zero = True
            break
        a = b
    past = kind in ("cut", "eof")
    if refined:
        past = past and eof % 512 == 0
    return ("torn" if zero or past else "damage"), len(records)


# --- the logs under test --------------------------------------------------------------------
def snapshot(rnd, gen):
    return bytes([1]) + bytes([gen]) + bytes(rnd.randrange(256) for _ in range(300)) + bytes(600) + bytes(
        rnd.randrange(256) for _ in range(200))


def edit(rnd, gen, size, zero_run=False):
    body = bytearray(rnd.randrange(256) for _ in range(size))
    if zero_run and size > 200:
        body[100:size - 50] = bytes(size - 150)  # a deleted-set bitset with a long zero run
    return bytes([2]) + gen.to_bytes(3, "little") + bytes(body)


def build(sizes, seed, block=512):
    """A log: a snapshot, then one edit per size. A size of ('end', r) makes the edit leave exactly
    r bytes of room in its block (0: it ends on the block boundary; 1..24: the next record pads)."""
    rnd, em, gen = random.Random(seed), Emitter(block), SEG
    em.record(snapshot(rnd, gen))
    ends = [len(em.out)]
    for s in sizes:
        gen += 1
        if isinstance(s, tuple):
            em._enter()
            room = block - em.pos % block
            if room < FRAG + 1:
                em.out += bytes(room)
                em.pos += room
                em._enter()
                room = block - em.pos % block
            s = max(1, room - FRAG - s[1] - 4)  # 4: the kind and generation bytes in edit()
        em.record(edit(rnd, gen, s, zero_run=s > 200))
        ends.append(len(em.out))
    return bytes(em.out), ends


LOGS = {
    "A (the review's)": ([70, 95, 130, 60, 110, 88, 700, 75, 120], 7),
    "B (block ends, padding)": ([("end", 0), 60, ("end", 10), 40, ("end", 24), ("end", 1), 90, ("end", 0), 30, 500,
                                 ("end", 3)], 3),
    "C (multi-block edits)": ([1000, 64, 1500, 80, 900, 2100, 45], 5),
    "D (random 20-400)": (None, 11),
}


def logs(block=512):
    out = {}
    for name, (sizes, seed) in LOGS.items():
        if sizes is None:
            r = random.Random(seed)
            sizes = [r.randrange(20, 400) for _ in range(30)]
        out[name] = build(sizes, seed, block)
    return out


def honest_images(prev, new):
    """Images a crash can leave for one append from E=len(prev) to E+n=len(new), written in one
    write: each 512-byte sector of the append persisted or not, the size at E, E+n or a 512-byte
    multiple between (a page boundary; every page size is a multiple of 512)."""
    E, N = len(prev), len(new)
    secs = list(range(E // 512, (N - 1) // 512 + 1))
    sizes = sorted({E, N} | set(range((E // 512 + 1) * 512, N, 512)))
    base = bytearray(prev) + bytes(N - E)
    for mask in itertools.product((0, 1), repeat=len(secs)):
        img = bytearray(base)
        for keep, s in zip(mask, secs):
            if keep:
                img[s * 512:min((s + 1) * 512, N)] = new[s * 512:min((s + 1) * 512, N)]
        for size in sizes:
            yield bytes(img[:size]), size == N and bytes(img) == new  # a sector rewritten with the
            # bytes it held (block padding) leaves the append whole


def padding_bytes(log, block=512):
    """Offsets no reader reads: block tails too short for a fragment."""
    pad, pos = set(), 0
    eof = len(log)
    while pos < eof:
        if pos % block == 0:
            pos += HB
        room = block - pos % block
        if room < FRAG + 1:
            pad.update(range(pos, min(pos + room, eof)))
            pos += room
            continue
        if pos >= eof:
            break
        length = g16(log, pos + 4)
        pos += FRAG + length
    return pad


def main():
    fixture_dir = sys.argv[1] if len(sys.argv) > 1 else None
    if fixture_dir:
        n_hdr = n_frag = 0
        for name in sorted(os.listdir(fixture_dir)):
            data = open(os.path.join(fixture_dir, name), "rb").read()
            seg = int(name[6:], 16)
            for boff in range(0, len(data), 32 << 10):
                h = data[boff:boff + HEADER_FIXED]
                if h[:8] != b"RFTW0001":
                    continue
                count = g16(h, 56)
                hb = HEADER_FIXED + (0 if count == NO_MANIFEST else 16 * count) + 4
                if g32(data, boff + hb - 4) != crc32c(data[boff:boff + hb - 4]) or g64(h, 24) != seg:
                    continue
                n_hdr += 1
                epoch, pos = g64(h, 36), hb
                while (32 << 10) - pos >= FRAG + 1:
                    fh = data[boff + pos:boff + pos + FRAG]
                    length = g16(fh, 4)
                    if not length or not 1 <= fh[6] <= 4:
                        break
                    pl = data[boff + pos + FRAG:boff + pos + FRAG + length]
                    if crc32c(fh[4:] + pl) != g32(fh, 0) or g64(fh, 8) != seg or g64(fh, 16) != epoch:
                        break
                    n_frag += 1
                    pos += FRAG + length
        print(f"0. journal's RFTW0001 fixtures: {n_hdr} block headers and {n_frag} fragments verify under the model")

    built = logs()
    for name, (log, ends) in built.items():
        assert classify_r1(log) == ("clean", len(ends)), name
        print(f"log {name}: {len(ends)} records (a snapshot, {len(ends) - 1} edits), {len(log)} bytes, "
              f"{(len(log) + 511) // 512} blocks")

    # 1. Honest crashes of every append.
    total = bad = torn = 0
    for name, (log, ends) in built.items():
        for k in range(1, len(ends)):
            for img, whole in honest_images(log[:ends[k - 1]], log[:ends[k]]):
                total += 1
                v, n = classify_r1(img)
                want = k + 1 if whole else k
                torn += v == "torn"
                if v == "damage" or n != want:
                    bad += 1
    print(f"1. honest crash images, every append of every log: {total} images, {torn} read as torn, "
          f"{bad} misread (damage, or a generation other than the last whole one)")

    # 2. and 3. Every single-bit flip in every complete log.
    for label, fn in (("2. revision 1", classify_r1), ("3. revision 0 (as worded)", classify_r0)):
        flips = silent = damage = unread = 0
        by_where = {}
        for name, (log, ends) in built.items():
            pad = padding_bytes(log)
            for off in range(len(log)):
                for bit in range(8):
                    img = bytearray(log)
                    img[off] ^= 1 << bit
                    v, n = fn(bytes(img))
                    flips += 1
                    if v == "damage":
                        damage += 1
                    elif n == len(ends) and off in pad:
                        unread += 1
                    else:
                        silent += 1
                        by_where[name] = by_where.get(name, 0) + 1
        print(f"{label}: {flips} single-bit flips over every byte of the 4 logs: {damage} damage, {silent} SILENT "
              f"(an acknowledged edit dropped), {unread} in padding no reader reads; silent by log: {by_where}")

    # 4. Two faults: a torn last append plus a flip in the last acknowledged record.
    for label, fn in (("revision 1", classify_r1), ("the review's refined rule", lambda i: classify_r0(i, refined=True))):
        cases = silent = 0
        for name, (log, ends) in built.items():
            prev, new = log[:ends[-2]], log
            lo = ends[-3]  # the last acknowledged record's first 256 bytes: its headers and early payload
            hi = min(ends[-2], lo + 256)
            seen = set()
            for img, whole in honest_images(prev, new):
                if whole or img in seen:
                    continue
                seen.add(img)
                if classify_r1(img)[0] != "torn":
                    continue
                for off in range(lo, hi):
                    for bit in range(8):
                        b = bytearray(img)
                        b[off] ^= 1 << bit
                        v, n = fn(bytes(b))
                        cases += 1
                        if v != "damage" and n < len(ends) - 1:
                            silent += 1
        print(f"4. two faults, {label}: {cases} images (a torn last append, a flip in the last acknowledged record): "
              f"{silent} SILENT")

    # 5. Two edits in flight.
    damage = total = 0
    for name, (log, ends) in built.items():
        E0, E2 = ends[-3], ends[-1]
        secs = list(range(E0 // 512, (E2 - 1) // 512 + 1))
        if len(secs) > 10:
            continue
        for mask in itertools.product((0, 1), repeat=len(secs)):
            img = bytearray(log[:E0]) + bytes(E2 - E0)
            for keep, s in zip(mask, secs):
                if keep:
                    img[s * 512:min((s + 1) * 512, E2)] = log[s * 512:min((s + 1) * 512, E2)]
            total += 1
            damage += classify_r1(bytes(img))[0] == "damage"
    print(f"5. two edits in flight, revision 1: {damage} of {total} honest images read as damage "
          f"(so the writer keeps one record in flight)")

    # 6. A short write left in place.
    sizes = closed = torn_or_clean = 0
    for name, (log, ends) in built.items():
        for k in range(1, len(ends)):
            E, N = ends[k - 1], ends[k]
            for size in range(E + 1, N):
                sizes += 1
                v, n = classify_r1(log[:size])
                if v == "damage":
                    closed += 1
                else:
                    torn_or_clean += 1
    print(f"6. a short write not truncated back: {sizes} sizes E+k, {closed} fail closed (damage), "
          f"{torn_or_clean} open at the previous generation; after the truncate-back every one opens clean")


if __name__ == "__main__":
    main()
