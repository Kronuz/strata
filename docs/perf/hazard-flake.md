# hazard test flake (CI 40444409174 :293, run 40355527674 :491)

Cause: test bug, not the registry. The releaser threads in `drift`, `stress` and the shared-registry
test do `pop queue; if empty and stop.load() break`. A reader can push its last guard after the
releaser saw the queue empty and before `stop` is set (set after the reader joins); the releaser
then sees `stop`, exits, and that guard stays in the deque, still announcing its generation, so
`retired.reclaim` keeps the view (`retired.empty()` / `left == 0` fail). Rare on a fast host, more
likely on a loaded FreeBSD runner.

Evidence: widening the window (2 ms sleep between the empty-queue check and the `stop` load)
fails 100/100 runs at :293 and :491 with "record N names ... live" and "holder keeps retired";
unwidened original, 0/300 locally (the race needs a loaded host).

Fix: sample `stop` before the queue check (`const bool done = stop.load()`); once `done` is
true no push can follow, so an empty queue is final. Fixed binary: 0/300 plain. Not run: TSan loop
of 10,000 (the cause is in the test's queue protocol, not engine memory ordering).
