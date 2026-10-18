#!/bin/bash
# Targeted indexing A/B in the gate VM (PLAN, "Targeted runs"): strata binaries A, B, and up
# to C and D beside an anchor baseline, keyed, on the first DOCS documents (100,000 by default),
# TRIALS rounds (5) with the order rotated, the page cache dropped before each unit.
# Engines run their binaries directly, as write_amp.sh does: `make index` would time a
# Lucene rebuild too. Units with a result are skipped, so a stopped run resumes.
# Results in ~/bench/results/iab-NAME, scored by index_ab.py.
#   [VARIANTS="a b tantivy-0.26-st"] [A_BIN=... B_BIN=... C_BIN=... D_BIN=...] [DOCS=100000] [TRIALS=5] \
#     [STRATA_THREADS=1] [STRATA_BUDGET_MB=2056] index_ab.sh NAME
# A variant is a, b, c, or d (strata at A_BIN to D_BIN, with A_ENV to D_ENV's VAR=value words
# in its environment) or a harness engine's directory name.
set -u
name=${1:?usage: index_ab.sh NAME}
cd ~/bench/search-benchmark-game || exit 1
out=~/bench/results/iab-$name
mkdir -p "$out"
export PATH=$HOME/.cargo/bin:$PATH KEYED=1
export STRATA_THREADS=${STRATA_THREADS:-1}
export STRATA_BUDGET_MB=${STRATA_BUDGET_MB:-$(awk '/^BUDGET [0-9]+$/ {print $2}' ~/bench/results/calibrate-n2-5m.out)}
docs=${DOCS:-100000} trials=${TRIALS:-5}
[ -f "corpus-$docs.json" ] || head -n "$docs" corpus.json > "corpus-$docs.json"
corpus=$PWD/corpus-$docs.json
[ -s "$corpus" ] || { echo "empty corpus $corpus"; exit 1; }
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
{
	echo "docs $docs trials $trials threads $STRATA_THREADS budget $STRATA_BUDGET_MB"
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
		dir=~/iab-idx; rm -rf "$dir" && mkdir -p "$dir"
		case $v in
		a | b | c | d)
			read -r -a vars <<< "$(strata_env "$v")"
			cmd=(env "${vars[@]}" "$(strata_bin "$v")" index "$dir") ;;
		tantivy*) cmd=("engines/$v/target/release/build_index" "$dir") ;;
		lucene*) cmd=(java -server --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
			-cp "engines/$v/build/libs/search-index-benchmark-game-lucene-1.0-SNAPSHOT.jar:engines/$v/build/dependencies/*" BuildIndex "$dir") ;;
		*) echo "unknown variant $v"; exit 1 ;;
		esac
		sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
		echo "$(date +%T) $unit"
		if /usr/bin/time -v -o "$out/$unit.time" "${cmd[@]}" < "$corpus" > "$out/$unit.log" 2>&1; then
			ls -l "$dir" > "$out/$unit.files"
			find "$dir" -type f -exec stat -c%s {} + | awk '{n += $1} END {print n}' > "$out/$unit.bytes"
		else
			echo "UNIT_FAILED $unit"; failed=1
		fi
	done
done
rm -rf ~/iab-idx
[ "$failed" = 0 ] || exit 1
echo IAB_DONE
