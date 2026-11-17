#!/usr/bin/env python3
"""Mutants for strata::group's lone voter (quest group-core A1).

Each mutant breaks one rule of cluster-release D3, D12, D16 or D18; group_lone_voter_test must
fail with it. Usage:
  test/group/mutants.py BUILD_DIR NAME [TEST ARGS...]   one mutant: exits with the test's status
  test/group/mutants.py BUILD_DIR --list
BUILD_DIR is a configured build of this checkout. The mutant is applied, the test rebuilt and
run, and every file restored. Exits 0 when the test passes (the mutant survives), the test's
nonzero status when it fails, and 127 when the mutant does not apply or does not build, so a
`campaign job submit --fails-without` job passes only on a killed mutant.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
GROUP = "include/strata/group/group.hh"
FILES = "include/strata/group/file_set.hh"
APP = "test/group/blob_app.hh"

# name: [(file, old, new), ...]
MUTANTS = {
    # Recovery replays the file set only above P_H, as strata does (D16's a1-literal).
    "replay-from-ph": [(GROUP,
        "\t\tif (phase_ == phase::effects) {\n",
        "\t\tif (phase_ == phase::effects && files::position{e.index, 1} <= head_) { phase_ = phase::strata; }\n"
        "\t\tif (phase_ == phase::effects) {\n")],
    # Apply unlinks a retired file at once, not after a bundle that omits it publishes (D12).
    "unlink-at-apply": [(FILES,
        "\t\tretired_.emplace_back(std::string(name), at);\n",
        "\t\tio_->remove(std::string(name));\n\t\t(void)at;\n")],
    # The bundle publishes before the files it names are synced (D16 steps 2 and 4).
    "publish-before-sync": [
        (GROUP, "\t\tfiles_.sync(b.files);\n\t\tix_->commit();\n", "\t\tix_->commit();\n"),
        (GROUP, "co_await on_chore([&] { files_.unlink_retired(b.position); });",
                "co_await on_chore([&] { files_.sync(b.files); files_.unlink_retired(b.position); });")],
    # The capture runs inside an entry's apply, between its file effects and its strata write.
    "capture-mid-entry": [
        (GROUP, "\t\t\tphase_ = phase::strata;\n\t\t}\n", "\t\t\tphase_ = phase::strata;\n\t\t\treturn step::more;\n\t\t}\n"),
        (GROUP, "co_await on_chore_step(capture_work(b));", "co_await on_chore_step(capture_work(b), true);")],
    # File effects run only when strata accepts: replay, where strata skips, loses them (D3).
    "effects-gated": [
        (GROUP, "\t\t\t\tapp_->effects(state_, view, files_);\n", "\t\t\t\t;\n"),
        (GROUP, "\t\tapp_->advance(state_, view);\n",
                "\t\tif (v.result == verdict::accepted) { app_->effects(state_, view, files_); }\n\t\tapp_->advance(state_, view);\n")],
    # The application state moves only when strata accepts (D3, N13).
    "appstate-gated": [(GROUP,
        "\t\tapp_->advance(state_, view);\n",
        "\t\tif (v.result == verdict::accepted) { app_->advance(state_, view); }\n")],
    # The capture skips its mark: strata's position may stay below P_b (D16 step 1).
    "no-mark": [(GROUP,
        "\t\t\tif (ix_->applied() < files::position{at, 1}) {\n",
        "\t\t\tif (false) {\n")],
    # strata commits after the bundle publishes, so a crash between leaves P_b > P_H (D16 step 3).
    "commit-after-publish": [
        (GROUP, "\t\tfiles_.sync(b.files);\n\t\tix_->commit();\n", "\t\tfiles_.sync(b.files);\n"),
        (GROUP, "co_await on_chore([&] { files_.unlink_retired(b.position); });",
                "co_await on_chore([&] { ix_->commit(); files_.unlink_retired(b.position); });")],
    # A file effect appends at the file's size: a retry after a partial push writes past it (D3).
    "append-on-retry": [(FILES,
        "\t\tfor (std::size_t done = 0; done < bytes.size();) { done += f->write_at(offset + done, bytes.substr(done)); }\n",
        "\t\tfor (std::size_t done = 0; done < bytes.size();) { done += f->write_at(f->size(), bytes.substr(done)); }\n")],
    # A failed checkpoint sync is retried and the bundle published (D16 step 2's sync-retry).
    "sync-retry": [(GROUP,
        "\t\tfiles_.sync(b.files);\n\t\tix_->commit();\n",
        "\t\ttry { files_.sync(b.files); } catch (...) { files_.sync(b.files); }\n\t\tix_->commit();\n")],
}


def main():
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
    saved = {}
    try:
        for path, old, new in MUTANTS[name]:
            f = ROOT / path
            text = saved.setdefault(f, f.read_text()) if f not in saved else f.read_text()
            if text.count(old) != 1:
                print(f"mutant {name}: {path} does not hold its text once", file=sys.stderr)
                return 127
            f.write_text(text.replace(old, new))
        built = subprocess.run(["cmake", "--build", str(build), "--target", "group_lone_voter_test", "-j", "4"], capture_output=True, text=True)
        if built.returncode != 0:
            print(built.stdout[-4000:] + built.stderr[-4000:], file=sys.stderr)
            return 127
        run = subprocess.run([str(build / "group_lone_voter_test"), *sys.argv[3:]])
        print(f"mutant {name}: the test {'fails (killed)' if run.returncode else 'passes (survives)'}")
        return run.returncode
    finally:
        for f, text in saved.items():
            f.write_text(text)
        subprocess.run(["cmake", "--build", str(build), "--target", "group_lone_voter_test", "-j", "4"], capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
