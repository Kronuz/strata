#!/usr/bin/env python3
"""T6 across versions (manifest-log): builds test/digest_streams.cc against this tree's headers and
against REF's, runs both on the same streams and compares their lines (verdicts and digests).

usage: test/digest_vs_ref.py BUILD_DIR REF [STREAMS]
BUILD_DIR is a configured build of this checkout with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON and
digest_streams built. Exits 1 when any line differs.
"""
import json
import pathlib
import shlex
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        return 2
    build = pathlib.Path(sys.argv[1]).resolve()
    ref = sys.argv[2]
    streams = sys.argv[3] if len(sys.argv) == 4 else "40"
    out = ROOT / ".scratch" / "digest-ref"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    archive = subprocess.run(["git", "archive", ref, "include"], cwd=ROOT, check=True, stdout=subprocess.PIPE).stdout
    subprocess.run(["tar", "-x", "-C", str(out)], input=archive, check=True)
    entry = [e for e in json.load(open(build / "compile_commands.json")) if e["file"].endswith("test/digest_streams.cc")][0]
    args = entry.get("arguments") or shlex.split(entry["command"])
    # Drop the object output and compile-only flags; link with the libraries strata::core brings.
    keep, skip = [], False
    for a in args:
        if skip:
            skip = False
        elif a == "-o":
            skip = True
        elif a != "-c":
            keep.append(a)
    libs = sorted(str(p) for p in (build / "_deps").rglob("*.a"))
    lines = {}
    for side, include in (("head", ROOT / "include"), (ref, out / "include")):
        exe = out / f"digest_streams-{side.replace('/', '_')}"
        cmd = [a.replace(str(ROOT / "include"), str(include)) for a in keep] + ["-o", str(exe)] + libs
        subprocess.run(cmd, cwd=entry["directory"], check=True)
        r = subprocess.run([str(exe), streams], stdout=subprocess.PIPE, text=True)
        if r.returncode != 0:
            print(f"{side}: digest_streams exited {r.returncode}")
            return 1
        lines[side] = r.stdout.splitlines()
        print(f"{side}: {len(lines[side])} streams")
    diff = [(a, b) for a, b in zip(lines["head"], lines[ref]) if a != b]
    for a, b in diff[:5]:
        print(f"head: {a}\n{ref}: {b}")
    same = not diff and len(lines["head"]) == len(lines[ref])
    print(f"digests equal to {ref} on {len(lines['head'])} streams" if same else f"{len(diff)} streams differ")
    return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main())
