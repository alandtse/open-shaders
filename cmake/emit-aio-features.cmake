# Standalone (cmake -P) emitter for the authoritative AIO feature-dir list.
# Computes the partition with NO project()/vcpkg configure, so the CI consistency
# guard can run it in an unprivileged, cache-free job.
#
# Usage: cmake [-DOUTPUT=path] [-DFEATURES_DIR=path] -P cmake/emit-aio-features.cmake
#   OUTPUT       output file (default: aio-features.txt in the working dir)
#   FEATURES_DIR features root (default: <repo>/features, repo = parent of this dir)

include("${CMAKE_CURRENT_LIST_DIR}/AioPartition.cmake")

if(NOT DEFINED FEATURES_DIR)
    get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
    set(FEATURES_DIR "${_repo}/features")
endif()
if(NOT DEFINED OUTPUT)
    set(OUTPUT "aio-features.txt")
endif()

file(GLOB _feature_paths LIST_DIRECTORIES true "${FEATURES_DIR}/*")
set(_names "")
foreach(_fpath IN LISTS _feature_paths)
    if(NOT IS_DIRECTORY "${_fpath}")
        continue()
    endif()
    feature_in_aio("${_fpath}" _ok)
    if(_ok)
        get_filename_component(_n "${_fpath}" NAME)
        list(APPEND _names "${_n}")
    endif()
endforeach()

list(SORT _names)
string(REPLACE ";" "\n" _text "${_names}")
file(WRITE "${OUTPUT}" "${_text}\n")
message(STATUS "Wrote AIO feature list (${_text}) to ${OUTPUT}")
