#!/usr/bin/env bash
# Builds the pinned RocksDB (11.11.1: release branch 11.11.fb at ec933778, untagged upstream;
# tags stop at v11.8.1) and kv_rocksdb against it, Release, with zstd and LZ4, the system
# allocator, and no -march, as strata's engine builds. The driver builds without RTTI, as
# a Release RocksDB does (its audit listener derives from a RocksDB class).
#   bench/engines/rocksdb-11.11/build.sh            sources in $ROCKSDB_SRC, build in $ROCKSDB_BUILD
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
commit=ec93377878cdebdf5fdc6d20dafa14d6934cff9a
src="${ROCKSDB_SRC:-$HOME/.scratch/rocksdb-11.11.fb}"
out="${ROCKSDB_BUILD:-$HOME/.scratch/rocksdb-11.11-build}"
jobs="${JOBS:-4}"
if [[ ! -d "$src/.git" ]]; then
	git clone -q --branch 11.11.fb https://github.com/facebook/rocksdb.git "$src"
fi
if [[ "$(git -C "$src" rev-parse HEAD)" != "$commit" ]]; then
	git -C "$src" fetch -q --depth 1 origin "$commit"
	git -C "$src" checkout -q "$commit"
fi
gflags=OFF
if [[ -e /usr/local/include/gflags/gflags.h || -e /usr/include/gflags/gflags.h ]]; then gflags=ON; fi
cmake -S "$src" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTABLE=1 \
	-DWITH_ZSTD=ON -DWITH_LZ4=ON -DWITH_SNAPPY=OFF -DWITH_ZLIB=OFF -DWITH_BZ2=OFF -DWITH_JEMALLOC=OFF \
	-DWITH_GFLAGS=$gflags -DWITH_TESTS=OFF -DWITH_BENCHMARK_TOOLS=$gflags -DWITH_TOOLS=OFF -DWITH_CORE_TOOLS=OFF \
	-DROCKSDB_BUILD_SHARED=OFF -DFAIL_ON_WARNINGS=OFF >/dev/null
targets=(rocksdb)
[[ $gflags == ON ]] && targets+=(db_bench)
ninja -C "$out" -j "$jobs" "${targets[@]}"
extra=()
[[ "$(uname -s)" == Linux ]] && extra+=(-ldl)
inc=(); lib=()
[[ -d /usr/local/include ]] && inc+=(-I/usr/local/include) && lib+=(-L/usr/local/lib)
"${CXX:-c++}" -std=c++20 -O3 -DNDEBUG -fno-rtti -I"$src/include" "${inc[@]}" "$here/kv_rocksdb.cc" "$out/librocksdb.a" "${lib[@]}" -lzstd -llz4 -lpthread "${extra[@]}" -o "$out/kv_rocksdb"
echo "$out/kv_rocksdb"
