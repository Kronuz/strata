#!/usr/bin/env python3
"""Mutants of the field table (fields-and-keys F1): each edits one rule in a copy of the headers,
builds test/fields_test.cc against the copy and expects the test to fail.

usage: tools/mutants/fields.py BUILD_DIR [NAME ...]   (BUILD_DIR configured with fields_test built)
Run it under a hold: `quiet light NAME MINUTES "what" -- tools/mutants/fields.py build-rel`.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# name: (header, old text, new text); old must occur exactly once.
MUTANTS = {
    "declared-as-build-size-plus-i": ("fields.hh", "return declared[r.n];", "return std::uint32_t(known + r.n);"),
    "declared-out-of-range-resolves": ("fields.hh", "if (r.n >= declared.size()) { return std::nullopt; }\n\t\t\treturn declared[r.n];", "return r.n < declared.size() ? declared[r.n] : 0;"),
    "id-above-mark-resolves": ("fields.hh", "if (r.n >= known) { return std::nullopt; }", ""),
    "chain-check-skipped": ("index.hh", "if (!chain || *chain != t.expected_fields->chain) {", "if (!chain) {"),
    "shorter-table-accepted": ("index.hh", "if (!chain || *chain != t.expected_fields->chain) {", "if (chain && *chain != t.expected_fields->chain) {"),
    "id-without-mark-resolves": ("index.hh", "t.expected_fields ? t.expected_fields->size : 0}", "t.expected_fields ? t.expected_fields->size : fields_.size()}"),
    "name-length-unchecked": ("fields.hh", "if (d.name.empty() || d.name.size() > format::max_term_bytes) { return \"name length\"; }", ""),
    "role-unchecked": ("fields.hh", "if (std::uint8_t(d.role) > 1) { return \"role\"; }", ""),
    "postings-unchecked": ("fields.hh", "if (std::uint8_t(d.postings) > 2) { return \"postings\"; }", ""),
    "key-field-options-unchecked": ("fields.hh", "if (d.role == field_role::key && (d.postings != field_postings::docs || d.norms)) { return \"key field options\"; }", ""),
    "key-field-in-keyless-index": ("index.hh", "(m_.key_prefix.empty() || fields_.key_field())", "(fields_.key_field())"),
    "second-key-field": ("index.hh", "(m_.key_prefix.empty() || fields_.key_field())", "(m_.key_prefix.empty())"),
    "conflicting-declaration-accepted": ("index.hh", "if (fields_[*id] != d) { return refuse(); }", ""),
    "identical-declaration-refused": ("index.hh", "if (fields_[*id] != d) { return refuse(); }", "if (true) { return refuse(); }"),
    "rollback-keeps-declarations": ("index.hh", "case undo_step::fields: fields_.truncate(it->a); break;", "case undo_step::fields: break;"),
    "commit-forgets-the-file": ("index.hh", "			m_.fields = *fields_ref;\n", ""),
    "torn-tail-appended-over": ("index.hh", "fields_clean_ = clean_fields_tail();", "fields_clean_ = true;"),
    "view-keeps-old-table": ("index.hh", "if (fields_.size() != fields_before) { ++fields_epoch_; }", ""),
    "digest-ignores-fields": ("index.hh", "for (std::size_t id = 0; id < table.size(); ++id) {  // the field table, in id order", "for (std::size_t id = 0; id < 0; ++id) {"),
    "check-skips-the-file": ("index.hh", "if (m_.fields.generation) {  // the file's prefix", "if (false) {  // the file's prefix"),
    "decoder-skips-block-crc": ("fields.hh", "if (last != ::kronuz::crc32c::crc32c(start, p - start)) { bad(\"checksum\"); }", ""),
    "decoder-allows-a-name-twice": ("fields.hh", "if (out.find(d.name)) { bad(\"a name twice\"); }", ""),
    "decoder-allows-two-key-fields": ("fields.hh", "if (d.role == field_role::key && out.key_field()) { bad(\"two key fields\"); }", ""),
    "decoder-allows-an-empty-block": ("fields.hh", "|| count == 0) { bad(\"block count\"); }", ") { bad(\"block count\"); }"),
    "decoder-allows-trailing-bytes": ("fields.hh", "	while (p != end) {", "	while (end - p >= 5) {"),
}


def run(cmd, cwd):
    return subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True)


def main():
    build = os.path.abspath(sys.argv[1])
    names = sys.argv[2:] or list(MUTANTS)
    out_root = os.path.join(ROOT, ".scratch", "mutants")
    commands = subprocess.check_output(["ninja", "-C", build, "-t", "commands", "fields_test"], text=True).splitlines()
    compile_cmd = [c for c in commands if " -c " in c and "test/fields_test.cc" in c][-1]
    link_cmd = [c for c in commands if " -o fields_test" in c][-1]
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
        obj = os.path.join(out, "m.o")
        exe = os.path.join(out, "fields_test")
        cc = compile_cmd.replace(ROOT + "/include", os.path.join(out, "include")).replace("CMakeFiles/fields_test.dir/test/fields_test.cc.o", obj)
        ld = link_cmd.replace("CMakeFiles/fields_test.dir/test/fields_test.cc.o", obj).replace(" -o fields_test", " -o " + exe)
        if run(cc, build).returncode != 0:
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
