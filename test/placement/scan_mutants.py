#!/usr/bin/env python3
"""Mutants for the placement scan (quest placement A2).

Each mutant breaks one rule of the scan; its test must fail with it. Usage:
  test/placement/scan_mutants.py BUILD_DIR NAME [TEST ARGS...]   one mutant: the test's status
  test/placement/scan_mutants.py BUILD_DIR --list
BUILD_DIR is a configured build of this checkout. The mutant is applied, placement_scan_test
rebuilt and run with TEST ARGS, and the file restored. Exits 0 when the test passes (the mutant
survives), the test's nonzero status when it fails, and 127 when the mutant does not apply or does
not build, so a `campaign job submit --fails-without` job passes only on a killed mutant.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCAN = "include/strata/placement/scan.hh"
TARGET = "placement_scan_test"

# name: [(file, old, new), ...]
MUTANTS = {
    # The scan reads on instead of seeking past what the matcher cannot match: the same listing,
    # every record of a dead subtree read.
    "no-seek-past": [(SCAN,
        "\t\t\t\t++work.seeks;\n\t\t\t\treturn *next == dead ? source.skip(child(depth, dead)) : source.seek(child(depth, *next));\n",
        "\t\t\t\treturn source.next();\n")],
}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 127
    build, name, args = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
    if name == "--list":
        print("\n".join(MUTANTS))
        return 0
    if name not in MUTANTS:
        print(f"no mutant {name!r}", file=sys.stderr)
        return 127
    saved = {}
    try:
        for path, old, new in MUTANTS[name]:
            f = ROOT / path
            text = saved.setdefault(f, f.read_text()) if f not in saved else f.read_text()
            if text.count(old) != 1:
                print(f"mutant {name}: {path} does not hold its text once", file=sys.stderr)
                return 127
            f.write_text(text.replace(old, new))
        built = subprocess.run(["cmake", "--build", str(build), "--target", TARGET, "-j", "4"], capture_output=True, text=True)
        if built.returncode != 0:
            print(built.stdout[-4000:] + built.stderr[-4000:], file=sys.stderr)
            return 127
        run = subprocess.run([str(build / TARGET), *args])
        print(f"mutant {name}: the test {'fails (killed)' if run.returncode else 'passes (survives)'}")
        return run.returncode
    finally:
        for f, text in saved.items():
            f.write_text(text)
        subprocess.run(["cmake", "--build", str(build), "--target", TARGET, "-j", "4"], capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
