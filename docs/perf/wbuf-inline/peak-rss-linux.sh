set -u
cmake -S . -B ../b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DSTRATA_KV_PROTO=ON > $OUT/cfg.log 2>&1 || { echo cfg failed; tail -5 $OUT/cfg.log; exit 1; }
cmake --build ../b -j4 --target kv_proto_bufmem > $OUT/build.log 2>&1 || { echo build failed; grep error $OUT/build.log | head; exit 1; }
incs=$(ninja -C ../b -t commands kv_proto_bufmem | grep "bufmem.cc" | grep -o -- "-I[^ ]*" | grep -v "^-I$PWD/include$" | tr '\n' ' ')
libs=$(for n in libcompressors.a liblz4.a libzstd.a libthreadpool.a; do find ../b/_deps -name $n | head -1; done | tr '\n' ' ')
echo "libs: $libs"
for arm in base new fix2; do
  src=$PWD/arms/$arm; [ $arm = new ] && src=$PWD
  clang++ -I$src/include -I$src $incs -O2 -g -DNDEBUG -std=gnu++20 -o ../cad-$arm docs/perf/wbuf-inline/flush_cadence.cc -Wl,--start-group $libs -Wl,--end-group -lpthread -ldl > $OUT/cc-$arm.log 2>&1 || { echo cc $arm failed; head -20 $OUT/cc-$arm.log; exit 1; }
done
cd ..
for t in 1 2 3 4 5; do
  for cfg in "100 8" "500 8" "500 32" "1024 32"; do
    read -r v mb <<< "$cfg"
    for arm in A A2 B F; do
      bin=./cad-base; [ $arm = B ] && bin=./cad-new; [ $arm = F ] && bin=./cad-fix2
      echo "$arm t$t $v $mb m $(env -u CADENCE_NO_MERGE $bin $v $mb 400000 d-$arm)" | tee -a $OUT/rss-linux.txt
    done
  done
done
