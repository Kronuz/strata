#!/usr/bin/env python3
"""Prints what the crash tests cover, to diff between two commits (faultlab-adopt-strata A1).

usage: test/crash_counts.py BUILD_DIR [SOURCE_DIR]   (run from, or pass, the checkout; BUILD_DIR is built)

Two sections, both stable between runs:
  seeds   each crash( / power_off_after( call in the crash tests' sources: the seed or cut
          expression, and how many call sites use it
  counts  per crash test (ctest -R, in parallel): its status and the stdout lines that carry
          crash points, workloads, manifests or recoveries, durations masked
"""
import collections
import pathlib
import re
import subprocess
import sys

TESTS = [
    "fields", "manifest_log", "group_crash_lone_voter", "group_voters", "group_file_set",
    "reclaim", "reclaim_bg_crash", "reclaim_bg", "reclaim_bg_mapped", "reclaim_bg_crash_mapped",
    "standalone_turn_log", "kv_strata_batch", "lifecycle", "index", "warm", "background_merge",
    "directory", "reclaim_mapped", "lifecycle_mapped", "index_mapped", "background_merge_mapped",
]
SOURCES = [
    "test/fields_test.cc", "test/manifest_log_test.cc", "test/group/lone_voter_test.cc",
    "test/group/voters_test.cc", "test/group/file_set_test.cc", "test/reclaim_test.cc",
    "test/reclaim_bg_test.cc", "test/standalone_turn_log_test.cc", "test/kv_strata_batch_test.cc",
    "test/lifecycle_test.cc", "test/index_test.cc", "test/warm_test.cc",
    "test/background_merge_test.cc", "test/directory_test.cc",
]
CALL = re.compile(r"\b(crash|power_off_after)\(([^;]*?)\)\s*;")
KEEP = re.compile(r"crash|point|workload|manifest|rollover|recover|mutation|checked|seed", re.I)
TIME = re.compile(r"\b[0-9.]+ ?(ms|s|sec|seconds|us)\b")


def seeds(root: pathlib.Path) -> None:
    print("== seeds")
    for rel in SOURCES:
        path = root / rel
        if not path.exists():
            continue
        calls = collections.Counter()
        for m in CALL.finditer(path.read_text()):
            calls[f"{m.group(1)}({' '.join(m.group(2).split())})"] += 1
        for call, n in sorted(calls.items()):
            print(f"{rel}: {call} x{n}")


def counts(build: pathlib.Path) -> int:
    print("== counts")
    regex = "^(" + "|".join(TESTS) + ")$"
    run = subprocess.run(["ctest", "--test-dir", str(build), "-R", regex, "-V", "-j4"], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    names: dict[str, str] = {}
    lines: dict[str, list[str]] = collections.defaultdict(list)
    status: dict[str, str] = {}
    for line in run.stdout.splitlines():
        if m := re.match(r"\s*Start\s+(\d+): (\S+)", line):
            names[m.group(1)] = m.group(2)
        elif m := re.match(r"(\d+): (.*)$", line):
            if KEEP.search(m.group(2)) and re.search(r"\d", m.group(2)):
                lines[m.group(1)].append(TIME.sub("<t>", m.group(2).strip()))
        elif m := re.match(r"\s*\d+/\d+ Test\s+#(\d+): (\S+) \.+\s*(.*?)\s+[0-9.]+ sec", line):
            status[m.group(1)] = m.group(3).replace("*", "").strip()
    for idx, name in sorted(names.items(), key=lambda kv: kv[1]):
        print(f"{name}: {status.get(idx, 'no status')}")
        for text in lines[idx]:
            print(f"  {text}")
    missing = set(TESTS) - set(names.values())
    if missing:
        print(f"not registered: {', '.join(sorted(missing))}")
    return run.returncode


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    seeds(pathlib.Path(sys.argv[2] if len(sys.argv) == 3 else "."))
    return counts(pathlib.Path(sys.argv[1]))


if __name__ == "__main__":
    sys.exit(main())
