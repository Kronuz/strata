#!/usr/bin/env python3
"""durability-modes D18 (DM-R25): how many F21 digest records a document of T2's corpus makes, and
their bytes, so the keyed comparison's cost can be estimated. A count, not a timing.

The documents are sizing.cc's (bench/durability/sizing.cc: fill() and read_corpus()): key
"Q" + id, the key as a term with wdf 0 and no positions, the text split into words at spaces
(strata's text_detail::split_words, reader.hh:115-133: a word is a maximal run of bytes other than
' '), and the id stored. Their records are digest()'s (index.hh:644-700, reader.hh:74-87,
625-641, 897-912): per live document one D (tag, docid u64, version u64, key as u32 length and
bytes), one N (tag, docid u64, length u32), one P per distinct term (tag, docid u64, term as u32
length and bytes, wdf u32, position count u32, each position u32), and one R for a non-empty
stored value (tag, docid u64, bytes as u32 length and bytes).

Usage: python3 -I records.py CORPUS [N]   (T2's corpus: the 1M corpus's first 100,000 lines)
"""
import sys
from collections import Counter


def field(line, name):
    key = '"' + name + '": "'
    at = line.find(key)
    if at < 0:
        return ""
    at += len(key)
    return line[at:line.find('"', at)]  # as read_corpus() takes it: up to the next quote


def siphash_cycles(size):
    """SipHash-2-4's cycles for one input of `size` bytes, two ways, both estimates (not measured
    here): `paper`, Aumasson and Bernstein (2012), Table 6.1, "bulldozer" (AMD FX-8150): 124 cycles
    at 8 bytes to 234 at 64, about 15.7 more per 8-byte block, and past 64 bytes its long-input
    1.96 cycles per byte (section 6); `bound`, section 6's lower-bound arithmetic for a core with
    three 64-bit units: 14 operations per SipRound, two rounds per 8-byte block (the block with the
    length byte included) and four to finalize, which gives 47 cycles for 16 to 23 bytes where the
    paper's own figure is "at least 49" (more units do not help much: with four, it says, "at least
    1 cycle per byte, since SipRound does not always have 4 operations to perform in parallel")."""
    blocks = size // 8 + 1  # the 8-byte blocks hashed, the one with the length byte included
    # 124 cycles at 8 bytes, which is two blocks; 15.7 per further block to 64 bytes (nine blocks).
    paper = 124 + 15.7 * (min(blocks, 9) - 2) + 1.96 * max(size - 64, 0)
    bound = (2 * blocks + 4) * 14 / 3
    return paper, bound


def main():
    path, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 100_000
    docs = records = record_bytes = 0
    paper = bound = 0.0
    sizes = Counter()
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "surrogateescape").rstrip("\n")
            if not line:
                continue
            ident = field(line, "id").encode("utf-8", "surrogateescape")
            text = field(line, "text").encode("utf-8", "surrogateescape")
            key = b"Q" + ident
            words = [w for w in text.split(b" ") if w]
            terms = Counter(words)
            recs = [1 + 8 + 8 + 4 + len(key), 1 + 8 + 4]          # D, N
            recs.append(1 + 8 + 4 + len(key) + 4 + 4)              # the key's P (wdf 0, no positions)
            for term, wdf in terms.items():
                recs.append(1 + 8 + 4 + len(term) + 4 + 4 + 4 * wdf)
            if ident:
                recs.append(1 + 8 + 4 + len(ident))                # R
            docs += 1
            records += len(recs)
            record_bytes += sum(recs)
            for r in recs:
                sizes[min(r // 16 * 16, 128)] += 1
                p_, b_ = siphash_cycles(r)
                paper += p_
                bound += b_
            if docs == n:
                break
    print(f"{docs:,} documents: {records:,} records ({records / docs:.1f} per document), "
          f"{record_bytes:,} bytes ({record_bytes / docs:.0f} per document, {record_bytes / records:.1f} per record)")
    print("record sizes, bytes rounded down to 16 (128: 128 or more): " +
          ", ".join(f"{k}: {100 * v / records:.1f}%" for k, v in sorted(sizes.items())))
    print(f"SipHash-2-4 per document, estimates: {paper / docs:,.0f} cycles at the paper's 2012 rates, "
          f"{bound / docs:,.0f} at the three-unit bound; at 3 GHz {paper / docs / 3000:.2f} and {bound / docs / 3000:.2f} us "
          f"per pass over a document's records")


if __name__ == "__main__":
    main()
