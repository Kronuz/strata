# Research prototypes for background work without stalls (docs/research/no-stalls.md).
# Built only with -DSTRATA_RESEARCH=ON; never part of the default build or CI. The RocksDB
# driver needs a pinned RocksDB source and build: -DSTRATA_ROCKSDB_SRC=... (its include/)
# and -DSTRATA_ROCKSDB_BUILD=... (its librocksdb.a); without them it is skipped.
add_executable(research_ingest ingest.cc)
target_link_libraries(research_ingest PRIVATE strata)

set(STRATA_ROCKSDB_SRC "" CACHE PATH "RocksDB source checkout (for research_rocks)")
set(STRATA_ROCKSDB_BUILD "" CACHE PATH "RocksDB build directory holding librocksdb.a")
if(STRATA_ROCKSDB_SRC AND STRATA_ROCKSDB_BUILD)
  find_library(STRATA_ROCKSDB_LIB rocksdb PATHS "${STRATA_ROCKSDB_BUILD}" NO_DEFAULT_PATH)
  find_library(STRATA_ZSTD_LIB zstd PATHS /usr/local/lib /opt/homebrew/lib)
  find_library(STRATA_LZ4_LIB lz4 PATHS /usr/local/lib /opt/homebrew/lib)
  if(STRATA_ROCKSDB_LIB)
    add_executable(research_rocks rocks.cc)
    target_include_directories(research_rocks PRIVATE "${STRATA_ROCKSDB_SRC}/include")
    target_compile_features(research_rocks PRIVATE cxx_std_20)
    target_link_libraries(research_rocks PRIVATE "${STRATA_ROCKSDB_LIB}" "${STRATA_ZSTD_LIB}" "${STRATA_LZ4_LIB}" pthread)
  endif()
endif()
