#!/bin/bash
# N indexes writing at once on the gate VM's cores (the many-index rows, docs/PLAN.md): strata
# variants run `strata_engine index-many` (one process, one owner thread per index, one shared
# pool), each baseline runs N one-thread builders at once; every index gets its own slice of
# DOCS documents (the first N x DOCS of the corpus), keyed. TRIALS rounds (5) with the order
# rotated, the page cache dropped before each unit; wall and CPU are the whole set's.
# Results in ~/bench/results/imany-NAME, scored by index_ab.py.
#   [VARIANTS="a b tantivy-0.26-st"] [A_BIN=... A_ENV=...] [N=8] [DOCS=100000] [TRIALS=5] \
#     [STRATA_BUDGET_MB=2056] index_many.sh NAME
set -u
name=${1:?usage: index_many.sh NAME}
cd ~/bench/search-benchmark-game || exit 1
out=~/bench/results/imany-$name
mkdir -p "$out"
export PATH=$HOME/.cargo/bin:$PATH KEYED=1
export STRATA_BUDGET_MB=${STRATA_BUDGET_MB:-$(awk '/^BUDGET [0-9]+$/ {print $2}' ~/bench/results/calibrate-n2-5m.out)}
n=${N:-8} docs=${DOCS:-100000} trials=${TRIALS:-5}
slices=()
for i in $(seq 0 $((n - 1))); do
	f=$PWD/corpus-many-${n}x$docs-$i.json
	[ -s "$f" ] || head -n $((n * docs)) corpus.json | tail -n +$((i * docs + 1)) | head -n "$docs" > "$f"
	slices+=("$f")
done
read -r -a variants <<< "${VARIANTS:-a b tantivy-0.26-st}"
count=${#variants[@]}
strata_bin() {
	case $1 in
	a) echo "${A_BIN:?}" ;;
	b) echo "${B_BIN:?}" ;;
	c) echo "${C_BIN:?}" ;;
	d) echo "${D_BIN:?}" ;;
	esac
}
strata_env() {
	case $1 in
	a) echo "${A_ENV:-}" ;;
	b) echo "${B_ENV:-}" ;;
	c) echo "${C_ENV:-}" ;;
	d) echo "${D_ENV:-}" ;;
	esac
}
# One baseline builder per slice, all at once, into DIR/0 .. DIR/N-1.
many() {
	local e=$1 dir=$2 i pids=()
	for i in $(seq 0 $((n - 1))); do
		mkdir -p "$dir/$i"
		case $e in
		tantivy*) "engines/$e/target/release/build_index" "$dir/$i" < "${slices[$i]}" > /dev/null 2>&1 & ;;
		lucene*) java -server --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED \
			-cp "engines/$e/build/libs/search-index-benchmark-game-lucene-1.0-SNAPSHOT.jar:engines/$e/build/dependencies/*" BuildIndex "$dir/$i" \
			< "${slices[$i]}" > /dev/null 2>&1 & ;;
		esac
		pids+=($!)
	done
	local failed=0
	for p in "${pids[@]}"; do wait "$p" || failed=1; done
	return $failed
}
{
	echo "n $n docs $docs trials $trials budget $STRATA_BUDGET_MB"
	for v in "${variants[@]}"; do
		case $v in
		a | b | c | d) bin=$(strata_bin "$v"); echo "$v $bin $(sha256sum "$bin" | cut -c1-16) $(strata_env "$v")" ;;
		*) echo "$v" ;;
		esac
	done
} > "$out/setup"
failed=0
for t in $(seq 1 "$trials"); do
	for i in $(seq 0 $((count - 1))); do
		v=${variants[$(((i + t - 1) % count))]}
		unit=t$t-$v
		[ -f "$out/$unit.bytes" ] && continue
		dir=~/imany-idx; rm -rf "$dir" && mkdir -p "$dir"
		case $v in
		a | b | c | d)
			read -r -a vars <<< "$(strata_env "$v")"
			cmd=(env "${vars[@]}" "$(strata_bin "$v")" index-many "$dir" "${slices[@]}") ;;
		*) cmd=(bash -c "$(declare -p slices); n=$n; $(declare -f many); many $v $dir") ;;
		esac
		sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
		echo "$(date +%T) $unit"
		if /usr/bin/time -v -o "$out/$unit.time" "${cmd[@]}" > "$out/$unit.log" 2>&1; then
			find "$dir" -type f -exec stat -c%s {} + | awk '{n += $1} END {print n}' > "$out/$unit.bytes"
		else
			echo "UNIT_FAILED $unit"; failed=1
		fi
	done
done
rm -rf ~/imany-idx
[ "$failed" = 0 ] || exit 1
echo IMANY_DONE
