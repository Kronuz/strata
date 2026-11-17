#!/usr/bin/env python3
"""Live heap by owner, from a stopped footprint run (quest group-core A6; macOS).

  test/group/footprint_owners.py BUILD_DIR COUNT [--keep DIR]

Runs `group_footprint_test COUNT 0` under lldb with MallocStackLogging, stops it at its idle
minute (its first getrusage), reads every live allocation's stack with `malloc_history
-callTree -invert` and charges each to its innermost owner frame: strata::group, consensus,
journal, the strata index, or the host (the test). Prints bytes per group by owner and the
largest sites. Sizes are malloc's blocks (size classes), not the requested bytes the test
counts, so the totals run a little above its heap line.
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


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    build, count = pathlib.Path(sys.argv[1]).resolve(), int(sys.argv[2])
    keep = pathlib.Path(sys.argv[sys.argv.index("--keep") + 1]) if "--keep" in sys.argv else None
    workdir = keep or pathlib.Path.cwd()
    workdir.mkdir(parents=True, exist_ok=True)
    cmds = workdir / "footprint_owners.lldb"
    cmds.write_text(
        "settings set target.env-vars MallocStackLogging=1 GROUP_FOOTPRINT_NOSYNC=1 GROUP_FOOTPRINT_WAVE=8\n"
        f"b getrusage\nrun {count} 0\nscript import time; time.sleep(600)\nkill\nquit\n")
    log = open(workdir / "footprint_owners.log", "w")
    lldb = subprocess.Popen(["lldb", "-b", "-s", str(cmds), str(build / "group_footprint_test")], stdout=log, stderr=subprocess.STDOUT, cwd=workdir)
    try:
        pid = None
        for _ in range(1200):
            time.sleep(0.5)
            text = (workdir / "footprint_owners.log").read_text(errors="replace")
            if "stop reason = breakpoint" in text:
                found = subprocess.run(["pgrep", "-f", f"group_footprint_test {count} 0"], capture_output=True, text=True).stdout.split()
                pid = next((p for p in found if int(p) != lldb.pid), None)
                break
            if lldb.poll() is not None:
                break
        if not pid:
            print("the run did not stop at its idle minute", file=sys.stderr)
            return 1
        tree = subprocess.run(["malloc_history", pid, "-callTree", "-invert", "-ignoreThreads"], capture_output=True, text=True).stdout
        if keep:
            (keep / "footprint_owners.tree").write_text(tree)
    finally:
        lldb.kill()
        lldb.wait()
    leaves = parse(tree)
    by_owner = collections.Counter()
    by_site = collections.Counter()
    for who, site, size in leaves:
        by_owner[who] += size
        by_site[(who, site)] += size
    print(f"live heap per group by owner ({count} groups; malloc blocks):")
    for who, size in by_owner.most_common():
        print(f"  {who:10} {size / count:9.0f} B")
    print("largest sites per group:")
    for (who, site), size in by_site.most_common(25):
        if size / count >= 16:
            print(f"  {who:10} {size / count:7.0f} B  {site}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
