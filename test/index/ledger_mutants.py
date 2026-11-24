#!/usr/bin/env python3
"""Mutants for basic_index's file ledger (quest index-restructure A3).

Each mutant breaks one transition of the ledger; the named test case must fail with it. Usage:
  test/index/ledger_mutants.py BUILD_DIR NAME [TEST ARGS...]   one mutant: the test's status
  test/index/ledger_mutants.py BUILD_DIR --list
BUILD_DIR is a configured build of this checkout. The mutant is applied, index_ledger_test rebuilt
and run with TEST ARGS, and the file restored. Exits 0 when the test passes (the mutant survives),
the test's nonzero status when it fails, and 127 when the mutant does not apply or does not build,
so a `campaign job submit --fails-without` job passes only on a killed mutant.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
LEDGER = "include/strata/core/index_ledger.hh"
TARGET = "index_ledger_test"

# name: (old, new), each in LEDGER; the case that kills it is in the comment.
MUTANTS = {
    # track counts the new size on top of the old (killed by `track`).
    "track-adds": ("\t\ton_disk_ += size - at;\n", "\t\ton_disk_ += size;\n"),
    # forget drops a removed file's bytes instead of keeping them unlinked (killed by `forget`).
    "forget-drops": (
        "\t\t\ton_disk_ -= it->second;\n\t\t\tunlinked_ += it->second;\n\t\t\tfiles_.erase(it);\n",
        "\t\t\ton_disk_ -= it->second;\n\t\t\tfiles_.erase(it);\n",
    ),
    # post leaves the file on disk as well as in flight (killed by `post`).
    "post-keeps-file": (
        "\t\t\tsize = it->second;\n\t\t\ton_disk_ -= size;\n\t\t\tfiles_.erase(it);\n",
        "\t\t\tsize = it->second;\n",
    ),
    # unpost leaves the bytes in flight (killed by `unpost`).
    "unpost-keeps-flight": (
        "\t\tin_flight_ -= it->second;\n\t\tposted_.erase(it);\n\t\treturn true;\n",
        "\t\tposted_.erase(it);\n\t\treturn true;\n",
    ),
}


def build(where: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["cmake", "--build", str(where), "--target", TARGET, "-j", "4"],
        capture_output=True,
        check=False,
        text=True,
    )


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 127
    out, name, args = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
    if name == "--list":
        print("\n".join(MUTANTS))
        return 0
    if name not in MUTANTS:
        print(f"no mutant {name!r}", file=sys.stderr)
        return 127
    f = ROOT / LEDGER
    text = f.read_text()
    old, new = MUTANTS[name]
    if text.count(old) != 1:
        print(f"mutant {name}: {LEDGER} does not hold its text once", file=sys.stderr)
        return 127
    try:
        f.write_text(text.replace(old, new))
        built = build(out)
        print(built.stdout[-2000:], end="")
        if built.returncode != 0:
            print(built.stderr[-4000:], file=sys.stderr)
            return 127
        print(f"mutant {name}: built {TARGET}; running it {' '.join(args)}", flush=True)
        run = subprocess.run([str(out / TARGET), *args], check=False)
        print(
            f"mutant {name}: the test {'fails (killed)' if run.returncode else 'passes (survives)'}"
        )
        return run.returncode
    finally:
        f.write_text(text)
        build(out)


if __name__ == "__main__":
    sys.exit(main())
