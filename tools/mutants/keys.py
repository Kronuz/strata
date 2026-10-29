#!/usr/bin/env python3
"""Mutants of the keyed-index rules (fields-and-keys F3): each edits one rule in a copy of the
headers, builds the named test against the copy and expects it to fail.

usage: tools/mutants/keys.py BUILD_DIR [NAME ...]
BUILD_DIR is a Makefile or Ninja build configured with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON and the
tests built. Run it under a hold: `quiet light NAME MINUTES "what" -- tools/mutants/keys.py DIR`.
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# name: (test, header, old text, new text); old must occur exactly once.
MUTANTS = {
    "key-first-byte-refused": ("index_test", "reader.hh", "if (!vc.keyed || d.key.size() > format::max_term_bytes) {", "if (!vc.keyed || d.key.size() > format::max_term_bytes || (unsigned char)d.key[0] == 0xff) {"),
    "key-q-refused": ("index_test", "reader.hh", "if (!vc.keyed || d.key.size() > format::max_term_bytes) {", "if (!vc.keyed || d.key.size() > format::max_term_bytes || d.key[0] == 'Q') {"),
    "keyless-index-takes-a-key": ("index_test", "reader.hh", "if (!vc.keyed || d.key.size() > format::max_term_bytes) {", "if (d.key.size() > format::max_term_bytes) {"),
    "empty-key-addresses": ("index_test", "index.hh", "if (!(m_.configured & format::bit::keys) || o.key.empty()) { return reject(verdict::invalid); }", "if (!(m_.configured & format::bit::keys)) { return reject(verdict::invalid); }"),
    "keyless-index-addressed-by-key": ("index_test", "index.hh", "if (!(m_.configured & format::bit::keys) || o.key.empty()) { return reject(verdict::invalid); }", "if (o.key.empty()) { return reject(verdict::invalid); }"),
    "created-keyless-when-asked-keyed": ("index_test", "index.hh", "(opts.keyed ? 0 : format::bit::keys)", "0"),
    "open-keyed-on-keyless": ("index_test", "index.hh", "if (opts_.keyed && !(m_.configured & format::bit::keys)) {", "if (false) {"),
    "manifest-keeps-a-prefix-field": ("files_test", "files.hh", "	put_ref(out, m.fields);\n", "	put(out, 0);\n	put_ref(out, m.fields);\n"),
}


def run(cmd, cwd):
    return subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True)


def main():
    build = os.path.abspath(sys.argv[1])
    names = sys.argv[2:] or list(MUTANTS)
    out_root = os.path.join(ROOT, ".scratch", "mutants")
    db = json.load(open(os.path.join(build, "compile_commands.json")))
    results = {}
    for name in ["(unmutated)"] + names:
        test, header, old, new = MUTANTS[name] if name != "(unmutated)" else ("index_test", None, None, None)
        out = os.path.join(out_root, name.strip("()"))
        shutil.rmtree(out, ignore_errors=True)
        shutil.copytree(os.path.join(ROOT, "include"), os.path.join(out, "include"))
        if header:
            path = os.path.join(out, "include", "strata", "core", header)
            text = open(path).read()
            assert text.count(old) == 1, f"{name}: the text to mutate occurs {text.count(old)} times"
            open(path, "w").write(text.replace(old, new))
        entry = [e for e in db if e["file"].endswith(f"test/{test}.cc")][0]
        obj = os.path.join(out, "m.o")
        cc = entry["command"].replace(ROOT + "/include", os.path.join(out, "include"))
        cc = cc.split(" -o ")[0] + f" -o {obj} -c " + entry["file"]
        link = open(os.path.join(build, "CMakeFiles", f"{test}.dir", "link.txt")).read().strip()
        exe = os.path.join(out, test)
        ld = link.replace(f"CMakeFiles/{test}.dir/test/{test}.cc.o", obj).replace(f"-o {test}", f"-o {exe}")
        if run(cc, entry["directory"]).returncode != 0:
            results[name] = "BUILD ERROR"
        elif run(ld, build).returncode != 0:
            results[name] = "LINK ERROR"
        else:
            r = run(exe, build)
            results[name] = "survived" if r.returncode == 0 else "killed"
        print(f"{name}: {results[name]}", flush=True)
        shutil.rmtree(out, ignore_errors=True)
    bad = [n for n, r in results.items() if (r != "killed") != (n == "(unmutated)")]
    print("ok" if not bad else "PROBLEM: " + ", ".join(bad))
    return 1 if bad else 0


sys.exit(main())
