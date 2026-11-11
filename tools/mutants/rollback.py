#!/usr/bin/env python3
"""CT-R5's rollback, rechecked (cold-tier K3): apply_atomic_test (the corrupt-read cases and the
allocation sweep) and tools/mutants/apply_throw_probe.cc pass on the tree, and each revert of the
rollback fails one of them.

usage: tools/mutants/rollback.py BUILD_DIR [NAME ...]
BUILD_DIR is a Unix Makefiles build configured with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON and
apply_atomic_test built. Run it under a hold or as a campaign job.
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROBE = os.path.join(ROOT, "tools", "mutants", "apply_throw_probe.cc")

# name: (header, old text, new text); old must occur exactly once.
MUTANTS = {
    # CT-R5 reverted: a stage-1 throw leaves its earlier operations in the buffer.
    "no-rollback-on-throw": ("index.hh", "\t\t\tabandon(reserve);\n\t\t\tthrow;", "\t\t\tthrow;"),
    # CT-R5a reverted: an undo step's push can fail after the change it undoes.
    "undo-not-reserved": ("index.hh", "if (undo_.capacity() - undo_.size() < n) {", "if (false) {"),
    # The rollback restores no slot.
    "slots-not-restored": ("index.hh", "case undo_step::key_slot: b->keys.restore(it->place); break;", "case undo_step::key_slot: break;"),
}


def run(cmd, cwd):
    return subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True)


def build(db, build_dir, include, out, source, name):
    """Compiles `source` with apply_atomic_test's flags against `include`; links like the test."""
    entry = [e for e in db if e["file"].endswith("test/apply_atomic_test.cc")][0]
    obj = os.path.join(out, name + ".o")
    cc = entry["command"].replace(ROOT + "/include", include)
    cc = cc.split(" -o ")[0] + f" -I {ROOT}/test -o {obj} -c {source}"
    link = open(os.path.join(build_dir, "CMakeFiles", "apply_atomic_test.dir", "link.txt")).read().strip()
    exe = os.path.join(out, name)
    ld = link.replace("CMakeFiles/apply_atomic_test.dir/test/apply_atomic_test.cc.o", obj).replace("-o apply_atomic_test", f"-o {exe}")
    if run(cc, entry["directory"]).returncode != 0:
        return None, "BUILD ERROR"
    if run(ld, build_dir).returncode != 0:
        return None, "LINK ERROR"
    return exe, None


def main():
    build_dir = os.path.abspath(sys.argv[1])
    names = sys.argv[2:] or list(MUTANTS)
    out_root = os.path.join(ROOT, ".scratch", "mutants-rollback")
    db = json.load(open(os.path.join(build_dir, "compile_commands.json")))
    results = {}
    for name in ["(unmutated)"] + names:
        out = os.path.join(out_root, name.strip("()"))
        shutil.rmtree(out, ignore_errors=True)
        shutil.copytree(os.path.join(ROOT, "include"), os.path.join(out, "include"))
        if name != "(unmutated)":
            header, old, new = MUTANTS[name]
            path = os.path.join(out, "include", "strata", "core", header)
            text = open(path).read()
            assert text.count(old) == 1, f"{name}: the text to mutate occurs {text.count(old)} times"
            open(path, "w").write(text.replace(old, new))
        failed = []
        for exe_name, source in (("apply_atomic_test", os.path.join(ROOT, "test", "apply_atomic_test.cc")), ("apply_throw_probe", PROBE)):
            exe, err = build(db, build_dir, os.path.join(out, "include"), out, source, exe_name)
            if err:
                results[name] = err
                break
            r = run(exe, build_dir)
            last = (r.stdout.strip().splitlines() or [""])[-1]
            print(f"  {name} {exe_name}: exit {r.returncode}: {last}", flush=True)
            if r.returncode != 0:
                failed.append(exe_name)
        else:
            results[name] = "killed by " + ", ".join(failed) if failed else "survived"
        print(f"{name}: {results[name]}", flush=True)
        shutil.rmtree(out, ignore_errors=True)
    bad = [n for n, r in results.items() if (not r.startswith("killed")) != (n == "(unmutated)")]
    print("ok" if not bad else "PROBLEM: " + ", ".join(bad))
    return 1 if bad else 0


sys.exit(main())
