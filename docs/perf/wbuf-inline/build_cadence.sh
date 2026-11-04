#!/bin/bash
# build_cadence.sh SRCROOT BUILDDIR OUT: flush_cadence against SRCROOT's headers and BUILDDIR's deps
set -e
W=/Users/kronuz/code/Kronuz/strata-wt/wbuf-inline
src=$1; bd=$2; out=$3
incs=$(ninja -C $bd -t commands kv_proto_bufmem | grep "bufmem.cc" | grep -o -- "-I[^ ]*" | grep -v "^-I$W/include$" | grep -v "^-I$src/include$" | tr '\n' ' ')
/usr/local/opt/llvm/bin/clang++ -I$src/include -I$src $incs -O3 -g -fno-omit-frame-pointer -DNDEBUG -std=gnu++20 -o $out $W/docs/perf/wbuf-inline/flush_cadence.cc \
  $bd/_deps/compressors-build/libcompressors.a $bd/_deps/lz4-build/liblz4.a $bd/_deps/zstd-build/lib/libzstd.a $bd/_deps/threadpool-build/libthreadpool.a
