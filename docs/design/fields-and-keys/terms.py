#!/usr/bin/env python3
"""Emits one corpus's terms under three layouts for dictprobe.cc (fields-and-keys, T1).

Usage: terms.py {wiki|bank} CORPUS OUTDIR [LIMIT]
       terms.py sparse - OUTDIR   (1,000 docs, each 20 of 2,000 keyword fields, seed 7)

Layouts (each a file of length-prefixed keys, sorted, unique):
  flat.keys   Xapiand's concatenation: field path prefix ("a.b.") + type char + value
  fid.keys    one dictionary: the field id as an order-preserving varint + value
  field.keys  per-field dictionaries: [field id u32 BE] + value, split by dictprobe
The value bytes are the same in all three; only how the field is named differs.
"""

import json
import re
import struct
import sys

WORD = re.compile(r"[a-z0-9]+")
ACC = [
    100,
    1000,
    10000,
    100000,
    1000000,
    100000000,
]  # six accuracy levels, as the census found


def num(v):  # an order-preserving 8-byte form of a float (the f64 map of phase-4 D3a)
    b = struct.unpack(">Q", struct.pack(">d", float(v)))[0]
    b = b ^ 0xFFFFFFFFFFFFFFFF if b >> 63 else b | (1 << 63)
    return struct.pack(">Q", b)


def emit(out, path, v):
    """Appends (field name, type char, value bytes) for one value, Xapiand-style typing."""
    if isinstance(v, bool):
        out.append((path, "B", b"t" if v else b"f"))
    elif isinstance(v, (int, float)):
        out.append((path, "N", num(v)))
        for a in ACC:  # accuracy terms: one field per (path, level)
            out.append((f"{path}.<{a:x}>", "N", num(int(v) - int(v) % a)))
    elif isinstance(v, str):
        if " " in v.strip():
            for w in WORD.findall(v.lower()):
                out.append((path, "S", w.encode()))
        else:
            out.append((path, "K", v.encode()))
    elif v is None:
        out.append((path, "", b""))  # the field's exists term


def walk(out, path, v):
    if isinstance(v, dict):
        if "_value" in v:
            return emit(out, path, v["_value"])
        for k, x in v.items():
            if not k.startswith("_"):
                walk(out, f"{path}.{k}" if path else k, x)
    elif isinstance(v, list):
        for x in v:
            walk(out, path, x)
    else:
        emit(out, path, v)


def varint(
    n,
):  # order-preserving, prefix-free: 1 byte below 0xF0, then 0xF0+len and big-endian
    if n < 0xF0:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0xF0 + len(b)]) + b


def main():
    kind, corpus, outdir = sys.argv[1:4]
    limit = int(sys.argv[4]) if len(sys.argv) > 4 else None
    if kind == "sparse":
        import random

        random.seed(7)
        vocab = [
            "red",
            "green",
            "blue",
            "alpha",
            "beta",
            "gamma",
            "delta",
            "on",
            "off",
            "yes",
            "no",
        ]
        lines = [
            json.dumps(
                {
                    f"attr_{random.randrange(2000):04d}": random.choice(vocab)
                    for _ in range(20)
                }
            )
            for _ in range(1000)
        ]
    else:
        with open(corpus) as f:
            lines = (
                f.readlines()
                if limit is None
                else [line for _, line in zip(range(limit), f)]
            )
    terms = []
    for line in lines:
        d = json.loads(line)
        if kind == "wiki":  # `id` is the document key, not a term
            d = {"text": d["text"], "sort_field": d["sort_field"]}
        walk(terms, "", d)
    uniq = sorted(set(terms))
    ids = {}
    for name, _, _ in uniq:
        ids.setdefault(name, len(ids))
    flat = sorted({name.encode() + b"." + t.encode() + v for name, t, v in uniq})
    fid = sorted({varint(ids[name]) + v for name, _, v in uniq})
    fld = sorted({struct.pack(">I", ids[name]) + v for name, _, v in uniq})
    for fname, keys in (("flat", flat), ("fid", fid), ("field", fld)):
        with open(f"{outdir}/{fname}.keys", "wb") as o:
            o.writelines(struct.pack("<I", len(k)) + k for k in keys)
    print(
        f"{kind}: {len(uniq)} terms, {len(ids)} fields, flat {len(flat)} fid {len(fid)} field {len(fld)}"
    )


main()
