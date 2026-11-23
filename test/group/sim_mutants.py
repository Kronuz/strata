#!/usr/bin/env python3
"""Mutants for strata::group's simulator rows (quest group-heal A5), on mutants.py's runner.

Each breaks one rule a row must catch. Usage:
  test/group/sim_mutants.py BUILD_DIR NAME [--target=TEST] [TEST ARGS...]
  test/group/sim_mutants.py BUILD_DIR --list
TEST defaults to group_sim_test. Exits as mutants.py does: 0 when the test passes (the mutant
survives), the test's status when it fails, 127 when the mutant does not apply or build.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import mutants  # noqa: E402

GROUP, FILES, APP = mutants.GROUP, mutants.FILES, mutants.APP

SIM = {
    # Apply skips one committed entry (index 12): marked applied and answered, never run.
    "apply-skips-entry": [(GROUP,
        "\t\tconst entry view{e.index, e.term, e.kind, e.payload};\n\t\tif (phase_ == phase::effects) {\n",
        "\t\tconst entry view{e.index, e.term, e.kind, e.payload};\n"
        "\t\tif (e.index == 12 && e.kind == cs::EntryKind::Command) {\n"
        "\t\t\tapplied_term_ = e.term;\n\t\t\tapplied_.store(e.index, std::memory_order_release);\n"
        "\t\t\tanswer_waiter(e.index, e.term, verdict{});\n\t\t\twake_readers(e.index);\n\t\t\treturn step::done;\n\t\t}\n"
        "\t\tif (phase_ == phase::effects) {\n")],
    # Recovery ignores the bundle's file-set table: every file under the prefix stays at its size
    # on disk, none truncated or deleted (D16 recovery step 2).
    "recovery-as-found": [
        (FILES, "\t\t\tif (size > f.length) {\n", "\t\t\tif (false) {\n"),
        (FILES, "\t\t\tif (!named.count(name)) { io_->remove(path(name)); }\n", "\t\t\t(void)name;\n"),
        (FILES, "\t\tlive_ = named;\n",
                "\t\tlive_ = named;\n\t\tfor (const auto& name : names) {\n"
                "\t\t\tif (auto file = open_named(name)) { live_[name].length = file->size(); }\n\t\t}\n")],
    # A leader applies an abandoned chunk of a lower term: an upload outlives its term (D12).
    "apply-abandoned-chunk": [
        (APP, "\t\t\tif (u.term < e.term) { files.retire(staging_name(id), e.index); }\n",
              "\t\t\tif (u.term < e.term && false) { files.retire(staging_name(id), e.index); }\n"),
        (APP, "\t\tstd::erase_if(s.uploads, [&](const auto& u) { return u.second.term < e.term; });\n",
              "\t\tfor (auto& u : s.uploads) { u.second.term = std::max(u.second.term, e.term); }\n")],
    # read_applied without the Driver's quorum probe (mutants.py's).
    "read-skips-probe": mutants.MUTANTS["read-skips-probe"],
}

if __name__ == "__main__":
    mutants.MUTANTS = SIM
    args = sys.argv[:3]
    rest = sys.argv[3:]
    if len(sys.argv) > 2 and sys.argv[2] != "--list" and not (rest and rest[0].startswith("--target=")):
        rest = ["--target=group_sim_test", *rest]
    sys.argv = args + rest
    sys.exit(mutants.main())
