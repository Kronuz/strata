#!/usr/bin/env python3
"""Mutants for strata::group (quests group-core, group-heal, group-catalog).

Each mutant breaks one rule of the design or a criterion (named beside it); its test must fail
with it. Usage:
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
BUNDLE = "include/strata/group/bundle.hh"
INDEX = "include/strata/core/index.hh"
CATALOG = "include/strata/core/catalog.hh"
FOOTPRINT = "test/group/footprint_test.cc"
WITNESS = "include/strata/group/witness.hh"

# name: [(file, old, new), ...]
MUTANTS = {
    # A settle runs with an isolated node still cut off: a cut upload's isolation in a
    # fault-free stretch outlives the stretch (corruption row, file seed 3). voters_test's
    # settle_heals fails.
    "settle-keeps-isolation": [("test/group/world.hh",
        "\t\tisolated.clear();\n\t\tfor (auto& [id, x] : nodes) {\n\t\t\tif (!x->up()) { boot(*x); }\n",
        "\t\tfor (auto& [id, x] : nodes) {\n\t\t\tif (!x->up()) { boot(*x); }\n")],
    # Recovery replays the file set only above P_H, as strata does (D16's a1-literal).
    "replay-from-ph": [(GROUP,
        "\t\tif (phase_ == phase::effects) {\n",
        "\t\tif (phase_ == phase::effects && files::position{e.index, 1} <= head_) { phase_ = phase::strata; }\n"
        "\t\tif (phase_ == phase::effects) {\n")],
    # Apply unlinks a retired file at once, not after a bundle that omits it publishes (D12).
    "unlink-at-apply": [(FILES,
        "\t\tretired_.emplace_back(std::string(name), at);\n",
        "\t\tio_->remove(path(name));\n\t\t(void)at;\n")],
    # The bundle publishes before the files it names are synced (D16 steps 2 and 4).
    "publish-before-sync": [
        (GROUP, "\t\tfiles_.sync(b.files);\n\t\tix_->commit();\n", "\t\tix_->commit();\n"),
        (GROUP, "\t\t\tfiles_.unlink_retired(unlink_through(b.position));\n",
                "\t\t\tfiles_.sync(b.files);\n\t\t\tfiles_.unlink_retired(unlink_through(b.position));\n")],
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
        (GROUP, "\t\t\tfiles_.unlink_retired(unlink_through(b.position));\n",
                "\t\t\tix_->commit();\n\t\t\tfiles_.unlink_retired(unlink_through(b.position));\n")],
    # A file effect appends at the file's size: a retry after a partial push writes past it (D3).
    "append-on-retry": [(FILES,
        "\t\tfor (std::size_t done = 0; done < bytes.size();) { done += f->write_at(offset + done, bytes.substr(done)); }\n",
        "\t\tfor (std::size_t done = 0; done < bytes.size();) { done += f->write_at(f->size(), bytes.substr(done)); }\n")],
    # A failed checkpoint sync is retried and the bundle published (D16 step 2's sync-retry).
    "sync-retry": [(GROUP,
        "\t\tfiles_.sync(b.files);\n\t\tix_->commit();\n",
        "\t\ttry { files_.sync(b.files); } catch (...) { files_.sync(b.files); }\n\t\tix_->commit();\n")],
    # Recovery takes a sealed file at its length without its CRC32C: a damaged rotated volume
    # recovers silently (N4; --target=group_file_set_test).
    "sealed-unchecked": [(FILES,
        "\t\t\tif (f.crc && crc32c_of(*file, f.length) != *f.crc) {",
        "\t\t\tif (false) {")],
    # The close leaves a waiter whose placement never applied here linked (journal group_closed).
    "close-keeps-waiters": [(GROUP,
        "\t\tfail_waiters(std::make_exception_ptr(cs::closed{}));\n",
        "")],
    # A checkpoint that can never get its admission waits for it (hunt: default limits).
    "admission-waits": [(GROUP,
        "\t\tif (log_bound_ + cap > replacement_pool_) {\n",
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
    # The group drops consensus's SnapshotNeeded: no pin, so a replica behind the leader's
    # checkpoint never installs and the ungated schedules do not settle (group-heal A2).
    "install-silent": [(GROUP,
        "\tif (auto* m = owner->find(id)) { m->install_needed(s.peer, s.key); }\n",
        "\t(void)id;\n\t(void)s;\n")],
    # A witness's Sink holds a range below its promotion: the learner it joins as never applies
    # it, and its promotion never commits (group-heal A4; group_witness_test join).
    "witness-holds-range": [(WITNESS,
        "\t\tloop_->driver().applied(id_, cs::Applied{range.entries.back().index});\n",
        "")],
    # A witness refuses the image it joins by: it never catches up past the leader's checkpoint
    # (group-heal A4; group_witness_test join).
    "witness-refuses-image": [(WITNESS,
        "\t\tloop_->driver().validated(id_, token);\n",
        "\t\tloop_->driver().validation_failed(id_, token);\n")],
    # The teardown waits for the chore alone, not the learner's install tasks: a close returns
    # while a stage still touches the group (--target=group_voters_test 0: close_mid_install).
    "close-skips-install-tasks": [
        (GROUP, "\t\t\treturn !self->running_ && !self->tasks_;\n", "\t\t\treturn !self->running_;\n"),
        (GROUP, "\t\t\tif (!self->running_ && !self->tasks_) { return false; }\n", "\t\t\tif (!self->running_) { return false; }\n")],
    # A pin release short of space fences the leader instead of stalling its chore
    # (--target=group_voters_test 0: release_short_of_space).
    "release-fences": [(GROUP,
        "\t\t\t\trelease_pin(*ix_, gone->pin);\n\t\t\t} catch (const std::system_error& x) {\n\t\t\t\tif (x.code() == std::errc::no_space_on_device) { return step::stall; }\n",
        "\t\t\t\trelease_pin(*ix_, gone->pin);\n\t\t\t} catch (const std::system_error& x) {\n")],
    # Recovery releases the pins an earlier process left, committing strata: a full disk fails
    # the open (--target=group_voters_test 0: reopen_short_of_space).
    "release-at-open": [(GROUP,
        "\t\tauto stale = held_pins(files_.io());\n",
        "\t\tfor (const auto g : held_pins(files_.io())) { ix_->release(g); }\n\t\tstd::vector<std::uint64_t> stale;\n")],
    # The pin goes with its flight's end, not when a later bundle supersedes it: the learner's
    # fetch outruns the leader's chore at every flight (--target=group_voters_test 0: install_gap).
    "pin-per-flight": [(GROUP,
        "if (at != recovered_.position && p->flights.empty()) {",
        "if (p->flights.empty()) {")],
    # A deposed proposer answers 503 instead of forwarding to the leader (A3).
    "no-forward": [(SUBMIT,
        "\t\t\t\tif (forwarded) {",
        "\t\t\t\tif (true) {")],
    # An invalid document is submitted again like a refused merge (A3).
    "resubmit-invalid": [(ANSWER,
        "\tcase verdict::version_conflict:\n",
        "\tcase verdict::invalid: return placements < max_placements ? next_step::resubmit : next_step::unavailable;\n\tcase verdict::version_conflict:\n")],
    # The file set keeps its last written file open: an idle group holds a descriptor (A6).
    "files-keep-handle": [
        (FILES, "\t\tauto f = open(name, true);\n", "\t\tlast_ = open(name, true);\n\t\tauto& f = last_;\n"),
        (FILES, "\tkronuz::journal::IO* io_;\n", "\tkronuz::journal::IO* io_;\n\tstd::unique_ptr<kronuz::journal::File> last_;\n")],
    # The file set owns the whole directory, as when it had its own: its recovery deletes
    # strata's names (A6, D111; --target=group_file_set_test).
    "no-prefix": [(BUNDLE,
        'inline constexpr std::string_view prefix = "app.";\n',
        'inline constexpr std::string_view prefix = "";\n')],
    # The first commit after a deferred create renames its log without syncing the directory:
    # a crash loses the index the group created (D112; --target=group_file_set_test).
    "first-commit-unsynced": [(INDEX,
        "\t\t\tfiles_[name] = bytes.size();\n\t\t\tsync_dir();\n\t\t});\n",
        "\t\t\tfiles_[name] = bytes.size();\n\t\t\tif (g != 1) { sync_dir(); }\n\t\t});\n")],
    # A closed group stays in its loop's table: what it leaves is ours (A6;
    # --target=group_footprint_test).
    "close-keeps-member": [(GROUP,
        "\t\tloop_->detach(id_);\n\t\tattached_ = false;\n\t\tco_await tear_down();\n",
        "\t\tattached_ = false;\n\t\tco_await tear_down();\n")],
    # The constructor joins the loop's table, on the host's thread, racing the loop's lookups
    # (group-catalog R2; --target=group_catalog_test attach, --target=group_lifetime_test failed-open).
    "attach-at-construction": [
        (GROUP, "\t\thandoff_.fire = &handoff_fired;\n\t}\n",
                "\t\thandoff_.fire = &handoff_fired;\n\t\tloop_->attach(id_, *this);\n\t\tattached_ = true;\n\t}\n"),
        (GROUP, "\t\tloop_->attach(id_, *this);  // another group with this id: refused, nothing begun\n\t\tattached_ = true;\n", "")],
    # A failed open stays in the loop's table with its chore running: the host destroys it off
    # the loop (group-catalog R1; --target=group_catalog_test failed-open).
    "failed-open-stays": [(GROUP,
        "\t\tco_await leave();\n\t\tstd::rethrow_exception(failed);\n",
        "\t\tstd::rethrow_exception(failed);\n")],
    # The sweep's, the bound's or the replace's close skips close_payload: the index closes
    # under a live group (group-catalog R3; --target=group_catalog_test sweep, bound, replace).
    "sweep-skips-payload": [(CATALOG,
        "\t\tclose_payload(s, down);\n",
        "\t\tif (mode != close_mode::sweep) { close_payload(s, down); }\n")],
    "bound-skips-payload": [(CATALOG,
        "\t\tclose_payload(s, down);\n",
        "\t\tif (mode != close_mode::bound) { close_payload(s, down); }\n")],
    # The sweep never closes a slot at the wake bound (D5; --target=group_catalog_test bound).
    "sweep-skips-bound": [(CATALOG,
        "\t\t\t\tif (st == slot_state::owed && posts < lim.transitions && begin_bound_close(i)) {\n",
        "\t\t\t\tif (st == slot_state::owed && posts < lim.transitions && false) {\n")],
    "replace-skips-payload": [(CATALOG,
        "\t\tclose_payload(s, false);\n",
        "")],
    # The group's chore runs its items unmarked: its own pinless applies are reported (N4; a
    # Debug build, --target=group_catalog_test pin-check, replace or bare-commit-crash).
    "payload-unmarked": [(GROUP,
        "\t\t\t\tconst payload_work mark;  // the slot payload's own call (D169; catalog CT-R15)\n",
        "")],
    # The footprint's schedulers start cold: the chore queue's first deque blocks land in the
    # groups' window and read as a destroyed group's (A6; --target=group_footprint_test).
    "cold-schedulers": [(FOOTPRINT,
        "\t\tfor (int i = 0; i < 1024; ++i) {\n",
        "\t\tfor (int i = 0; i < 0; ++i) {\n")],
    # An open group keeps the config it opened with: heap reserved but idle (A6, D111).
    "keeps-config": [(GROUP,
        "\t\topening_.reset();\n",
        "")],
    # Apply runs on the loop: one group's held apply stops every group on it (A10).
    "apply-on-loop": [(GROUP,
        "\t\ttask::spawn(chore_, drain());\n",
        "\t\ttask::spawn(loop_->scheduler(), drain());\n")],
    # read_applied runs its function on the loop, against apply on the chore (A10; TSan).
    "read-on-loop": [(GROUP,
        "\t\t\tco_await on_chore([&] { out.emplace(fn(*ix_)); });\n",
        "\t\t\tout.emplace(fn(*ix_));\n")],
    # The capture runs on the loop, against apply on the chore (A10; TSan).
    "capture-on-loop": [(GROUP,
        "co_await on_chore_step(capture_work(b));",
        "while (capture_work(b)() != step::done) {}")],
    # Checkpoints overlap: consensus refuses the second (Busy), failing a close racing one (A5).
    "checkpoints-overlap": [(GROUP,
        "\t\tauto serial = co_await checkpoints_.lock();\n\t\tco_return co_await checkpoint_held(std::move(token));\n",
        "\t\tco_return co_await checkpoint_held(std::move(token));\n")],
    # The close's teardown leaves no closed mark: a checkpoint or read after it runs against
    # the index and file set it destroyed (C2; --target=group_lifetime_test points).
    "no-closed-mark": [(GROUP,
        "\t\t\tclosed_ = true;\n",
        "")],
    # The close stops after one checkpoint, whatever committed since (A5).
    "close-uncovered": [(GROUP,
        "\t\t\tif (!s || published.through >= s->committed) { break; }\n",
        "\t\t\t(void)s;\n\t\t\tbreak;\n")],
    # A failed open leaves its consensus row: the same process cannot open the id again
    # (--target=group_lifetime_test failed-open).
    "failed-open-keeps-row": [(GROUP,
        "\t\t\ttry {\n\t\t\t\tco_await driver.close(id_);\n\t\t\t} catch (...) {}\n",
        "")],
    # The index's sealer hands a record on the driver's repeat calls in one turn, past D4's
    # window (--target=standalone_turn_log_test; its `lag` case).
    "seal-repeats": [(INDEX,
        "\t\tif (handed.load(std::memory_order_relaxed) > synced.load(std::memory_order_relaxed)) { return std::nullopt; }\n",
        "")],
    # A leader stalled on no_space keeps leadership: its group answers no `quorum` write (A3, D3).
    "no-hand-off": [(GROUP,
        "\t\t\tif (target) { task::spawn(loop_->scheduler(), transfer_on_loop(loop_, id_, *target)); }\n",
        "\t\t\t(void)target;\n")],
    # The hand-off timer re-arms only on a leader: a replica elected while stalled keeps
    # leadership (A3, D3; voters_test `stalled_follower_elected`).
    "handoff-leader-only": [(GROUP,
        "\t\tif (s && s->role == cs::Role::Leader) {\n",
        "\t\tif (!s || s->role != cs::Role::Leader) { return; }\n\t\t{\n")],
    # A `quorum` waiter takes the verdict of another term's entry at its placement (A3).
    "superseded-ignored": [(GROUP,
        "\t\t\tif (o.term != placement.term) {\n",
        "\t\t\tif (false) {\n")],
    # The write path submits a superseded request again (A3).
    "resubmit-superseded": [(SUBMIT,
        "\t\t\t\t} catch (...) {\n\t\t\t\t\tfailed = std::current_exception();\n\t\t\t\t}\n\t\t\t\tdrop(mine);\n",
        "\t\t\t\t} catch (const cs::superseded&) {\n\t\t\t\t\tdrop(mine);\n\t\t\t\t\tcontinue;\n"
        "\t\t\t\t} catch (...) {\n\t\t\t\t\tfailed = std::current_exception();\n\t\t\t\t}\n\t\t\t\tdrop(mine);\n")],
    # A forwarded request that finds no leader is forwarded again (A3).
    "forward-twice": [(SUBMIT,
        "\t\t\t\tif (forwarded) {",
        "\t\t\t\tif (false) {")],
    # Apply publishes the applied position before strata's write: a host sees an entry applied
    # whose write is not visible (A4; --target=group_lifetime_test held).
    "applied-before-write": [(GROUP,
        "\t\tverdict v;\n\t\tif (phase_ == phase::strata) {\n",
        "\t\tverdict v;\n\t\tapplied_.store(e.index, std::memory_order_release);\n\t\tif (phase_ == phase::strata) {\n")],
    # read_applied reads this replica's state without the Driver's quorum probe (A4).
    "read-skips-probe": [(GROUP,
        "\t\tconst auto ready = co_await loop_->driver().read(id_, std::move(token));\n\t\tco_await applied_at(ready.index);\n",
        "\t\t(void)token;\n")],
    # An index opens a bounded group as a stored one: a head below the bound replays the records after it
    # and misses the damaged batches (--target=standalone_turn_log_test t5b).
    "bounded-as-stored": [(INDEX,
        "status == kronuz::journal::GroupStatus::bounded && cur_.applied.index <= opts_.log->bound(g)",
        "false && cur_.applied.index <= opts_.log->bound(g)")],
    # A head at the bound loses the group, though it holds that record's whole batch
    # (--target=standalone_turn_log_test t5b).
    "bounded-at-bound": [(INDEX,
        "cur_.applied.index < opts_.log->bound(g)",
        "cur_.applied.index <= opts_.log->bound(g)")],
    # A bounded group opens through create, not recover: the table's entry stays bounded and the open
    # fails on it, not as the Store's loss (--target=group_lifetime_test bounded).
    "bounded-created": [(GROUP,
        "status == jn::GroupStatus::closed || status == jn::GroupStatus::bounded) {",
        "status == jn::GroupStatus::closed) {")],
    # A witness opens a bounded group through create, not recover (--target=group_lifetime_test bounded-witness).
    "bounded-witness-created": [(WITNESS,
        "status == jn::GroupStatus::closed || status == jn::GroupStatus::bounded) {",
        "status == jn::GroupStatus::closed) {")],
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
