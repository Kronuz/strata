cmake_minimum_required(VERSION 3.18)
project(strata LANGUAGES C CXX)

include(FetchContent)

# Building blocks (Kronuz libraries). Point FETCHCONTENT_SOURCE_DIR_<NAME> at local
# checkouts to develop them together.
set(TASK_ASIO OFF CACHE BOOL "" FORCE)
# Each building block is pinned to the commit its Phase 1 gate numbers were measured at.
FetchContent_Declare(fst GIT_REPOSITORY https://github.com/Kronuz/fst.git GIT_TAG 24da59a9e3d7225865335dcdc71ddeb2e1cb66ec)
FetchContent_Declare(task GIT_REPOSITORY https://github.com/Kronuz/task.git GIT_TAG bbb2e14aecb0fe49bcfac2685d64408ef6177c23)
FetchContent_Declare(hashes GIT_REPOSITORY https://github.com/Kronuz/hashes.git GIT_TAG 7ab0c23325debfbaf4104044987ba6583d14808f)
FetchContent_Declare(crc32c GIT_REPOSITORY https://github.com/Kronuz/crc32c.git GIT_TAG 04737c35c6b22f6edf4942757c7b2da084f7bd81)
FetchContent_Declare(termdict GIT_REPOSITORY https://github.com/Kronuz/termdict.git GIT_TAG eaf83b33621b54ea388b30b189e0cd5fd806e861)
FetchContent_Declare(varint GIT_REPOSITORY https://github.com/Kronuz/varint.git GIT_TAG 21af82a64fd5860d14d5ec9591bec2ffd5d662d1)
FetchContent_Declare(residency GIT_REPOSITORY https://github.com/Kronuz/residency.git GIT_TAG cdc5236dec435bdc453f69f1e9d7cbfdb353bbdf)
# Stored records use zstd alone.
set(COMPRESSORS_DEFLATE OFF CACHE BOOL "" FORCE)
set(COMPRESSORS_LZ4 OFF CACHE BOOL "" FORCE)
FetchContent_Declare(compressors GIT_REPOSITORY https://github.com/Kronuz/compressors.git GIT_TAG ed590c87201afd270afbfbb2d58d5d467c6d23b6)
# Segments compress stored records with zstd: the pinned release makes their bytes the same
# on every platform, whatever zstd the system has.
set(COMPRESSORS_ZSTD_PINNED ON CACHE BOOL "" FORCE)
FetchContent_Declare(fusefilter GIT_REPOSITORY https://github.com/Kronuz/fusefilter.git GIT_TAG 28f4241f22b27aa68ff92cdddc66b124e71f3298)
# Index I/O goes through kronuz::journal::IO, from the completion library.
FetchContent_Declare(completion GIT_REPOSITORY https://github.com/Kronuz/completion.git GIT_TAG 773454495e73361d231ec4dc59ad72b9b8f6163d)
# Background merges' default threaded executor: one pool per node, a task group per index.
FetchContent_Declare(threadpool GIT_REPOSITORY https://github.com/Kronuz/threadpool.git GIT_TAG 628679a2e27b563cf2709a6f35f98f1657baf7b3)
# fst is termdict's; strata pins it so the dictionary's bytes do not drift.
FetchContent_MakeAvailable(fst task hashes crc32c residency varint termdict compressors fusefilter completion threadpool)

# strata::core: the lifecycle (index, segments, manifests, merges, checkpoints, budgets, files),
# the keys and stored sections, and the document layout. Search is prospect
# (https://github.com/Kronuz/prospect), a separate library on top of it.
add_library(strata_core INTERFACE)
target_include_directories(strata_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(strata_core INTERFACE cxx_std_20)
target_link_libraries(strata_core INTERFACE hashes::hashes crc32c::crc32c residency::residency varint::varint termdict::termdict fusefilter::fusefilter compressors::compressors completion::completion task::task threadpool::threadpool)
add_library(strata::core ALIAS strata_core)

# Core links none of prospect's libraries, directly or through the libraries it links (fst reaches it
# through termdict; docset, bitpack, analysis, unicode, snowball are search's alone).
function(strata_check_links target forbidden)
  set(seen "")
  set(queue "${target}")
  list(LENGTH queue pending)
  while(pending GREATER 0)
    list(POP_FRONT queue name)
    list(LENGTH queue pending)
    if(NOT TARGET "${name}")
      continue()
    endif()
    get_target_property(real "${name}" ALIASED_TARGET)
    if(real)
      set(name "${real}")
    endif()
    if(name IN_LIST seen)
      continue()
    endif()
    list(APPEND seen "${name}")
    get_target_property(links "${name}" INTERFACE_LINK_LIBRARIES)
    if(links)
      foreach(link IN LISTS links)
        string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" link "${link}")
        list(APPEND queue "${link}")
      endforeach()
    endif()
    list(LENGTH queue pending)
  endwhile()
  foreach(name IN LISTS forbidden)
    if(name IN_LIST seen)
      message(FATAL_ERROR "${target} links ${name}, a search-only library (links: ${seen})")
    endif()
  endforeach()
  message(STATUS "${target} links: ${seen}")
endfunction()
strata_check_links(strata_core "docset;bitpack;analysis;unicode;snowball")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(files_test test/files_test.cc)
  target_link_libraries(files_test PRIVATE strata::core)
  target_compile_definitions(files_test PRIVATE STRATA_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
  add_test(NAME files COMMAND files_test)
  add_executable(merge_policy_test test/merge_policy_test.cc)
  target_link_libraries(merge_policy_test PRIVATE strata::core)
  add_test(NAME merge_policy COMMAND merge_policy_test)
  add_executable(faultfs_test test/faultfs_test.cc)
  target_link_libraries(faultfs_test PRIVATE strata::core)
  add_test(NAME faultfs COMMAND faultfs_test)
  add_executable(core_kv_test test/core_kv_test.cc)
  target_link_libraries(core_kv_test PRIVATE strata::core)
  add_test(NAME core_kv COMMAND core_kv_test)
  # Fuzz targets: each target's checked-in corpus replays as a test in every build; with
  # STRATA_FUZZ (Clang) the libFuzzer binaries build too.
  option(STRATA_FUZZ "Build the libFuzzer targets (Clang)" OFF)
  foreach(target files)
    add_executable(fuzz_${target}_replay fuzz/fuzz_${target}.cc fuzz/replay.cc)
    target_link_libraries(fuzz_${target}_replay PRIVATE strata::core)
    add_test(NAME fuzz_${target}_replay COMMAND fuzz_${target}_replay "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${target}")
    if(STRATA_FUZZ)
      add_executable(fuzz_${target} fuzz/fuzz_${target}.cc)
      target_link_libraries(fuzz_${target} PRIVATE strata::core)
      target_compile_options(fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
      target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
  endforeach()
  # The boundary (docs/design/core-split.md): every core header compiled alone, in a translation
  # unit of its own, with only strata::core's include root and libraries. core_boundary builds
  # with everything else, so a header that is not self-contained or reaches outside core fails
  # the build; its ctest entry rebuilds it, and core_boundary_negative proves the check can fail.
  file(GLOB core_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/include/strata/core/*.hh")
  set(core_boundary_sources "")
  foreach(header ${core_headers})
    string(REPLACE "/" "_" unit "${header}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/core_boundary/${unit}.cc" CONTENT "#include \"${header}\"\n")
    list(APPEND core_boundary_sources "${CMAKE_CURRENT_BINARY_DIR}/core_boundary/${unit}.cc")
  endforeach()
  set_source_files_properties(${core_boundary_sources} PROPERTIES GENERATED TRUE)
  add_library(core_boundary OBJECT ${core_boundary_sources})
  target_link_libraries(core_boundary PRIVATE strata::core)
  add_test(NAME core_boundary COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --config $<CONFIG> --target core_boundary)
  # A search header is not found from core's include root.
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/core_boundary/negative.cc" CONTENT "#include \"prospect/format.hh\"\n")
  add_library(core_boundary_negative OBJECT EXCLUDE_FROM_ALL "${CMAKE_CURRENT_BINARY_DIR}/core_boundary/negative.cc")
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/core_boundary/negative.cc" PROPERTIES GENERATED TRUE)
  target_link_libraries(core_boundary_negative PRIVATE strata::core)
  add_test(NAME core_boundary_negative COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --config $<CONFIG> --target core_boundary_negative)
  set_tests_properties(core_boundary_negative PROPERTIES PASS_REGULAR_EXPRESSION "prospect/format.hh.* file not found|prospect/format.hh.*No such file")
  set_tests_properties(core_boundary core_boundary_negative PROPERTIES RUN_SERIAL TRUE)
  add_test(NAME core_includes COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/include/strata/core -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_includes.cmake)
  add_test(NAME core_includes_negative COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/core_includes_bad -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_includes.cmake)
  set_tests_properties(core_includes_negative PROPERTIES PASS_REGULAR_EXPRESSION "\\.\\./search/exact\\.hh")
  # The key-value benchmark's strata driver (bench/kv, rocksdb-survey.md section 4): the document
  # layout on strata::core.
  add_executable(kv_strata bench/kv/kv_strata.cc)
  target_link_libraries(kv_strata PRIVATE strata::core)
  # format-14-kv's micro-prototypes (bench/kv/proto): block layouts and codecs, the decoded-block
  # cache, the buffer's sorted view, and the current buffer's bytes per record. Off by default, so
  # no other build fetches or looks for what they use; LZ4 is the system's, as strata links none.
  option(STRATA_KV_PROTO "Build format-14-kv's micro-prototypes (bench/kv/proto)" OFF)
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND STRATA_KV_PROTO)
  find_path(STRATA_PROTO_LZ4_INCLUDE lz4.h)
  find_library(STRATA_PROTO_LZ4 NAMES lz4 liblz4)
  if(STRATA_PROTO_LZ4_INCLUDE AND STRATA_PROTO_LZ4)
    add_executable(kv_proto_blocks EXCLUDE_FROM_ALL bench/kv/proto/blocks.cc)
    target_include_directories(kv_proto_blocks PRIVATE ${STRATA_PROTO_LZ4_INCLUDE})
    target_link_libraries(kv_proto_blocks PRIVATE termdict::termdict zstd::libzstd ${STRATA_PROTO_LZ4})
    target_compile_features(kv_proto_blocks PRIVATE cxx_std_20)
    add_executable(kv_proto_cached EXCLUDE_FROM_ALL bench/kv/proto/cached.cc)
    target_include_directories(kv_proto_cached PRIVATE ${STRATA_PROTO_LZ4_INCLUDE})
    target_link_libraries(kv_proto_cached PRIVATE termdict::termdict zstd::libzstd ${STRATA_PROTO_LZ4})
    target_compile_features(kv_proto_cached PRIVATE cxx_std_20)
  endif()
  find_package(Threads REQUIRED)
  add_executable(kv_proto_cache EXCLUDE_FROM_ALL bench/kv/proto/cache.cc)
  target_link_libraries(kv_proto_cache PRIVATE Threads::Threads)
  target_compile_features(kv_proto_cache PRIVATE cxx_std_20)
  # cpp-btree (Apache 2.0) is the buffer prototype's alone; it has no CMake project, so making it
  # available only fetches its headers.
  FetchContent_Declare(cpp_btree GIT_REPOSITORY https://github.com/Kronuz/cpp-btree.git GIT_TAG dd984d92540674d0794ce338712691ae9afa3d10)
  FetchContent_MakeAvailable(cpp_btree)
  add_executable(kv_proto_buffer EXCLUDE_FROM_ALL bench/kv/proto/buffer.cc)
  target_include_directories(kv_proto_buffer PRIVATE ${cpp_btree_SOURCE_DIR})
  target_compile_features(kv_proto_buffer PRIVATE cxx_std_20)
  add_executable(kv_proto_bufmem EXCLUDE_FROM_ALL bench/kv/proto/bufmem.cc)
  target_link_libraries(kv_proto_bufmem PRIVATE strata::core)
endif()
