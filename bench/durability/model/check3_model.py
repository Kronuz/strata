#!/usr/bin/env python3
"""durability-modes D18 check 3 over a merge output's every ordinal (revision 5; DM-R33).

A merge carries the documents deleted while it ran into its output as dead ordinals
(`install_merge`, index.hh:1386-1414), and ranking reads every ordinal: the snapshot's document
count and total length and each term's document frequency count dead ones (searcher.hh:92-121),
and BM25's IDF and average length come from them (search.hh:76-82). Revision 4's check 3 compared
only the output's live records with the inputs' live ones, so a planted dead ordinal passed
(the architect's dead_stats_plant.cc).

This model enumerates every merge output of up to MAXOUT ordinals drawn from the inputs' documents
as they are, the inputs' documents with altered content, and phantom documents, each live or dead,
over every input of up to MAXIN documents, each live or dead at the `Image`'s position, and runs:
  r4  revision 4's check 3 (the live records equal the inputs' live records under the bitmap) with
      check 2 (an output ordinal naming an input document is dead exactly when that document is);
  r5  revision 5's: every output ordinal names an input document by (docid, version), at most once;
      it is dead exactly when that document is dead at the `Image`'s position (check 2); and the
      keyed sum over every ordinal's records equals the inputs' records of the documents named.
The keyed sums are modelled as multiset equality of records, which they give but with
probability about 2^-64. For each output a check passes, the model asks:
  content  every ordinal's records are those of an input document (nothing the log never held);
  live     the live documents are exactly the inputs' live documents;
  stats    document count, total length, and every term's df are at most the inputs' (dead included).
It also confirms that r5 passes every honest output: the live documents and any subset of the dead.

Usage: python3 -I check3_model.py [--maxin 2] [--maxout 3]
"""

from __future__ import annotations

import argparse
import itertools
import sys
import time
from typing import NamedTuple

TERMS = ("alpha", "beta")
CONTENTS = tuple(
    (frozenset(ts), n)
    for k in (1, 2)
    for ts in itertools.combinations(TERMS, k)
    for n in (1, 2)
    if len(ts) <= n
)


class Doc(NamedTuple):
    docid: int
    version: int
    content: tuple[
        frozenset[str], int
    ]  # (terms, length): the records F21 builds for it


class Ord(NamedTuple):
    doc: Doc
    dead: bool


def stats(docs: list[Doc]) -> tuple[int, int, tuple[int, ...]]:
    return (
        len(docs),
        sum(d.content[1] for d in docs),
        tuple(sum(t in d.content[0] for d in docs) for t in TERMS),
    )


def check_r4(inputs: dict[Doc, bool], out: tuple[Ord, ...]) -> bool:
    by_id = {d.docid: (d, dead) for d, dead in inputs.items()}
    for o in out:
        if o.doc.docid in by_id and o.dead != by_id[o.doc.docid][1]:
            return (
                False  # check 2: a document's ordinal dead exactly when the document is
            )
    live_out = sorted(o.doc for o in out if not o.dead)
    live_in = sorted(d for d, dead in inputs.items() if not dead)
    return live_out == live_in


def check_r5(inputs: dict[Doc, bool], out: tuple[Ord, ...]) -> bool:
    by_key = {(d.docid, d.version): (d, dead) for d, dead in inputs.items()}
    named = []
    for o in out:
        hit = by_key.get((o.doc.docid, o.doc.version))
        if hit is None or o.dead != hit[1]:
            return False  # names no input document, or check 2's bit disagrees
        named.append(hit[0])
    if len({(d.docid, d.version) for d in named}) != len(named):
        return False  # a document at most once
    if any(not dead and d not in named for d, dead in inputs.items()):
        return False  # every live document carried
    return sorted(o.doc for o in out) == sorted(
        named
    )  # the keyed sum over every ordinal


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--maxin", type=int, default=2)
    ap.add_argument("--maxout", type=int, default=3)
    args = ap.parse_args()
    start = time.time()
    totals = {"inputs": 0, "outputs": 0}
    bad = {
        "r4": {"content": 0, "live": 0, "stats": 0},
        "r5": {"content": 0, "live": 0, "stats": 0},
    }
    passed = {"r4": 0, "r5": 0}
    honest_refused = 0
    example = None
    for n in range(1, args.maxin + 1):
        for contents in itertools.product(CONTENTS, repeat=n):
            for deads in itertools.product((False, True), repeat=n):
                inputs = {
                    Doc(i + 1, 1, c): dead
                    for i, (c, dead) in enumerate(zip(contents, deads))
                }
                totals["inputs"] += 1
                pool: list[Doc] = list(inputs)
                for d in inputs:
                    pool += [
                        d._replace(content=c) for c in CONTENTS if c != d.content
                    ]  # altered
                pool += [Doc(n + 1, 1, c) for c in CONTENTS]  # phantom
                ords = [Ord(d, dead) for d in pool for dead in (False, True)]
                in_stats = stats(list(inputs))
                live_in = sorted(d for d, dead in inputs.items() if not dead)
                dead_in = [d for d, dead in inputs.items() if dead]
                for k in range(args.maxout + 1):
                    for out in itertools.combinations_with_replacement(ords, k):
                        totals["outputs"] += 1
                        for name, check in (("r4", check_r4), ("r5", check_r5)):
                            if not check(inputs, out):
                                continue
                            passed[name] += 1
                            docs = [o.doc for o in out]
                            o_stats = stats(docs)
                            if any(d not in inputs for d in docs):
                                bad[name]["content"] += 1
                                if (
                                    name == "r4"
                                    and example is None
                                    and o_stats[2] > in_stats[2]
                                ):
                                    example = (inputs, out, in_stats, o_stats)
                            if sorted(o.doc for o in out if not o.dead) != live_in:
                                bad[name]["live"] += 1
                            if (
                                o_stats[0] > in_stats[0]
                                or o_stats[1] > in_stats[1]
                                or any(a > b for a, b in zip(o_stats[2], in_stats[2]))
                            ):
                                bad[name]["stats"] += 1
                # Every honest output: the live documents, and any subset of the dead, carried dead.
                for r in range(len(dead_in) + 1):
                    for carried in itertools.combinations(dead_in, r):
                        out = tuple(Ord(d, False) for d in live_in) + tuple(
                            Ord(d, True) for d in carried
                        )
                        if not check_r5(inputs, out):
                            honest_refused += 1
    print(
        f"inputs of up to {args.maxin} documents, outputs of up to {args.maxout} ordinals: {totals['inputs']:,} inputs, "
        f"{totals['outputs']:,} outputs, {time.time() - start:.1f} s"
    )
    for name in ("r4", "r5"):
        b = bad[name]
        print(
            f"{name}: passes {passed[name]:,}; of those, phantom or altered content {b['content']:,}, "
            f"live documents wrong {b['live']:,}, a statistic above the inputs' {b['stats']:,}"
        )
    print(
        f"r5 refuses honest outputs (live documents and any subset of the dead): {honest_refused}"
    )
    if example:
        inputs, out, i_s, o_s = example
        print("r4 example (dead_stats_plant's shape):")
        print(
            "  inputs: "
            + ", ".join(
                f"docid {d.docid} {sorted(d.content[0])} len {d.content[1]} {'dead' if dead else 'live'}"
                for d, dead in inputs.items()
            )
        )
        print(
            "  output: "
            + ", ".join(
                f"docid {o.doc.docid} {sorted(o.doc.content[0])} len {o.doc.content[1]} {'dead' if o.dead else 'live'}"
                for o in out
            )
        )
        print(
            f"  doc_count {i_s[0]} -> {o_s[0]}, total length {i_s[1]} -> {o_s[1]}, df {dict(zip(TERMS, i_s[2]))} -> {dict(zip(TERMS, o_s[2]))}"
        )
    ok = all(v == 0 for v in bad["r5"].values()) and honest_refused == 0
    print(
        "RESULT: "
        + (
            "r5 passes no phantom, altered, or statistic-raising output and every honest one"
            if ok
            else "r5 FAILS"
        )
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
