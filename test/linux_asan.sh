#!/bin/bash
# ASan+UBSan ctest of this tree on strata's local Linux VM, logs in $OUT:
#   tools/vm/aws-linux.sh run-local TREE test/linux_asan.sh   (inside a hold; `run` takes one itself)
set -uo pipefail
OUT=${OUT:-.}
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
	"-DCMAKE_CXX_FLAGS=-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -D_GLIBCXX_ASSERTIONS" \
	"-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" > "$OUT/cfg-asan.log" 2>&1 || { tail -20 "$OUT/cfg-asan.log"; exit 1; }
ninja -C build-asan -j4 > "$OUT/build-asan.log" 2>&1 || { tail -40 "$OUT/build-asan.log"; exit 1; }
ctest --test-dir build-asan --output-on-failure -j4 --timeout 1800 > "$OUT/ctest-asan.log" 2>&1; rc=$?
uname -sm; clang++ --version | head -1
tail -4 "$OUT/ctest-asan.log"
grep -c -E 'ERROR: AddressSanitizer|runtime error:' "$OUT/ctest-asan.log" | sed 's/^/sanitizer reports: /'
exit $rc
