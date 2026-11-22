# track-c-gates review: C2 gate (architect)

Commit: strata:f46af14b179a5a500e239106feadd192928a1dbc (the report as of strata:605f1dd5fc6d263ea21e86888828962bf5b08770), judging rows at journal:178030b38f5617337486407e6cc35d70881a0841 and consensus:8801b99025ba4f094e2566e7284d76b5b119a8b0
Verdict: APPROVE WITH CONDITIONS
Date: 2026-11-21

The report lands as the record of C2 at today's mains. C2 itself is not passed: T9 is red at
`journal` main, and C2 passes only when Condition 1 holds. Until then the lead does not mark
Track C's C2 passed.

Scope: `docs/gates/track-c-c2.md`, its section "At `consensus` main `8801b99` and `journal` main
`178030b`" above all, against each job's raw log (`~/.scratch/campaign/jobs/<id>/a1/log`) and
spec (`spec.json`: command, commit, repository). Code read: journal `test/turn_log_power_test.cc`
at `d90797c` and `178030b`, `fuzz/fuzz_turn_log.cc` at `178030b`; consensus
`fuzz/fuzz_decoders.cc:67-68` for comparison. No GPL sources were read.

## Evidence I relied on

| Job | Platform, commit | Result in the log |
|---|---|---|
| 261120-151854-18b807 | macOS, journal 178030b | `jpower 200`: six geometries, `1036264 checks, 7 failures`; FAIL lines at 512 B seeds 91 and 140, 2 KiB seeds 145 and 199 (two checks each), 256 B seed 122; exit 1 |
| 261120-152238-d74166 | FreeBSD VM, journal 178030b | the same seven FAIL lines and `1036264 checks`; the other rows pass (40,913 and 734,100 checks, 0 failures; 43, 4 and 2 of 2 ctest); both mutants `KILLED` |
| 261120-152229-2096f8 | Linux VM, journal 178030b | `jpower 200`: `1046208 checks, 0 failures`, other crash-point counts (512 B 51,928, 8,647 corrupted); both mutants `KILLED` |
| 261118-115852-e136e7 | macOS, journal d90797c | `jpower 200` passed (one geometry, see F2) |
| 261120-151850-4592f2, 261120-151852-86e99e, 261120-151857-83dd88, 261120-151859-75f543 | macOS, journal 178030b | build; rows 0 failures; `no-sync-data` 1,285 and `no-barrier-wait` 67 failures, both `KILLED` |
| 261120-151902-b711ce, 261120-151904-2b2fc3, 261120-151907-369f91, 261120-151910-c43dad, 261120-151912-4175b8, 261120-151915-b0da39 | macOS, consensus 8801b99 at its own pins | every count in the report's rows; the budget run prints `10000 seeds x 3000 steps ... : ok` for the core and the Driver; seeds 399 and 727 `ok` |
| 261120-152227-124538, 261120-152236-dd591f | Linux and FreeBSD VMs, consensus 8801b99 | the macOS counts exactly |

Every figure in the 8801b99/178030b section matches its log, and each job ran at the commit the
row names.

## What I checked myself

Scripts are under `.scratch/xapiand/review-track-c-gates-architect/` in the Xapiand checkout.

- Probe 261121-020233-595acb (`argorder.sh`, light, exit 0): the power test's workload line
  (`turn_log_power_test.cc:67`, two `random()` calls as arguments of one call) compiled with
  clang++ and g++-16 from one `mt19937_64` seed. The first eight records differ in size and kind
  (clang `g5 s1 size9 C`, g++ `g5 s1 size121 B`): the argument evaluation order is unspecified,
  and Clang and GCC take opposite orders. That is why Linux (GCC 13.3) draws other cases than
  macOS and FreeBSD (both Clang, identical counts). See F3.
- `git -C journal grep` of the two failing checks: both are new at `178030b`
  (`turn_log_power_test.cc:279-280`); `d90797c`'s test has neither and runs only the 512 B
  geometry (the six-geometry table is `:300` at `178030b`). See F2.

### Question 1: is the red row stated honestly, and does C2 approve with it?

Yes, it is stated honestly. The title, the section's opening paragraph, the failure table and both
rows say "Fail" in bold, with Detent's msg-183 named. The report does not attribute the failure,
and it says why the Linux pass does not clear it. The failures are of the kind T9 exists to catch:
"a group not lost keeps every acknowledged record" fails at 2 KiB seeds 145 and 199 and at 256 B
seed 122, so acknowledged records are lost in groups recovery keeps.

C2 does not pass with a red T9, and a condition does not turn it green. The report is accurate,
so it lands; C2 passes when Condition 1 holds. Detent's fix is in progress
(`quest/detent/journal-power-silent-loss`; `journal` main is still `178030b`).

## Findings

| ID | Blocking | Kind | Finding | Evidence | Fix |
|---|---|---|---|---|---|
| F1 | no | gap | T9's `jpower 200` fails at `journal:178030b` on macOS and FreeBSD with the same seven failures, three of them acknowledged records lost in kept groups. The report states it as red. C2 is not passed while it is. | 18b807, d74166 (logs above); report lines 106-131 | Condition 1 |
| F2 | no | gap | "Passed at `journal:d90797c`" (lines 117-121, 131) is not a baseline for a regression. `d90797c`'s test runs one geometry and lacks both failing checks, so the 512 B case with "the same 51,998 crash points" passes at `d90797c` because the checks it fails do not exist there. The failure is new coverage finding a defect, not a demonstrated regression, and `consensus` main's pin `journal:9e11713`, which also predates the checks, has not been tested against them. | `turn_log_power_test.cc:279-280, :300` at `178030b`; absent at `d90797c` | One sentence at line 121: the checks and five geometries are new at `178030b`, so the earlier pass does not date the defect. |
| F3 | no | defect | The power test's seeds do not replay across compilers. `turn_log_power_test.cc:67` passes `random() % 700 + 1` and `random() % 5 ? ...` as two arguments, and Clang and GCC evaluate them in opposite orders. The Linux leg of T9 therefore never runs the failing cases, and T9's "crash points and seeds recorded" is only replayable per compiler. The defect is in Detent's test, not the journal. | Probe 261121-020233-595acb | Detent: draw both into named locals before the call. Report it with msg-183. |
| F4 | no | gap | T8's CPU-hour per decoder (turn-log segment, group checkpoint record) is not shown. The five runs total 125 wall-minutes, niced on a loaded host, with no CPU time recorded. `journal_fuzz` picks the decoder from `data[0]` and has no pin, so neither decoder's share is known. They ran at `journal:4d364f1`, and `turn_log.h`'s recovery has changed by 77 lines since then, with msg-183's fix still to come. The report says all of this (line 75), so the row is honest but unmet. | Report line 75; `fuzz/fuzz_turn_log.cc` at `178030b` (`data[0]` picks one); consensus pins its decoders with `FUZZ_DECODER` (`fuzz_decoders.cc:67-68`) | Condition 2 |
| F5 | no | nit | "Obsolete, owed, and moved" (line 150) says both simulations run one turn log per node and assigns T2 on several loops to `consensus-sim-loops`. That quest has landed, and the 8801b99 rows already boot hosts with 2, 3 and 4 loops (2b2fc3: "hosts booted with 2/3/4 loops 58/55/66 times"; 369f91: 8,648 re-partitions). | 2b2fc3, 369f91 | Move the line to the rerun: T2's budget ran on hosts of 2 to 4 loops. |
| F6 | no | nit | "Commits and rig" still names `journal:4d364f1` and `consensus:5923b6c` as the mains, while the title and the last section use `178030b` and `8801b99`. | Report lines 9-13 | Name both pairs, and say which rows each covers. |

## Conditions

1. T9's `jpower 200` passes (0 failures) at the `journal` commit that carries Detent's msg-183 fix,
   on macOS, Linux and FreeBSD, together with the report's other `journal` rows and both mutants.
   `consensus` moves its `journal` pin to that commit or later, and C2's `consensus` rows rerun at
   the new pin. Until F3 is fixed, the Linux leg runs other cases and the report says so. C2 is
   passed when this holds; the lead fills the cells with the job ids and closes C2. This needs no
   second review unless the fix changes the `RFTW0001` bytes.
2. T8: one measured CPU-hour per decoder (`user + sys` from `time -p`, at least 3,600 s each),
   with the input pinned to that decoder, at the commit of Condition 1, recorded with the
   executed-unit count. The pin would be a `FUZZ_DECODER`-style override in `fuzz_turn_log.cc`,
   which is Detent's. Each run ends with no crash, timeout or leak artifact.

## Not run

- I did not rerun any row: every claim above is checked against a passing or failing job at the
  stated commit, plus one light probe.
- I did not attribute msg-183's failures, and I did not run the new checks against
  `journal:9e11713` (F2). Detent owns both.
- Sanitizers and CI for `journal` and `consensus` belong to CI. T6, D14's quiescence rows and
  installs in the cluster simulation are outside these rows, as the report says.
- The pre-driver results section is kept as history; I did not recheck it.
