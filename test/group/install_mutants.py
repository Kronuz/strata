#!/usr/bin/env python3
"""Mutants for strata::group's install (quest group-heal A1; docs/mechanisms/group-install.md).

Each mutant breaks one guard; install_test's case beside it must fail with it. Usage:
  test/group/install_mutants.py BUILD_DIR NAME     the named case's status with the mutant
  test/group/install_mutants.py BUILD_DIR --list
BUILD_DIR is a configured build of this checkout. The mutant is applied, group_install_test
rebuilt and run, and every file restored.
Exits 0 when the test passes (the mutant survives), the test's nonzero status when it fails, and
127 when the mutant does not apply or does not build, so a `campaign job submit --fails-without`
job passes only on a killed mutant.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
INSTALL = "include/strata/group/install.hh"
TARGET = "group_install_test"

# name: (the case that kills it, [(file, old, new), ...])
MUTANTS = {
    # The install moves strata alone: the set's files never reach the learner (A1).
    "skip-file-set": ("values", [(INSTALL,
        "\tfor (const auto& f : p.files) { plans.push_back({&f, 'a', std::string(d::files_prefix) + f.name, {}, {}, 0, 0}); }\n",
        "")]),
    # Held bytes are kept without their CRC32C: a damaged file the learner holds is installed.
    "keep-unchecked": ("damaged", [(INSTALL,
        "\t\tif (pl.crc.value() == pl.file->crc) { continue; }\n",
        "\t\tif (true) { continue; }\n")]),
    # A window writes the chunks after a failed one: the staged file holds a gap, and the retry
    # fetches it whole instead of only what is lacking.
    "write-past-failure": ("retry", [(INSTALL,
        "\t\t\t\tif (s.error) { throw install_failed(\"strata::group: an install fetch failed: \" + s.error.message(), s.error); }\n",
        "\t\t\t\tif (s.error) { failed = s.error; continue; }\n"),
        (INSTALL,
        "\t\t\tco_await d::window_awaiter{&fetch, &slots};\n",
        "\t\t\tco_await d::window_awaiter{&fetch, &slots};\n\t\t\tstd::error_code failed;\n"),
        (INSTALL,
        "\t\t\t\t++counts.chunks;\n\t\t\t}\n",
        "\t\t\t\t++counts.chunks;\n\t\t\t}\n\t\t\tif (failed) { throw install_failed(\"strata::group: an install fetch failed\", failed); }\n")]),
    # The marker names the pin before the staged files are durable: a power loss keeps the
    # marker and loses their bytes, and activation installs them.
    "marker-before-sync": ("crash", [(INSTALL,
        "\tfor (auto& pl : plans) {\n\t\tpl.f->sync();\n\t\tpl.f.reset();\n\t}\n\tdir.sync_directory();\n",
        "\tfor (auto& pl : plans) { pl.f.reset(); }\n")]),
    # The staged manifest log and its segments go in one directory epoch: a power loss can keep
    # the log and lose a segment, and the repeated activation installs from a partial checkpoint.
    "holder-unordered": ("crash", [(INSTALL,
        "\t\tdir.remove(std::string(d::strata_prefix) + p.strata.front().name);\n\t\tdir.sync_directory();\n",
        "")]),
    # Recovery does not finish an activation consensus persisted: P_b > P_H fences the replica.
    "no-resume": ("crash", [(INSTALL,
        "\tif (p && p->base == restored) {\n",
        "\tif (false) {\n")]),
    # A pin may name any file: the learner writes where the source says.
    "pin-names-unchecked": ("values", [(INSTALL,
        "\t\t\tif (!install_detail::strata_name(f.name) || !seen.insert(f.name).second) {",
        "\t\t\tif (!seen.insert(f.name).second) {")]),
    # The source serves past a file's pinned length.
    "serve-past-length": ("values", [(INSTALL,
        "\tif (!offset || !f || *offset >= f->length) { return std::nullopt; }\n",
        "\tif (!offset || !f) { return std::nullopt; }\n")]),
}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 127
    build, name = pathlib.Path(sys.argv[1]), sys.argv[2]
    if name == "--list":
        print("\n".join(f"{n} ({c})" for n, (c, _) in MUTANTS.items()))
        return 0
    if name not in MUTANTS:
        print(f"no mutant {name!r}", file=sys.stderr)
        return 127
    case, edits = MUTANTS[name]
    saved = {}
    try:
        for path, old, new in edits:
            f = ROOT / path
            saved.setdefault(f, f.read_text())
            text = f.read_text()
            if text.count(old) != 1:
                print(f"mutant {name}: {path} does not hold its text once", file=sys.stderr)
                return 127
            f.write_text(text.replace(old, new))
        built = subprocess.run(["cmake", "--build", str(build), "--target", TARGET, "-j", "4"], capture_output=True, text=True, check=False)
        if built.returncode != 0:
            print(built.stdout[-4000:] + built.stderr[-4000:], file=sys.stderr)
            return 127
        run = subprocess.run([str(build / TARGET), case], check=False)
        print(f"mutant {name}: {case} {'fails (killed)' if run.returncode else 'passes (survives)'}")
        return run.returncode
    finally:
        for f, text in saved.items():
            f.write_text(text)
        subprocess.run(["cmake", "--build", str(build), "--target", TARGET, "-j", "4"], capture_output=True, check=False)


if __name__ == "__main__":
    sys.exit(main())
