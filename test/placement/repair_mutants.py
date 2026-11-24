#!/usr/bin/env python3
"""Mutants for placement repair (quest placement, task repair).

Each mutant removes one mechanism of include/strata/placement/repair.hh (named beside it); its
test must fail with it. Usage:
  test/placement/repair_mutants.py BUILD_DIR NAME     one mutant: its test's status
  test/placement/repair_mutants.py BUILD_DIR --list
BUILD_DIR is a configured build of this checkout. The mutant is applied, its test rebuilt and run,
and every file restored. Exits 0 when the test passes (the mutant survives), the test's nonzero
status when it fails, and 127 when the mutant does not apply or does not build, so a
`campaign job submit --fails-without` job passes only on a killed mutant.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
REPAIR = "include/strata/placement/repair.hh"
PLAN, GROUP = "placement_repair_plan_test", "placement_repair_group_test"

# name: (test, test arguments, [(file, old, new), ...])
MUTANTS = {
    # Leader balance transfers while a multi-entry command is open: the new term aborts it (A4).
    "transfer-anyway": (GROUP, ["defer", "1"], [(REPAIR, "\t\tif (r.open_command) { return wait(", "\t\tif (false) { return wait(")]),
    # The same, against the plan's model.
    "transfer-anyway-plan": (PLAN, ["0"], [(REPAIR, "\t\tif (r.open_command) { return wait(", "\t\tif (false) { return wait(")]),
    # A node down is lost at once: members move before replace_after.
    "replace-now": (GROUP, ["replace", "1"], [(REPAIR, "s.lost = now >= d->second && now - d->second >= after;", "s.lost = true;")]),
    # The root's setting is ignored: the default 300 s holds after a runtime change.
    "setting-ignored": (PLAN, ["0"], [(REPAIR, "return s ? decode_settings(s->bytes) : settings_record{};", "return settings_record{};")]),
    # A leaving node keeps its members: a drain moves nothing.
    "drain-ignored": (PLAN, ["0"], [(REPAIR, "s.lost = s.lost || s.state != node_state::active;", "")]),
    # A promotion never names the member it replaces: no 3 + 1 or 2 + 2 window, no growth past a witness.
    "no-leaving": (PLAN, ["0"], [(REPAIR, "\t\t\tbase.leaving = leaving;\n", "")]),
    # A change may leave fewer up voters than a quorum.
    "no-quorum-guard": (PLAN, ["0"], [(REPAIR, "return live >= cs::voter_count(n) / 2 + 1;", "return true;")]),
    # The root's record does not follow the group: loads and preferred leaders drift.
    "record-ignored": (PLAN, ["0"], [(REPAIR, "\t\t\tif (!same_record(want, *recorded)) {", "\t\t\tif (false) {")]),
    # A change retires the leader without moving leadership first.
    "leader-not-first": (PLAN, ["0"], [(REPAIR, "\t\tif (leaving != r.leader) { return std::nullopt; }", "\t\treturn std::nullopt;")]),
    # A lost member no removal drops stays: a 2 + witness group with a lost member waits at a lower rf.
    "no-window-replace": (PLAN, ["0"], [(REPAIR, "\t\tfor (const auto* bad : {&bad_wit, &bad_data}) {", "\t\tfor (const auto* bad : std::initializer_list<const std::vector<const cs::Member*>*>{}) {")]),
    # A shrink removes a data voter before the witness: 1 data voter beside a witness.
    "shrink-order": (PLAN, ["0"], [(REPAIR, "\t\tif (wit.size() > t.witnesses) {\n\t\t\tif (auto next = without(wit.front()->incarnation); valid(next)) { return change(std::move(next)); }\n\t\t}\n",
                                     "")]),
    # The record names a member on a node the root removed: the root refuses it every pass.
    "record-unknown": (PLAN, ["0"], [(REPAIR, "if (status(x.node)) { s.members.push_back(", "if (true) { s.members.push_back(")]),
    # A pass does not count the learners it adds: one node takes them all.
    "join-uncounted": (PLAN, ["0"], [(REPAIR, "\t\t\tif (const auto j = nodes.find(next.joining); j != nodes.end()) { ++j->second.groups; }\n", "")]),
    # A pass does not count a record's learner: the next shard ignores it.
    "record-uncounted": (PLAN, ["0"], [(REPAIR, "\t\t\tif (next.kind == step::record) { count(nodes, s, next.recorded); }\n", "")]),
    # A node down but not lost takes a new learner.
    "candidate-down": (PLAN, ["0"], [(REPAIR, "if (s.lost || s.down || on_node(id)) { continue; }", "if (s.lost || on_node(id)) { continue; }")]),
    # A new root leader's empty table counts every node up.
    "cold-table-up": (PLAN, ["0"], [(REPAIR, "\t\ts.down = s.down || now < l.full_at;\n", "")]),
}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    build, name = pathlib.Path(sys.argv[1]), sys.argv[2]
    if name == "--list":
        print("\n".join(MUTANTS))
        return 0
    test, args, edits = MUTANTS[name]
    saved = {}
    try:
        for path, old, new in edits:
            f = ROOT / path
            text = saved.setdefault(f, f.read_text())
            current = f.read_text()
            if current.count(old) != 1:
                print(f"{name}: {path} does not hold the guard once", file=sys.stderr)
                return 127
            f.write_text(current.replace(old, new))
        if subprocess.run(["cmake", "--build", str(build), "--target", test, "-j4"]).returncode:
            return 127
        status = subprocess.run([str(build / test), *args]).returncode
        print(f"{name}: {test} exit {status}")
        return status
    finally:
        for f, text in saved.items():
            f.write_text(text)
        subprocess.run(["cmake", "--build", str(build), "--target", test, "-j4"], stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    sys.exit(main())
