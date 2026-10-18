#!/usr/bin/env python3
"""ab.py over the fixture A/B in test/fixtures/ab: the set rows as before, a row per class, and
a row per stratum (a tag with a colon, QG-R22), each query keyed as strata_engine prints it."""

from __future__ import annotations

import contextlib
import functools
import io
import math
import os
import sys
import unittest
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "..", "..", "test", "fixtures", "ab")
sys.path.insert(0, HERE)

import ab

BACKSLASH = chr(92)


def by_key(rows: list[ab.Row]) -> dict[tuple[str, str], ab.Row]:
    return {(r["command"], r["row"]): r for r in rows}


@dataclass(frozen=True)
class Scored:
    sets: dict[tuple[str, str], ab.Row]
    rows: list[ab.Row]
    classes: dict[tuple[str, str], ab.Row]
    untagged: dict[str, int]


@functools.cache
def scored() -> Scored:
    best, rounds, results = ab.load(FIXTURE)
    rows, untagged = ab.class_rows(best, rounds, results, ab.load_tags(FIXTURE))
    sets = by_key(ab.set_rows(best, rounds, results))
    return Scored(sets, rows, by_key(rows), untagged)


class Scorer(unittest.TestCase):
    def check(self, row: ab.Row, n: int, gm: float, mismatches: int = 0) -> None:
        self.assertEqual(row["n"], n)
        self.assertAlmostEqual(row["gm"], gm, places=12)
        self.assertLessEqual(row["lo"], row["gm"] + 1e-12)
        self.assertGreaterEqual(row["hi"], row["gm"] - 1e-12)
        self.assertEqual(row["mismatches"], mismatches)

    def test_strata_have_rows_of_their_own(self) -> None:
        c = scored().classes
        self.check(c[("TOP_100_COUNT", "union:num_tokens_3")], 1, 0.5)
        self.check(c[("TOP_100_COUNT", "union:num_tokens_>3")], 1, 0.25)
        self.check(c[("TOP_100_COUNT", "intersection:num_tokens_2")], 2, 1.0, 1)
        self.check(c[("TOP_100_COUNT", "term:frequent")], 1, 0.8)
        self.check(c[("TOP_100_COUNT", "term:rare")], 2, 1.0)
        for tag, spread in (
            ("union:num_tokens_3", [0.5, 0.6]),
            ("union:num_tokens_>3", [0.25, 0.3]),
        ):
            got = c[("TOP_100_COUNT", tag)]["rounds"]
            self.assertEqual(len(got), len(spread))
            for x, want in zip(got, spread):
                self.assertAlmostEqual(x, want, places=12)

    def test_strata_follow_their_class(self) -> None:
        tags = [r["row"] for r in scored().rows if r["command"] == "TOP_100_COUNT"]
        self.assertEqual(
            tags,
            [
                "global",
                "intersection",
                "intersection:num_tokens_2",
                "phrase",
                "term",
                "term:frequent",
                "term:rare",
                "union",
                "union:num_tokens_3",
                "union:num_tokens_>3",
            ],
        )

    def test_classes(self) -> None:
        c = scored().classes
        union = c[("TOP_100_COUNT", "union")]
        self.check(union, 2, math.sqrt(0.125))
        self.assertGreaterEqual(union["lo"], 0.25 - 1e-12)
        self.assertLessEqual(union["hi"], 0.5 + 1e-12)
        self.check(c[("TOP_100_COUNT", "intersection")], 2, 1.0, 1)
        self.check(c[("TOP_100_COUNT", "phrase")], 1, 1.0)
        self.check(c[("TOP_100_COUNT", "global")], 5, 0.125 ** (1 / 5), 1)
        self.check(c[("TOP_100_COUNT", "term")], 3, 0.8 ** (1 / 3))

    def test_sets_as_before(self) -> None:
        s = scored().sets
        self.assertEqual(
            list(s),
            [
                ("TOP_100_COUNT", "queries"),
                ("TOP_100_COUNT", "terms"),
                ("COUNT", "queries"),
            ],
        )
        self.check(s[("TOP_100_COUNT", "queries")], 6, 0.125 ** (1 / 6), 1)
        self.check(s[("TOP_100_COUNT", "terms")], 3, 0.8 ** (1 / 3))
        self.check(s[("COUNT", "queries")], 6, 0.9)

    def test_rows_stay_in_their_command(self) -> None:
        c = scored().classes
        self.assertNotIn(("COUNT", "term"), c)
        for tag in (
            "global",
            "union",
            "union:num_tokens_3",
            "union:num_tokens_>3",
            "phrase",
        ):
            self.check(c[("COUNT", tag)], c[("TOP_100_COUNT", tag)]["n"], 0.9)

    def test_keys_as_strata_engine_prints_them(self) -> None:
        escaped = '{"query": "caf' + BACKSLASH + 'u00e9", "tags": []}\n'
        self.assertEqual(ab.query_key(escaped), "cafu00e9")
        quoted = '{"query": "' + BACKSLASH + '"a b' + BACKSLASH + '"", "tags": []}'
        self.assertEqual(ab.query_key(quoted), '"a b"')
        self.assertIsNone(ab.query_key('{"q": "x"}'))
        # The fixture's escaped term is tagged under the key the TSV carries.
        self.assertEqual(scored().untagged, {"queries": 1})

    def test_markdown(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            ab.main(FIXTURE)
        text = out.getvalue()
        self.assertIn("| Command | Set | n | B/A | 95% | Rounds | Mismatches |", text)
        self.assertIn("| Command | Class | n | B/A | 95% | Rounds | Mismatches |", text)
        self.assertIn(
            "| `TOP_100_COUNT` | union:num_tokens_3 | 1 | 0.500 | 0.500–0.500 | 0.50 0.60 | 0 |",
            text,
        )
        self.assertIn(
            "Queries of the queries set without tags, in no class row: 1", text
        )


if __name__ == "__main__":
    unittest.main()
