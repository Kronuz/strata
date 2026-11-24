#!/usr/bin/env python3
"""Mutants for the placement root (quest placement, task root).

Each mutant removes one guard of the records, the commands or placement (named beside it); its
test must fail with it. Usage:
  test/placement/root_mutants.py BUILD_DIR NAME     one mutant: its test's status
  test/placement/root_mutants.py BUILD_DIR --list
BUILD_DIR is a configured build of this checkout. The mutant is applied, its test rebuilt and run,
and every file restored. Exits 0 when the test passes (the mutant survives), the test's nonzero
status when it fails, and 127 when the mutant does not apply or does not build, so a
`campaign job submit --fails-without` job passes only on a killed mutant.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
RECORDS = "include/strata/placement/records.hh"
PLACEMENT = "include/strata/placement/placement.hh"
APP = "include/strata/placement/root.hh"
REC, PLACE, GROUP = "placement_root_records_test", "placement_root_placement_test", "placement_root_group_test"

# name: (test, [(file, old, new), ...])
MUTANTS = {
    # A namespace key keeps its raw `/`: siblings such as `twitter-a` sort inside a subtree.
    "slash-raw": (REC, [(RECORDS, "\tstd::replace(k.begin() + 1, k.end(), '/', '\\0');\n", "")]),
    # A decoder accepts bytes past its record, so a record has two encodings.
    "trailing-bytes": (REC, [(RECORDS, "\t\tif (left()) { fail(\"trailing bytes\"); }", "\t\tif (false) { fail(\"trailing bytes\"); }")]),
    # A decoder accepts an overlong integer, so a record has two encodings.
    "shortest-form": (REC, [(RECORDS, "if (p - in.data() > std::ptrdiff_t(at + 1) && p[-1] == 0) { fail(", "if (false) { fail(")]),
    # A count is not bounded by the bytes left: a hostile count allocates before it fails.
    "count-bound": (REC, [(RECORDS, "\t\tif (n > left() / least) { fail(\"count past the bytes left\"); }", "\t\t(void)least;")]),
    # A namespace's preferred leader may be a learner or a witness.
    "leader-voter": (REC, [(RECORDS, "if (s.members.front().role != role::voter) { r.fail(", "if (false) { r.fail(")]),
    # A node may be a member of one shard twice.
    "distinct-members": (REC, [(RECORDS, "if (s.members[i].node == s.members[k].node) { r.fail(", "if (false) { r.fail(")]),
    # Two shards of a namespace may share a group.
    "distinct-groups": (REC, [(RECORDS, "if (s.group == 0 || !groups.insert(s.group).second) { r.fail(", "if (s.group == 0) { r.fail(")]),
    # A load may hold more leaderships than groups.
    "leaders-le-groups": (REC, [(RECORDS, "if (l.leaders > l.groups) { r.fail(", "if (false) { r.fail(")]),
    # A command may erase a counter, so ids are reused (M1).
    "erase-counter": (REC, [(RECORDS, "} else if (!(node_of(w.key) || load_of(w.key) || path_of(w.key))) {",
                             "} else if (!(w.key == directory_key() || w.key == groups_key() || node_of(w.key) || load_of(w.key) || path_of(w.key))) {")]),
    # A command's put is stored without its key's decoder accepting the value.
    "put-unchecked": (REC, [(RECORDS, "if (!valid_record(w.key, w.value)) { r.fail(", "if (false) { r.fail(")]),
    # Apply turns a malformed command into no write, answered accepted rather than invalid.
    "corrupt-accepted": (GROUP, [(APP, "\t\t\tindex_type::operation o;\n\t\t\to.type = index_type::operation::delete_key;\n\t\t\tt.ops.push_back(std::move(o));\n\t\t\treturn t;",
                                  "\t\t\treturn std::nullopt;")]),
    # Apply drops the leader's expected versions: a stale command overwrites the winner.
    "no-expectations": (GROUP, [(APP, "\t\t\to.expected_version = w.expected;\n", "")]),
    # A node holding groups is removed.
    "remove-busy-node": (GROUP, [(APP, "if (l->record.groups != 0) { return detail::refuse(", "if (false) { return detail::refuse(")]),
    # A create of an existing path is not refused at prepare: it spends its placements.
    "create-existing": (GROUP, [(APP, "if (const auto cur = v.namespace_at(path)) { return detail::refuse(verdict::version_conflict, cur->version); }", "")]),
    # The leader prepares from the index alone, not its pending commands: concurrent writes conflict.
    "no-pending": (GROUP, [(APP, "\t\tfor (const auto& p : pending) {\n\t\t\tcommand c;", "\t\tfor (const auto& p : std::vector<std::string>{}) {\n\t\t\t(void)pending;\n\t\t\tcommand c;")]),
    # Placement takes nodes that are leaving or retired.
    "any-state": (GROUP, [(APP, "if (decode_node(v).state != node_state::active) { return; }", "")]),
    # A delete leaves its members' loads counted.
    "delete-keeps-load": (GROUP, [(APP, "\tdetail::load_changes(v, cur->record.shards, {}, c);\n", "")]),
    # A node update leaves the directory version where it was.
    "update-no-version": (GROUP, [(APP, "\tc.writes.push_back(detail::touch_directory(v));\n\tc.writes.push_back(detail::put(node_key(id), cur->version, encode(n)));",
                                   "\tc.writes.push_back(detail::put(node_key(id), cur->version, encode(n)));")]),
    # The preferred leader is chosen by groups like the others: leaderships drift past the mean.
    "leader-by-groups": (PLACE, [(PLACEMENT, "std::min_element(nodes.begin(), nodes.end(), by_leaders)", "std::min_element(nodes.begin(), nodes.end(), by_groups)")]),
    # Two data members run as a lasting two-voter group, with no witness (D7).
    "no-witness": (PLACE, [(PLACEMENT, "\tif (data == 2 && nodes.size() > 2) { out.push_back({nodes[2].node, role::witness}); }\n", "")]),
}


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 127
    build, name = pathlib.Path(sys.argv[1]), sys.argv[2]
    if name == "--list":
        print("\n".join(MUTANTS))
        return 0
    if name not in MUTANTS:
        print(f"no mutant {name!r}", file=sys.stderr)
        return 127
    target, edits = MUTANTS[name]
    saved: dict[pathlib.Path, str] = {}
    try:
        for path, old, new in edits:
            f = ROOT / path
            text = f.read_text()
            saved.setdefault(f, text)
            if text.count(old) != 1:
                print(f"mutant {name}: {path} does not hold its text once", file=sys.stderr)
                return 127
            f.write_text(text.replace(old, new))
        built = subprocess.run(["cmake", "--build", str(build), "--target", target, "-j", "2"], capture_output=True, text=True)
        if built.returncode != 0:
            print(built.stdout[-4000:] + built.stderr[-4000:], file=sys.stderr)
            return 127
        run = subprocess.run([str(build / target)])
        print(f"mutant {name}: {target} {'fails (killed)' if run.returncode else 'passes (survives)'}")
        return run.returncode
    finally:
        for f, text in saved.items():
            f.write_text(text)
        subprocess.run(["cmake", "--build", str(build), "--target", target, "-j", "2"], capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
