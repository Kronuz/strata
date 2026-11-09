#!/usr/bin/env python3
"""Mutants for the manifest log's reader, appender and replay cost (manifest-log step 3).

Each mutant breaks one claim; manifest_log_test must fail with it. Usage:
  test/manifest_log_mutants.py BUILD_DIR   (a configured build of this checkout)
Restores every file it touches; exits 1 when a mutant survives or does not apply.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LOG = "include/strata/core/manifest_log.hh"
FIELDS = "include/strata/core/fields.hh"

MUTANTS = [
    ("an empty scan reads as a fresh state", LOG,
     '\tif (scan.records.empty()) { bad("no whole snapshot"); }  // D6.3\n',
     '\tif (scan.records.empty()) { return {}; }\n'),
    ("the snapshot's generation unchecked", LOG,
     'if (out.state.generation != g) { bad("snapshot generation differs from its log"); }',
     'if (false) { bad("snapshot generation differs from its log"); }'),
    ("the snapshot's UUID unchecked", LOG,
     'if (out.state.uuid != uuid) { bad("snapshot UUID differs from its blocks"); }',
     'if (false) { bad("snapshot UUID differs from its blocks"); }'),
    ("no bounds on read", LOG,
     'if (!log_within(out.state, out.end, out.edits, 2))',
     'if (false && !log_within(out.state, out.end, out.edits, 2))'),
    ("bounds on read at the writer's slack", LOG,
     'if (!log_within(out.state, out.end, out.edits, 2))',
     'if (!log_within(out.state, out.end, out.edits, 1) && out.edits > 300)'),
    ("a failure inside replay read as a torn tail", LOG,
     '\t\tapply(out.state, decode_edit(scan.records[i].bytes, r));\n',
     '\t\ttry { apply(out.state, decode_edit(scan.records[i].bytes, r)); } catch (const std::bad_alloc&) { out.dirty = true; return out; }\n'),
    ("damage read as a torn tail", LOG,
     'if (scan.tail == fr::Tail::damage) { throw log_damage(',
     'if (false) { throw log_damage('),
    ("damage past `to` not reported", LOG,
     'if (scan.tail == fr::Tail::damage) { out.damaged = g + scan.records.size(); }',
     ''),
    ("no truncate-back", LOG,
     '\t\t\tfile_.truncate(end_);\n\t\t\tfile_.sync_data();\n',
     ''),
    ("a truncate-back without its sync", LOG,
     '\t\t\tfile_.truncate(end_);\n\t\t\tfile_.sync_data();\n',
     '\t\t\tfile_.truncate(end_);\n'),
    ("two records in flight", LOG,
     'if (in_flight_) { throw std::logic_error("manifest log: an append is already in flight"); }',
     ''),
    ("the end moves before the barrier", LOG,
     '\t\tpending_ = emitter.position();\n\t\tin_flight_ = true;\n',
     '\t\tpending_ = end_ = emitter.position();\n\t\tin_flight_ = true;\n'),
    ("pieces not ending on a sector", LOG,
     'if (at + n < bytes.size()) { n -= (end_ + at + n) % 512; }',
     ''),
    ("a failed barrier leaves the appender usable", LOG,
     '\t\t} catch (...) {\n\t\t\tbroken_ = true;\n\t\t\tthrow;\n\t\t}\n\t\tend_ = pending_;',
     '\t\t} catch (...) {\n\t\t\tin_flight_ = false;\n\t\t\tthrow;\n\t\t}\n\t\tend_ = pending_;'),
    ("a failed truncate-back leaves the appender usable", LOG,
     '\t\t\tbroken_ = true;  // the caller\'s error stands',
     '\t\t\t(void)0;  // the caller\'s error stands'),
    ("declaration names scanned per declaration", LOG,
     'if (s.fields.find(d.name) || !names.insert(d.name).second) { bad("a name twice"); }',
     'if (s.fields.find(d.name) || !names.insert(d.name).second || std::count_if(e.fields.begin(), e.fields.end(), [&](const auto& x) { return x.name == d.name; }) > 1) { bad("a name twice"); }'),
    ("identities scanned per edit", LOG,
     '\tif (!added.empty()) {\n\t\tstd::set<std::array<std::uint8_t, 16>> leaving;',
     '\tif (!added.empty()) {\n\t\tfor (const auto& [id, seg] : s.segments) { if (!removed(id) && added.count(seg.identity)) { bad("segment identity twice"); } }\n\t\tstd::set<std::array<std::uint8_t, 16>> leaving;'),
    ("an added identity not checked against the state", LOG,
     'if (s.identities.count(id) && !leaving.count(id)) { bad("segment identity twice"); }',
     ''),
    ("the field table grows by one", FIELDS,
     '\t\troom(decls_);\n\t\troom(chain_);\n',
     '\t\tdecls_.reserve(decls_.size() + 1);\n\t\tchain_.reserve(chain_.size() + 1);\n'),
]


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    build = pathlib.Path(sys.argv[1]).resolve()
    test = build / "manifest_log_test"
    survived = []
    for name, path, old, new in MUTANTS:
        f = ROOT / path
        text = f.read_text()
        if text.count(old) != 1:
            print(f"DOES NOT APPLY: {name}")
            survived.append(name)
            continue
        f.write_text(text.replace(old, new))
        try:
            b = run(["cmake", "--build", str(build), "--target", "manifest_log_test", "-j", "4"])
            if b.returncode != 0:
                print(f"killed (build): {name}")
                continue
            t = run([str(test)])
            failed = [ln.split(":")[0] for ln in t.stdout.splitlines() if ln.endswith("FAILED") and not ln.startswith("manifest_log")]
            if t.returncode == 0:
                print(f"SURVIVED: {name}")
                survived.append(name)
            else:
                print(f"killed: {name} ({', '.join(failed) or 'exit ' + str(t.returncode)})")
        finally:
            f.write_text(text)
    run(["cmake", "--build", str(build), "--target", "manifest_log_test", "-j", "4"])
    print(f"{len(MUTANTS) - len(survived)} of {len(MUTANTS)} killed")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
