#!/bin/sh
# Builds index_test with one piece of sync-at-first-commit undone and exits 0 only when the check
# that guards it fails (docs/design/sync-at-first-commit.md, Evidence). Exits 2 on a build failure
# or a mutant that no longer applies, 1 when the check passes.
#   open   the open's sync kept               A2's zero-sync check
#   drop   the inherited sync dropped         A3's first-commit crash check
#   after  the inherited sync after the edit's sync   A3's first-commit crash check
#   gate   the removal gate dropped           A3's reclaim crash check
#   dir    directory syncs outside a commit not the inherited one   A3's review-F1 check
# Usage: first_commit_mutant.sh NAME, from the checkout's root; restores index.hh on exit.
set -u
f=include/strata/core/index.hh
t=test/index_test.cc
line() { grep -n "$1" "$t" | head -1 | cut -d: -f1; }
# The first lines at or after the line holding $1 that hold $2.
after() { awk -v a="$(line "$1")" -v p="$2" 'NR >= a && index($0, p) { print NR }' "$t" | head -${3:-1} | tr '\n' ' '; }
case ${1:-} in
open) want=$(line 'CHECK(fs.syncs() == before);') ;;
drop|after) want=$(after 'A3: a power loss at each mutation of the first commit' 'CHECK(lost == 0);'; after 'A3: a power loss at each mutation of the first commit' 'CHECK(broken == 0);') ;;
gate) want=$(after 'A3, D4: a removal waits' 'CHECK(broken == 0);') ;;
dir) want=$(after 'A3, review F1' 'CHECK(fenced_by(') ;;
*) echo "usage: first_commit_mutant.sh open|drop|after|gate|dir" >&2; exit 2 ;;
esac
cp "$f" "$f.orig" || exit 2
trap 'mv "$f.orig" "$f"' EXIT
python3 - "$1" "$f" <<'PY' || exit 2
import sys
name, path = sys.argv[1], sys.argv[2]
s = open(path).read()
edits = {
    "open": [("\t\t\tinherited_below_ = cur_.next_segment;\n",
              "\t\t\tinherited_below_ = cur_.next_segment;\n\t\t\t{ kronuz::journal::File* const lf = log_.get(); io_->sync_files_and_directory({&lf, 1}); }  // MUTANT open\n")],
    "drop": [("\t\t\tinherited_owed_ = true;\n", "\t\t\tinherited_owed_ = false;  // MUTANT drop\n")],
    "after": [("\t\tif (inherited_owed_) { files.push_back(log_.get()); }\n", "\t\t// MUTANT after\n"),
              ("\t\tinherited_owed_ = false;  // ordered ahead of the edit, durable with it\n",
               "\t\tif (inherited_owed_) { sync_inherited(); }  // MUTANT after\n")],
    "gate": [("std::any_of(out.begin(), out.end(), [&](const std::string& n) { return inherited(n); })) { sync_inherited(); }",
              "false) { sync_inherited(); }  // MUTANT gate")],
    "dir": [("\t\tif (inherited_owed_) { return sync_inherited(); }\n", "\t\t// MUTANT dir\n")],
}[name]
for a, b in edits:
    if s.count(a) != 1:
        sys.exit("mutant %s no longer applies" % name)
    s = s.replace(a, b)
open(path, "w").write(s)
PY
grep -n "MUTANT" "$f"
cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build --target index_test -j4 >/dev/null || exit 2
./build/index_test > index_test.out 2>&1
rc=$?
cat index_test.out
[ "$rc" -ne 0 ] || exit 1
for l in $want; do grep -q "index_test.cc:$l:" index_test.out && exit 0; done
echo "failed elsewhere: wanted index_test.cc line(s) $want" >&2
exit 1
