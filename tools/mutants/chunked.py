#!/usr/bin/env python3
"""Mutants of the chunked hazard registry (shared-reader revision 4, R4.2 to R4.6): each edits one
rule in a copy of the headers, builds a test against the copy and expects the test to fail.

usage: tools/mutants/chunked.py rel=BUILD asan=BUILD tsan=BUILD [NAME ...]
Each BUILD is a configured ninja build dir with hazard_test and hazard_litmus_test built (rel: a
plain optimized build; asan: -fsanitize=address,undefined; tsan: -fsanitize=thread). Run it under a
hold: `quiet light NAME MINUTES "what" -- tools/mutants/chunked.py rel=... asan=... tsan=...`.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
H = "hazard.hh"

# name: (build kind, test, old text, new text); old must occur exactly once.
MUTANTS = {
    # R4.6: the chunk link published and walked relaxed; TSan sees a walk reach a chunk before its contents.
    "link-relaxed": ("tsan", "hazard_test", "static constexpr std::memory_order link = std::memory_order_seq_cst;", "static constexpr std::memory_order link = std::memory_order_relaxed;"),
    # R4.5: the scan misses the newest chunk.
    "scan-skips-newest-chunk": ("rel", "hazard_test", "for (chunk* c = head_.load(Orders::link); c; c = c->next.load(Orders::link)) {\n\t\t\tfor (const record& r : c->r) {",
                                "for (chunk* c = head_.load(Orders::link); c && c->next.load(Orders::link); c = c->next.load(Orders::link)) {\n\t\t\tfor (const record& r : c->r) {"),
    "scan-skips-newest-chunk-litmus": ("rel", "hazard_litmus_test", "for (chunk* c = head_.load(Orders::link); c; c = c->next.load(Orders::link)) {\n\t\t\tfor (const record& r : c->r) {",
                                       "for (chunk* c = head_.load(Orders::link); c && c->next.load(Orders::link); c = c->next.load(Orders::link)) {\n\t\t\tfor (const record& r : c->r) {"),
    # R4.4: chunks freed while a record is claimed, then written by the guard's release.
    "chunks-freed-with-a-record-claimed": ("asan", "hazard_test", "if (held) {", "if (false) {"),
    # L1: the destructor's check allocates (a slot() walk over the capacity), as Xapiand's claimed() did.
    "claimed-allocates": ("rel", "hazard_test", "		for (const chunk* c = head_.load(std::memory_order_acquire); c; c = c->next.load(std::memory_order_acquire)) {\n			for (const record& r : c->r) {\n				if (r.p.load(std::memory_order_acquire)) { return true; }\n			}\n		}\n		return false;",
                         "		for (std::size_t i = 0; i < std::min<std::size_t>(cap_, 64); ++i) {\n			if (const_cast<basic_reader_registry*>(this)->slot(i).p.load(std::memory_order_acquire)) { return true; }\n		}\n		return false;"),
    # R4.3: the appender's pointer is not in the new chunk's first record when it is published.
    "first-record-not-preclaimed": ("rel", "hazard_test", "fresh->r[0].p.store(v, std::memory_order_relaxed);  // published by the link", ""),
    # R4.2: the capacity rounds up to whole chunks.
    "capacity-rounds-to-chunks": ("rel", "hazard_test", "const std::size_t n = std::min(total, cap_);", "const std::size_t n = total;"),
}


def run(cmd, cwd):
    return subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True)


def main():
    builds = {}
    names = []
    for a in sys.argv[1:]:
        if "=" in a:
            k, v = a.split("=", 1)
            builds[k] = os.path.abspath(v)
        else:
            names.append(a)
    names = names or list(MUTANTS)
    out_root = os.path.join(ROOT, ".scratch", "mutants-chunked")
    results = {}
    for name in ["(unmutated)"] + names:
        kind, test, old, new = (MUTANTS[name] if name in MUTANTS else ("rel", "hazard_test", None, None))
        build = builds[kind]
        commands = subprocess.check_output(["ninja", "-C", build, "-t", "commands", test], text=True).splitlines()
        compile_cmd = [c for c in commands if " -c " in c and f"test/{test}.cc" in c][-1]
        link_cmd = [c for c in commands if f" -o {test}" in c][-1]
        out = os.path.join(out_root, name.strip("()"))
        shutil.rmtree(out, ignore_errors=True)
        shutil.copytree(os.path.join(ROOT, "include"), os.path.join(out, "include"))
        if old is not None:
            path = os.path.join(out, "include", "strata", "core", H)
            text = open(path).read()
            assert text.count(old) == 1, f"{name}: the text to mutate occurs {text.count(old)} times"
            open(path, "w").write(text.replace(old, new))
        obj = os.path.join(out, "m.o")
        exe = os.path.join(out, test)
        obj_in = f"CMakeFiles/{test}.dir/test/{test}.cc.o"
        cc = compile_cmd.replace(ROOT + "/include", os.path.join(out, "include")).replace(obj_in, obj)
        ld = link_cmd.replace(obj_in, obj).replace(f" -o {test}", " -o " + exe)
        if run(cc, build).returncode != 0:
            results[name] = "BUILD ERROR"
        elif run(ld, build).returncode != 0:
            results[name] = "LINK ERROR"
        else:
            r = run(exe + (" 2000" if kind == "tsan" else ""), build)
            results[name] = "survived" if r.returncode == 0 else "killed"
            if r.returncode != 0:
                tail = (r.stderr or r.stdout).strip().splitlines()
                print("    " + (tail[0] if tail else "")[:160])
        print(f"{name} [{kind} {test}]: {results[name]}", flush=True)
        shutil.rmtree(out, ignore_errors=True)
    bad = [n for n, r in results.items() if (r == "killed") == (n == "(unmutated)")]
    print("ok" if not bad else "PROBLEM: " + ", ".join(bad))
    return 1 if bad else 0


sys.exit(main())
