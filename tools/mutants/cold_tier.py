#!/usr/bin/env python3
"""The cold tier's S3 to S7 mutants (cold_tier_table.py; docs/status/cold-tier-step2.md): each applies
its edits to a copy of the headers, builds its kind's tests against the copy and runs them; a mutant
is killed when one fails. The unmutated tree must pass every test first.

usage: tools/mutants/cold_tier.py BUILD_DIR [SEEDS] [ID ...]
BUILD_DIR is a Unix Makefiles build configured with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON and the tests
built; SEEDS is catalog_test's schedule count (T6: 1000). Run it as a light campaign job.
"""
import concurrent.futures as cf
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cold_tier_table import MUTANTS  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def tests_of(mid, kind, seeds):
    sched = ("catalog_test", str(seeds))
    by = {
        "S3": {"budget": [("resource_budget_test", ""), ("counted_io_test", "")]},
        "S4": {"sched": [sched], "sweep": [("catalog_sweep_test", ""), sched], "stress": [sched, ("catalog_stress_test", "400 4")]},
        "S5": {"wake": [("catalog_wake_test", "")], "sched": [("catalog_wake_test", ""), sched],
               "stress": [("catalog_stress_test", "3000 4"), ("catalog_wake_test", "")], "asan": [("catalog_wake_test", "")]},
        "S7": {"sweep": [("catalog_sweep_test", "")], "sched": [("catalog_sweep_test", ""), sched], "wake": [("catalog_wake_test", "")],
               "policy": [("catalog_policy_test", "20000")], "stress": [("catalog_stress_test", "3000 4"), ("catalog_sweep_test", "")]},
    }
    return by[mid[:2]][kind]


def run(cmd, cwd, timeout=None):
    try:
        return subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, "", "timeout")


def build(db, build_dir, include, out, test):
    entry = [e for e in db if e["file"].endswith(f"test/{test}.cc")][0]
    obj = os.path.join(out, test + ".o")
    cc = entry["command"].replace(ROOT + "/include", include)
    cc = cc.split(" -o ")[0] + f" -o {obj} -c " + entry["file"]
    link = open(os.path.join(build_dir, "CMakeFiles", f"{test}.dir", "link.txt")).read().strip()
    exe = os.path.join(out, test)
    ld = link.replace(f"CMakeFiles/{test}.dir/test/{test}.cc.o", obj).replace(f"-o {test}", f"-o {exe}")
    r = run(cc, entry["directory"])
    if r.returncode != 0:
        return None, "BUILD ERROR: " + r.stderr[-200:].replace("\n", " ")
    r = run(ld, build_dir)
    if r.returncode != 0:
        return None, "LINK ERROR: " + r.stderr[-200:].replace("\n", " ")
    return exe, None


def one(m, db, build_dir, seeds, out_root):
    mid = m["id"]
    out = os.path.join(out_root, mid)
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "include"), os.path.join(out, "include"))
    path = os.path.join(out, "include", "strata", "core", m["header"])
    text = open(path).read()
    for old, new in m["edits"]:
        if text.count(old) != 1:
            shutil.rmtree(out, ignore_errors=True)
            return mid, f"EDIT DID NOT APPLY ({text.count(old)} matches)", m["what"]
        text = text.replace(old, new)
    open(path, "w").write(text)
    res = "survived"
    for test, args in tests_of(mid, m["kind"], seeds):
        exe, err = build(db, build_dir, os.path.join(out, "include"), out, test)
        if err:
            res = err
            break
        r = run(f"{exe} {args}", build_dir, timeout=300)
        if r.returncode != 0:
            lines = [l for l in (r.stdout + r.stderr).splitlines() if "CHECK" in l or "Assert" in l or "timeout" in l or "FAIL" in l]
            res = f"killed by {test} (rc={r.returncode}): {(lines[0] if lines else 'rc only')[-100:]}"
            break
    shutil.rmtree(out, ignore_errors=True)
    return mid, res, m["what"]


def main():
    build_dir = os.path.abspath(sys.argv[1])
    seeds = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
    ids = set(sys.argv[3:])
    db = json.load(open(os.path.join(build_dir, "compile_commands.json")))
    out_root = os.path.join(ROOT, ".scratch", "mutants-cold-tier")
    base = sorted({t for m in MUTANTS for t in tests_of(m["id"], m["kind"], seeds)})
    for test, args in base:  # the control: the unmutated tests pass
        r = run(f"./{test} {args}", build_dir, timeout=600)
        print(f"(unmutated) {test} {args}: exit {r.returncode}", flush=True)
        if r.returncode != 0:
            print("PROBLEM: the unmutated tree fails " + test)
            return 1
    todo = [m for m in MUTANTS if not ids or m["id"] in ids]
    results = []
    with cf.ThreadPoolExecutor(4) as ex:
        for mid, res, what in ex.map(lambda m: one(m, db, build_dir, seeds, out_root), todo):
            print(f"{mid}: {res} | {what}", flush=True)
            results.append((mid, res))
    killed = sum(r.startswith("killed") for _, r in results)
    survived = [mid for mid, r in results if r == "survived"]
    broken = [mid for mid, r in results if not r.startswith("killed") and r != "survived"]
    print(f"{killed} of {len(results)} killed; survived: {' '.join(survived) or 'none'}; not run: {' '.join(broken) or 'none'}")
    return 0 if not survived and not broken else 1


sys.exit(main())
