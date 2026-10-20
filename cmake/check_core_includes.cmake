# Fails if a header in DIR includes anything but another core header or a library core depends on.
# Run: cmake -DDIR=<include/strata/core> -P check_core_includes.cmake
#
# core_boundary compiles the headers against core's include root, which does not hold search; this
# catches the spellings that root cannot, such as "../search/x.hh", which the linked root resolves
# physically into include/strata/search.
cmake_minimum_required(VERSION 3.18)  # script mode: IN_LIST needs CMP0057 set NEW
set(allowed_libraries
  completion/io.h compressor_zstd.h fusefilter.hh hashes.hh residency.hh task.hh task_group.hh
  termdict.hh threadpool.hh varint.hh crc32c/crc32c.hh)
file(GLOB headers "${DIR}/*.hh")
list(LENGTH headers count)
if(count EQUAL 0)
  message(FATAL_ERROR "no headers in ${DIR}")
endif()
set(bad "")
foreach(header IN LISTS headers)
  file(STRINGS "${header}" lines REGEX "^[ \t]*#[ \t]*include")
  foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*#[ \t]*include[ \t]*\"([^\"]*)\"")
      set(name "${CMAKE_MATCH_1}")
      if(NOT name MATCHES "^strata/core/[a-z_0-9]+\\.hh$" AND NOT name IN_LIST allowed_libraries)
        list(APPEND bad "${header}: ${line}")
      endif()
    elseif(line MATCHES "^[ \t]*#[ \t]*include[ \t]*<([^>]*)>")
      set(name "${CMAKE_MATCH_1}")
      if(name MATCHES "\\." AND NOT name IN_LIST allowed_libraries)  # the standard headers have no dot
        list(APPEND bad "${header}: ${line}")
      endif()
    else()
      list(APPEND bad "${header}: ${line}")
    endif()
  endforeach()
endforeach()
if(bad)
  list(JOIN bad "\n  " text)
  message(FATAL_ERROR "core headers include what core does not own:\n  ${text}")
endif()
message(STATUS "core includes: ${count} headers, only core and its libraries")
