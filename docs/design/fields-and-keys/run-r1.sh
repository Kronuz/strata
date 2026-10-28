#!/bin/sh
# Reruns revision 1's probe (probe-r1.txt) from the strata worktree root:
#   XAPIAND=path/to/xapiand ACCOUNTS=path/to/accounts.ndjson \
#     quiet light fields-rev 20 "fields probe" -- docs/design/fields-and-keys/run-r1.sh
set -e
D=docs/design/fields-and-keys; S=.scratch/fk; PY=/usr/local/bin/python3
mkdir -p $S
$PY -I $D/sparse_corpora.py $S
$PY -I $D/xapiand_terms.py "$XAPIAND" "$ACCOUNTS" $S bank
$PY -I $D/xapiand_terms.py "$XAPIAND" $S/sparse-kw.ndjson $S sparsekw
$PY -I $D/xapiand_terms.py "$XAPIAND" $S/sparse-num.ndjson $S sparsenum
clang++ -std=c++20 -O2 -DNDEBUG -I ../../termdict -I ../../fst -I ../../varint $D/fieldprobe.cc -o $S/fieldprobe
for c in bank sparsekw sparsenum; do
	for sh in all 0 1 2 3 4; do
		o=$S/out/$c-$sh; mkdir -p $o
		if [ $sh = all ]; then $PY -I $D/fieldsize.py split $S/$c.terms $o; else $PY -I $D/fieldsize.py split $S/$c.terms $o $sh; fi
		$S/fieldprobe $o 4 16 64 256 1024 > $o/probe.txt
		$PY -I $D/fieldsize.py report $o $o/probe.txt > $o/report.txt
	done
done
