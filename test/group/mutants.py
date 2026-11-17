#!/usr/bin/env python3
"""Mutants for strata::group's lone voter (quest group-core A1).

Each mutant breaks one rule of cluster-release D3, D12, D16 or D18; group_lone_voter_test must
fail with it. Usage:
  test/group/mutants.py BUILD_DIR NAME [--target=TEST] [TEST ARGS...]   one mutant: the test's status
  test/group/mutants.py BUILD_DIR --list
BUILD_DIR is a configured build of this checkout; TEST defaults to group_lone_voter_test. The mutant is applied, the test rebuilt and
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
ANSWER = "include/strata/group/answer.hh"
SUBMIT = "include/strata/group/submit.hh"

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
    # The close leaves a waiter whose placement never applied here linked (journal group_closed).
    "close-keeps-waiters": [(GROUP,
        "\t\t\tfail_waiters(std::make_exception_ptr(cs::closed{}));\n",
        "")],
    # A checkpoint that can never get its admission waits for it (hunt: default limits).
    "admission-waits": [(GROUP,
        "\t\tif (cs::checkpoint_detail::maximum_size(config_.options.limits) + cap > config_.options.admission.replacement_pool.logical_bytes) {\n",
        "\t\tif (false) {\n")],
    # `quorum` answers at ProposalPlaced, before apply (A3).
    "quorum-at-placement": [(GROUP,
        "\t\tif (placed) { placed(p.placement); }\n\t\tco_await p;\n",
        "\t\tif (placed) { placed(p.placement); }\n\t\tp.result = verdict{};\n")],
    # `quorum` answers as commit would, without apply's verdict (A3's acknowledge-at-commit).
    "ack-at-commit": [(GROUP,
        "\t\t\tresult = o.v;\n",
        "\t\t\tresult = verdict{};\n")],
    # The leader prepares from its index without the read's ready point (A3).
    "prepare-before-ready": [(SUBMIT,
        "p.emplace(co_await g_->read_applied([&](typename G::index_type& ix) {",
        "p.emplace(co_await [&](auto f) -> task::task<prepared> { co_return f(g_->strata()); }([&](typename G::index_type& ix) {")],
    # The predictor never hears the leader's own commands not applied yet (D106's pending).
    "pending-ignored": [(SUBMIT,
        "auto out = prepare(ix, pending(term));",
        "auto out = prepare(ix, std::vector<std::string>{});")],
    # No bound on placements (A3's lift-the-bound).
    "lift-the-bound": [(ANSWER,
        "return placements < max_placements ? next_step::resubmit : next_step::unavailable;",
        "return next_step::resubmit;")],
    # The group drops consensus's SnapshotNeeded: a replica left behind goes unnamed (D109).
    "install-silent": [(GROUP,
        "\tif (auto* m = owner->find(id)) { m->install_needed(s.peer); }\n",
        "\t(void)id;\n\t(void)s;\n")],
    # A2's checkpoints ungated: one compacts past a replica cut off (D109; run with 14 schedules).
    "checkpoint-ungated": [("test/group/voters_test.cc",
        "(!gated || w.leaves_none_behind(",
        "(true || w.leaves_none_behind(")],
    # A deposed proposer answers 503 instead of forwarding to the leader (A3).
    "no-forward": [(SUBMIT,
        "\t\t\t\tif (forwarded) {",
        "\t\t\t\tif (true) {")],
    # An invalid document is submitted again like a refused merge (A3).
    "resubmit-invalid": [(ANSWER,
        "\tcase verdict::version_conflict:\n",
        "\tcase verdict::invalid: return placements < max_placements ? next_step::resubmit : next_step::unavailable;\n\tcase verdict::version_conflict:\n")],
    # read_applied reads this replica's state without the Driver's quorum probe (A4).
    "read-skips-probe": [(GROUP,
        "\t\tconst auto ready = co_await loop_->driver().read(config_.id, std::move(token));\n\t\tco_await applied_at(ready.index);\n",
        "\t\t(void)token;\n")],
}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 127
    build, name, args = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
    target = "group_lone_voter_test"
    if args and args[0].startswith("--target="):
        target, args = args[0].split("=", 1)[1], args[1:]
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
        built = subprocess.run(["cmake", "--build", str(build), "--target", target, "-j", "4"], capture_output=True, text=True)
        if built.returncode != 0:
            print(built.stdout[-4000:] + built.stderr[-4000:], file=sys.stderr)
            return 127
        run = subprocess.run([str(build / target), *args])
        print(f"mutant {name}: the test {'fails (killed)' if run.returncode else 'passes (survives)'}")
        return run.returncode
    finally:
        for f, text in saved.items():
            f.write_text(text)
        subprocess.run(["cmake", "--build", str(build), "--target", target, "-j", "4"], capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
