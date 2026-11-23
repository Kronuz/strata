#!/usr/bin/env python3
"""Mutants for strata::group's repair (quest group-heal A3; docs/mechanisms/group-install.md, Repair).

Each mutant breaks one mechanism; repair_test's case beside it must fail with it. Usage:
  [CONSENSUS_DIR=DIR] test/group/repair_mutants.py BUILD_DIR NAME     the named case's status with the mutant
  test/group/repair_mutants.py BUILD_DIR --list
BUILD_DIR is configured here when it holds no build (CONSENSUS_DIR overrides the consensus pin).
group_repair_test is built whole first, then the mutant applied, rebuilt and run, and every file
restored. Exits 0 when the case passes (the mutant survives), the case's nonzero status when it
fails, and 127 when the build, the mutant or its rebuild fails, so a `campaign job submit
--fails-without` job passes only on a killed mutant.
"""
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
GROUP = "include/strata/group/group.hh"
INSTALL = "include/strata/group/install.hh"
TARGET = "group_repair_test"

# name: (the case that kills it, [(file, old, new), ...])
MUTANTS = {
    # A damaged index fails the open instead of making way for a deferred one.
    "open-rethrows": ("manifest", [(GROUP,
        "\t\t\tif (!damage(e)) { throw; }\n",
        "\t\t\tthrow;\n")]),
    # The damaged index's names go with no marker: the reopen holds a deferred index below the
    # bundle and nothing that says it is damaged.
    "unmarked": ("manifest", [(GROUP,
        "\t\t\tmark_damaged(directory, error_text(e));\n",
        "")]),
    # The names go before the marker is durable: a power cut between them reopens unmarked.
    "quarantine-first": ("crash-manifest-dir", [(GROUP,
        "\t\t\tmark_damaged(directory, error_text(e));\n\t\t\tquarantine_strata(directory);\n",
        "\t\t\tquarantine_strata(directory);\n\t\t\tmark_damaged(directory, error_text(e));\n")]),
    # A set file that fails its check at recovery fails the open instead of marking damage.
    "set-damage-fences": ("file", [(GROUP,
        "\t\t\tmark_damaged(files_.io(), e.what());\n\t\t\treturn adopt_damaged(b, e.what());\n",
        "\t\t\tthrow;\n")]),
    # Delivered ranges are dropped, never acknowledged: consensus's applied index stays at the
    # replica's, and it refuses the request (Busy) while a range is outstanding.
    "no-ack": ("refusal", [(GROUP,
        "\t\tif (const auto through = w->delivered()) { notify_applied(through); }\n",
        "")]),
    # A damaged replica leaves its waiters and new calls unanswered.
    "waiters-unanswered": ("refusal", [(GROUP,
        "\t\tfail_waiters(std::make_exception_ptr(replica_damaged(reason)), true);\n",
        ""),
        (GROUP,
        "\t\tusing R = std::invoke_result_t<F&, index_type&>;\n\t\tif (damaged_.load()) { throw replica_damaged(\"strata::group: the replica is damaged and reinstalls\"); }\n",
        "\t\tusing R = std::invoke_result_t<F&, index_type&>;\n"),
        (GROUP,
        "\t\tif (closing_.load()) { throw cs::closed{}; }\n\t\tif (damaged_.load()) { throw replica_damaged(\"strata::group: the replica is damaged and reinstalls\"); }\n",
        "\t\tif (closing_.load()) { throw cs::closed{}; }\n")]),
    # Damage met at apply fences the replica (consensus's application_failed) instead.
    "apply-damage-fences": ("apply", [(GROUP,
        "\t\t\t\tif (damage(e) && become_damaged(error_text(e))) {\n",
        "\t\t\t\tif (false) {\n")]),
    # Damage met at a read passes to the reader and leaves the replica serving.
    "read-damage-passes": ("read", [(GROUP,
        "\t\t\t\tif (damage(e) && become_damaged(error_text(e))) { throw replica_damaged(error_text(e)); }\n",
        "")]),
    # The activation moves staged files in unchecked: bytes that rotted since the stage are served.
    "activate-unchecked": ("staged", [(INSTALL,
        "\tif (auto bad = d::staged_damage(dir, p, strata_staged)) {\n",
        "\tif (std::optional<std::string> bad; false) {\n")]),
    # The activation keeps the marker: the replica stays damaged after its image.
    "marker-kept": ("manifest", [(INSTALL,
        "\tclear_damaged(dir);  // the replica holds the pin's bytes now (repair.hh)\n",
        "")]),
    # A damaged leader keeps leading: it can neither serve nor be sent an image.
    "leader-keeps": ("leader", [(GROUP,
        "\t\t\tif (r.reason == cs::RejectReason::LeaderMustTransfer) { hand_leadership_away(); }\n",
        "")]),
    # A refused request is never asked again: a replica the leader answered Current stays damaged.
    "no-retry": ("staged", [(GROUP,
        "\t\tself->hand_off();\n\t\tself->begin_repair();\n",
        "\t\tself->hand_off();\n")]),
    # The pin ships a segment unchecked: a source whose mapped segment rotted on disk keeps
    # leading, and a learner's activation of those bytes fails (source at rest).
    "pin-unchecked-footer": ("source", [(INSTALL,
        "\t\t\tif (footer) { install_detail::check_footer(*f, *length, *footer); }\n",
        "")]),
    # A refused activation reports the replica's first damage: the staged file's refusal is lost.
    "reason-kept": ("staged", [(GROUP,
        "\t\t\tif (marked_damaged(files_.io())) { return adopt_damaged(b, e.what()); }\n",
        "\t\t\tif (marked_damaged(files_.io())) { return adopt_damaged(b); }\n")]),
    # A damaged leader hands off to any follower the host names, a damaged one too: two damaged
    # replicas pass leadership between them and nothing reinstalls.
    "handoff-any": ("source", [(GROUP,
        "\t\t\t\ttarget = loop_->applied_past(id_, std::max<cs::Index>(s->committed, 1));\n",
        "\t\t\t\ttarget = loop_->applied_past(id_, 0);\n")]),
    # A sound replica stages an image consensus refuses as caught up: its stage stays behind.
    "stage-caught-up": ("crash-manifest-dir", [(GROUP,
        "\t\tif (!damaged_.load() && boundary.index <= applied_.load()) {\n",
        "\t\tif (false) {\n")]),
    # A flight's end names its peer still unless it installed or caught up: a leader deposed
    # mid-flight names the peer, and keeps its install state, for good.
    "needed-kept": ("deposed", [(GROUP,
        "\tvoid install_released(cs::NodeId peer, cs::SnapshotKey key, cs::SnapshotReleaseReason) override {\n",
        "\tvoid install_released(cs::NodeId peer, cs::SnapshotKey key, cs::SnapshotReleaseReason reason) override {\n"),
        (GROUP,
        "\t\t// is a new flight, which names the peer again.\n\t\t{\n",
        "\t\t// is a new flight, which names the peer again.\n\t\tif (reason == cs::SnapshotReleaseReason::Installed || reason == cs::SnapshotReleaseReason::CaughtUp) {\n")]),
}


def build(path):
    return subprocess.run(["cmake", "--build", str(path), "--target", TARGET, "-j", "4"], capture_output=True, text=True, check=False)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 127
    out, name = pathlib.Path(sys.argv[1]), sys.argv[2]
    if name == "--list":
        print("\n".join(f"{n} ({c})" for n, (c, _) in MUTANTS.items()))
        return 0
    if name not in MUTANTS:
        print(f"no mutant {name!r}", file=sys.stderr)
        return 127
    path = out / "strata"
    if not (path / "build.ninja").exists():
        cmd = ["cmake", "-G", "Ninja", "-S", str(ROOT), "-B", str(path), "-DCMAKE_BUILD_TYPE=Release"]
        if os.environ.get("CONSENSUS_DIR"):
            cmd.append("-DFETCHCONTENT_SOURCE_DIR_CONSENSUS=" + os.environ["CONSENSUS_DIR"])
        if subprocess.run(cmd, capture_output=True, check=False).returncode != 0:
            return 127
    whole = build(path)
    if whole.returncode != 0:
        print(whole.stdout[-4000:] + whole.stderr[-4000:], file=sys.stderr)
        return 127
    case, edits = MUTANTS[name]
    saved = {}
    try:
        for file, old, new in edits:
            f = ROOT / file
            saved.setdefault(f, f.read_text())
            text = f.read_text()
            if text.count(old) != 1:
                print(f"mutant {name}: {file} does not hold its text once", file=sys.stderr)
                return 127
            f.write_text(text.replace(old, new))
        built = build(path)
        if built.returncode != 0:
            print(built.stdout[-4000:] + built.stderr[-4000:], file=sys.stderr)
            return 127
        run = subprocess.run([str(path / TARGET), case], check=False)
        print(f"mutant {name}: {case} {'fails (killed)' if run.returncode else 'passes (survives)'}")
        return run.returncode
    finally:
        for f, text in saved.items():
            f.write_text(text)
        build(path)


if __name__ == "__main__":
    sys.exit(main())
