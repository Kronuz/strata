"""Re-pins the design's strata citations from one strata commit to another (DM-R39).

For each `file.hh:N` or `file.hh:A-B` of strata's include/ files (and `format-13.md:...`), and the
bare backticked numbers that follow it in the same parenthetical, maps each cited line through
difflib's equal blocks between the two commits' copies of that file. Prints every citation that
moves or no longer maps; with --apply, rewrites in place the ones whose every cited line maps, and
leaves the rest for a hand edit. Revision 7: 026ee10 to 3c86484, 29 unchanged, 56 moved, 7 by hand.
Usage, from the repository's root:
  python3 -I bench/durability/repin_citations.py DESIGN OLD NEW [--apply]
"""

from __future__ import annotations

import difflib
import re
import subprocess
import sys
from pathlib import Path

FILES = {
    f: f"include/strata/{f}"
    for f in (
        "index.hh",
        "reader.hh",
        "format.hh",
        "writer.hh",
        "search.hh",
        "searcher.hh",
        "files.hh",
        "keys.hh",
        "merge.hh",
        "tuple_buffer.hh",
        "pool_executor.hh",
    )
}
FILES["format-13.md"] = "docs/design/format-13.md"
CITE = re.compile(
    r"(?<![\w-])(" + "|".join(re.escape(f) for f in FILES) + r"):(\d+)(?:-(\d+))?"
)
BARE = re.compile(r"`:?(\d+)(?:-(\d+))?`")
STOP = re.compile(r"[A-Za-z_\-0-9]+\.(hh|h|md|cc|py|txt|out)[:`]|\)|;")


def at(ref: str, path: str) -> list[str]:
    run = subprocess.run(
        ["git", "show", f"{ref}:{path}"], check=True, capture_output=True, text=True
    )
    return run.stdout.split("\n")


def line_map(old: list[str], new: list[str]) -> dict[int, int]:
    """Old 1-based line to new 1-based line, for lines in equal blocks only."""
    sm = difflib.SequenceMatcher(a=old, b=new, autojunk=False)
    m: dict[int, int] = {}
    for tag, i1, i2, j1, _ in sm.get_opcodes():
        if tag == "equal":
            for k in range(i2 - i1):
                m[i1 + k + 1] = j1 + k + 1
    return m


def spans(line: str) -> list[tuple[str, int, int, int, int]]:
    """(file, start, end, first, last) for each citation in the line, bare follow-ons included."""
    out = []
    for mt in CITE.finditer(line):
        f, a = mt.group(1), int(mt.group(2))
        b = int(mt.group(3) or a)
        out.append((f, mt.start(2), mt.end(3) if mt.group(3) else mt.end(2), a, b))
        rest = line[mt.end() :]
        stop = STOP.search(rest)
        for b2 in BARE.finditer(rest[: stop.start()] if stop else rest):
            x = int(b2.group(1))
            y = int(b2.group(2) or x)
            end = b2.end(2) if b2.group(2) else b2.end(1)
            out.append((f, mt.end() + b2.start(1), mt.end() + end, x, y))
    return out


def main() -> None:
    design, old_ref, new_ref = sys.argv[1:4]
    maps = {f: line_map(at(old_ref, p), at(new_ref, p)) for f, p in FILES.items()}
    lines = Path(design).read_text().split("\n")
    counts = {"unchanged": 0, "moved": 0, "by hand": 0}
    out = []
    for ln, line in enumerate(lines, 1):
        edits = []
        for f, s, e, a, b in spans(line):
            m = maps[f]
            miss = [k for k in range(a, b + 1) if k not in m]
            if miss:
                counts["by hand"] += 1
                print(
                    f"DM:{ln} {f}:{a}-{b} CHANGED (lines {miss[:5]} not kept): {line.strip()[:110]}"
                )
            elif (m[a], m[b]) == (a, b):
                counts["unchanged"] += 1
            else:
                counts["moved"] += 1
                rng = f"{m[a]}" if a == b else f"{m[a]}-{m[b]}"
                edits.append((s, e, rng))
                print(f"DM:{ln} {f}:{a}-{b} -> {rng}")
        for s, e, txt in sorted(edits, reverse=True):
            line = line[:s] + txt + line[e:]
        out.append(line)
    print("# " + ", ".join(f"{v} {k}" for k, v in counts.items()))
    if "--apply" in sys.argv:
        Path(design).write_text("\n".join(out))


if __name__ == "__main__":
    main()
