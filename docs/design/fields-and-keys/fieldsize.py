#!/usr/bin/env python3
"""Sizes per-field terms on Xapiand's real terms (fields-and-keys revision 1: T1, FK-R2, FK-R3).

  fieldsize.py split NAME.terms OUTDIR [SHARD]  key files for fieldprobe and OUTDIR/stats.json
  fieldsize.py report OUTDIR PROBE_OUTPUT       the T1 table: directory, norms, layouts

`split` reads xapiand_terms.py's dump and names each term's engine field as D12 does: the Xapian
prefix up to and including the type character (a leading `Z` keeps the stem field apart). `Q`
(the key, which goes to `keys`) and `V` (the Xapian engine's version term; strata keeps versions
in `versions`) are not inverted terms on strata and are left out of every layout. Field ids are
assigned in order of first use, as declarations in document order would. With SHARD, only that
shard's documents (one segment of a 5-shard index).
"""

import json
import struct
import sys
from collections import defaultdict
from pathlib import Path


def field_of(t):
    i = 1 if t[:1] == b"Z" else 0
    while i < len(t):
        if 65 <= t[i] <= 90:
            return t[: i + 1], t[i + 1 :]
        i = t.index(b".", i) + 1
    raise ValueError(t)


def leb(n):
    b = 1
    while n >= 128:
        n >>= 7
        b += 1
    return b


def split(path, out, shard=None):
    ids, docs = {}, {}
    per_doc = defaultdict(lambda: defaultdict(int))  # (doc, field) -> length
    terms = defaultdict(lambda: defaultdict(int))  # field -> value -> df
    flat = set()
    skipped = 0
    for line in Path(path).read_text().splitlines():
        doc, wdf, h = line.split("\t")
        if shard is not None and not doc.startswith(f"{shard}:"):
            continue
        t = bytes.fromhex(h)
        name, value = field_of(t)
        if name in (b"Q", b"V"):
            skipped += 1
            continue
        d = docs.setdefault(doc, len(docs))
        f = ids.setdefault(name, len(ids))
        per_doc[f][d] += int(wdf)
        terms[f][value] += 1
        flat.add(t)
    with open(f"{out}/flat.keys", "wb") as o:
        o.writelines(struct.pack("<I", len(k)) + k for k in sorted(flat))
    with open(f"{out}/field.keys", "wb") as o:
        for f in sorted(terms):
            o.writelines(
                struct.pack("<I", 4 + len(v)) + struct.pack(">I", f) + v
                for v in sorted(terms[f])
            )
    stats = {"docs": len(docs), "skipped_q_v": skipped, "fields": []}
    for name, f in ids.items():
        lens = per_doc[f]
        ords = sorted(lens)
        sparse_norms = (
            leb(len(ords))
            + sum(leb(b - a) for a, b in zip([-1] + ords, ords))
            + len(ords)
        )
        stats["fields"].append(
            {
                "id": f,
                "name": name.decode("latin1"),
                "type": chr(name[-1]),
                "stem": name[:1] == b"Z",
                "terms": len(terms[f]),
                "docs": len(lens),
                "sumdf": sum(terms[f].values()),
                "ttf": sum(lens.values()),
                "min": min(lens.values()),
                "max": max(lens.values()),
                "sparse_norms": sparse_norms,
            }
        )
    Path(f"{out}/stats.json").write_text(json.dumps(stats))
    print(
        f"{path} shard {shard}: docs {len(docs)} fields {len(ids)} flat terms {len(flat)} (Q and V terms left out: {skipped})"
    )


def report(out, probe):
    st = json.loads(Path(f"{out}/stats.json").read_text())
    N, fields = st["docs"], {f["id"]: f for f in st["fields"]}
    flat, pools, rows = None, {}, {}
    for line in Path(probe).read_text().splitlines():
        w = line.split()
        if w[0] == "flat":
            flat = int(w[1]) + int(w[2])
        elif w[0] == "field":
            rows[int(w[1])] = dict(
                zip(
                    ("terms", "own", "own_n", "own_idx", "full", "full_n"),
                    map(int, w[2:]),
                )
            )
        elif w[0] == "pool":
            pools[int(w[1])] = dict(
                zip(("fields", "terms", "blocks", "index"), map(int, w[2:]))
            )

    def directory(layout, normed, pool_t=0):
        """LEB entries under a sampled fixed-width index (one 20 B sample, id u32 and the entry,
        blocks and index offsets, per 16 entries), so a lookup decodes at most 16 entries. An
        entry exists only for a field present in the segment that has blocks of its own or norms;
        statistics only with norms, where a scorer reads them (D4)."""
        size, prev, n = leb(len(rows)) + 1, -1, 0
        for f in sorted(rows):
            s, r = fields[f], rows[f]
            pooled = bool(pool_t) and r["terms"] < pool_t
            if pooled and not normed(s):
                continue
            n += 1
            size += leb(f - prev) + 1
            prev = f
            if normed(s):
                size += (
                    leb(s["docs"])
                    + leb(s["sumdf"])
                    + leb(s["ttf"])
                    + leb(s["min"])
                    + leb(s["max"])
                )
            if pooled:
                continue
            if layout == "field1" and r["full_n"] == 1:
                size += leb(r["full"])
            else:
                size += leb(r["own"]) + leb(r["own_idx"])
        return size + 20 * ((n + 15) // 16)

    def dicts(layout, pool_t=0):
        total = 0
        for r in rows.values():
            if pool_t and r["terms"] < pool_t:
                continue
            total += (
                r["full"]
                if layout == "field1" and r["full_n"] == 1
                else r["own"] + r["own_idx"]
            )
        if pool_t:
            total += pools[pool_t]["blocks"] + pools[pool_t]["index"]
        return total

    def norms(normed):
        return sum(min(N, s["sparse_norms"]) for s in fields.values() if normed(s))

    xapian = lambda s: False  # bm25_xapian reads `lengths`, no norms
    lucene = lambda s: s["type"] == "S" and not s["stem"]  # one norm per text field
    one = sum(1 for r in rows.values() if r["full_n"] == 1)
    print(
        f"{out}: docs {N} fields {len(rows)} one-block {one} text {sum(1 for s in fields.values() if lucene(s))}"
    )
    print(f"  flat dictionary {flat}; flat norms (one document byte) {N}")
    print(
        f"  {'layout':<12}{'dicts':>9}{'dir':>8}{'B/field':>8}{'xapian':>9}{'ratio':>7}{'+norms':>9}{'lucene':>9}{'ratio':>7}"
    )
    lay = [("field", "field", 0), ("field1", "field1", 0)] + [
        (f"pool<{t}", "field", t) for t in sorted(pools)
    ]
    for label, layout, t in lay:
        d = dicts(layout, t)
        dx, dl = directory(layout, xapian, t), directory(layout, lucene, t)
        nl = norms(lucene)
        print(
            f"  {label:<12}{d:>9}{dx:>8}{dx / len(rows):>8.1f}{d + dx:>9}{(d + dx) / flat:>7.3f}"
            f"{nl:>9}{d + dl + nl:>9}{(d + dl + nl) / (flat + N):>7.3f}"
        )
    print(
        f"  dense lucene norms would be {N * sum(1 for s in fields.values() if lucene(s))}; sparse/dense chosen per field: {norms(lucene)}"
    )


if __name__ == "__main__":
    if sys.argv[1] == "split":
        split(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
    else:
        report(sys.argv[2], sys.argv[3])
