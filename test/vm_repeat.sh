#!/usr/bin/env bash
# Repeats one test on strata's FreeBSD VM: ships each TREE, builds TARGET there and runs it N times,
# P at once (contention like a loaded CI runner). Prints one line per tree; fails if any run failed.
#   test/vm_repeat.sh TARGET N P LABEL=TREE...
set -uo pipefail
target=${1:?usage: vm_repeat.sh TARGET N P LABEL=TREE...} n=${2:?} p=${3:?}; shift 3
vm="$(cd "$(dirname "$0")/.." && pwd)/tools/vm/freebsd.sh"
status=0
for spec in "$@"; do
	label=${spec%%=*} tree=${spec#*=} dir="preflight/repeat-$label"
	(cd "$tree" && git ls-files -z | xargs -0 tar -cf -) |
		"$vm" ssh "rm -rf ~/$dir && mkdir -p ~/$dir && tar -xf - -C ~/$dir" || { echo "$label: ship failed"; status=1; continue; }
	"$vm" ssh "cd ~/$dir && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g' >build.log 2>&1 &&
		cmake --build build --parallel 4 --target $target >>build.log 2>&1 || { tail -30 build.log; exit 1; }" </dev/null ||
		{ echo "$label: build failed"; status=1; continue; }
	# Each run writes its exit status and, on failure, its output.
	out=$("$vm" ssh "cd ~/$dir && mkdir -p runs && seq 1 $n | xargs -P $p -I{} sh -c './build/$target >runs/{}.log 2>&1; echo \$? >runs/{}.rc'
		fails=\$(grep -L '^0\$' runs/*.rc | wc -l | tr -d ' ')
		echo \"runs=$n parallel=$p failed=\$fails\"
		for f in \$(grep -L '^0\$' runs/*.rc | head -3); do echo \"--- \$f\"; grep -n -i 'fail\|check' \"\${f%.rc}.log\" | head -5; done
		cd ~ && rm -rf ~/$dir" </dev/null)
	echo "$label ($(git -C "$tree" rev-parse --short HEAD)): $out"
	case $out in *failed=0*) ;; *) status=1 ;; esac
done
exit $status
