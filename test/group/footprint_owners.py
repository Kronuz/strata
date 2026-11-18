#!/usr/bin/env python3
"""Live heap by owner, from a stopped footprint run (quest group-core A6; macOS).

  test/group/footprint_owners.py BUILD_DIR COUNT [--closes N] [--at PHASE] [--base PHASE] [--keep DIR]
  test/group/footprint_owners.py BUILD_DIR COUNT [--closes N] --terms W:PHASE,... [--keep DIR]

Runs `group_footprint_test COUNT N` (N 0) under lldb with MallocStackLogging, stops it at PHASE
(`footprint_phase`: bare-closed, rows-before, before (the groups), idle (the default), closed,
destroyed, rows-closed), reads every live allocation's stack with `malloc_history -callTree -invert` and
charges each to its innermost owner frame: strata::group, consensus, journal, the strata index,
or the host (the test). Prints bytes per group by owner and the largest sites; with --base, what
PHASE holds past BASE (two runs); with --terms, the sum of each phase's heap times its weight
(what a destroyed group leaves past a closed bare row: 2:destroyed,-2:before,1:rows-before,
-1:rows-closed). GROUP_FOOTPRINT_WAVE (1, as ctest runs) sets the wave. Sizes are malloc's blocks (size classes), not the requested
bytes the test counts, so the totals run a little above its heap line.
"""
import collections
import os
import pathlib
import re
import subprocess
import sys
import time

LINE = re.compile(r"^(?P<lead>[ +!:|]*?)(?P<count>\d+) \((?P<size>[\d.]+)(?P<unit>K|M|G| bytes)\) (?P<frame>.*)$")
UNIT = {"K": 1024, "M": 1 << 20, "G": 1 << 30, " bytes": 1}
OWNERS = [("strata::group::", "group"), ("kronuz::consensus::", "consensus"), ("kronuz::journal::", "journal"), ("strata::", "index")]
WRAPPERS = ("std::make_unique", "std::allocate_shared", "std::construct_at")


def owner(path):
    """The owner of an allocation, from its frames innermost first."""
    for frame in path:
        name = frame.split("  (in ")[0]
        if name.startswith(WRAPPERS):
            # What is made decides: its type's namespace, else the caller's.
            made = name[name.index("<") + 1:] if "<" in name else ""
            if made.startswith("kronuz::journal::PosixIO"):
                return "host"  # the directories the host hands its groups
            for prefix, who in OWNERS:
                if made.startswith(prefix):
                    return who
            continue
        for prefix, who in OWNERS:
            if name.startswith(prefix) or name.startswith("<deduplicated_symbol>") and prefix in frame:
                return who
        if name.startswith(("main", "start")):
            return "host"
    return "other"


def parse(text):
    """(owner, innermost site, bytes) per leaf of the inverted tree."""
    out = []
    stack = []  # (depth, frame)
    rows = [m for m in map(LINE.match, text.splitlines()) if m]
    for i, m in enumerate(rows):
        depth = len(m["lead"])
        while stack and stack[-1][0] >= depth:
            stack.pop()
        stack.append((depth, m["frame"]))
        leaf = i + 1 == len(rows) or len(rows[i + 1]["lead"]) <= depth
        if leaf and len(stack) > 1:
            path = [f for _, f in stack[2:]] or [stack[-1][1]]  # past TOTAL and malloc's own frame
            out.append((owner(path), path[0].split("  (in ")[0][:120], float(m["size"]) * UNIT[m["unit"]]))
    return out


def live(build, count, closes, at, workdir, keep):
    """(owner, site, bytes) of every live allocation with the run stopped at `at`, or None."""
    cmds = workdir / f"footprint_owners.{at}.lldb"
    cmds.write_text(
        f"settings set target.env-vars MallocStackLogging=1 GROUP_FOOTPRINT_NOSYNC=1 GROUP_FOOTPRINT_WAVE={os.environ.get('GROUP_FOOTPRINT_WAVE', '1')} GROUP_FOOTPRINT_PAUSE={at}\n"
        f"b footprint_phase\nrun {count} {closes}\nscript import time; time.sleep(600)\nkill\nquit\n")
    logged = workdir / f"footprint_owners.{at}.log"
    log = open(logged, "w")
    lldb = subprocess.Popen(["lldb", "-b", "-s", str(cmds), str(build / "group_footprint_test")], stdout=log, stderr=subprocess.STDOUT, cwd=workdir)
    try:
        pid = None
        for _ in range(1200):
            time.sleep(0.5)
            if "stop reason = breakpoint" in logged.read_text(errors="replace"):
                found = subprocess.run(["pgrep", "-f", f"group_footprint_test {count} {closes}"], capture_output=True, text=True).stdout.split()
                pid = next((p for p in found if int(p) != lldb.pid), None)
                break
            if lldb.poll() is not None:
                break
        if not pid:
            return None
        tree = subprocess.run(["malloc_history", pid, "-callTree", "-invert", "-ignoreThreads"], capture_output=True, text=True).stdout
        if keep:
            (keep / f"footprint_owners.{at}.tree").write_text(tree)
    finally:
        lldb.kill()
        lldb.wait()
    return parse(tree)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    build, count = pathlib.Path(sys.argv[1]).resolve(), int(sys.argv[2])

    def flag(name, default):
        return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

    keep = pathlib.Path(flag("--keep", "")).resolve() if "--keep" in sys.argv else None
    closes, at, base = int(flag("--closes", "0")), flag("--at", "idle"), flag("--base", None)
    workdir = keep or pathlib.Path.cwd()
    workdir.mkdir(parents=True, exist_ok=True)
    by_owner = collections.Counter()
    by_site = collections.Counter()
    terms = [(at, 1)] + ([(base, -1)] if base else [])
    if "--terms" in sys.argv:
        terms = [(p, int(w)) for w, p in (t.split(":") for t in flag("--terms", "").split(","))]
        at, base = flag("--terms", ""), None
    for phase, sign in terms:
        leaves = live(build, count, closes, phase, workdir, keep)
        if leaves is None:
            print(f"the run did not stop at {phase}", file=sys.stderr)
            return 1
        for who, site, size in leaves:
            by_owner[who] += sign * size
            by_site[(who, site)] += sign * size
    since = f" past {base}" if base else ""
    print(f"live heap per group by owner at {at}{since} ({count} groups, {closes} closed; malloc blocks):")
    for who, size in sorted(by_owner.items(), key=lambda kv: -abs(kv[1])):
        print(f"  {who:10} {size / count:9.0f} B")
    print("largest sites per group:")
    for (who, site), size in sorted(by_site.items(), key=lambda kv: -abs(kv[1]))[:25]:
        if abs(size) / count >= 16:
            print(f"  {who:10} {size / count:7.0f} B  {site}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
