# Standalone (cmake -P) emitter for the authoritative Alpha/Beta feature-name
# list. Computes it with NO project()/vcpkg configure, so the CI consistency
# guard can run it in an unprivileged, cache-free job.
#
# Usage: cmake [-DOUTPUT=path] [-DFEATURES_DIR=path] -P cmake/emit-default-disabled-features.cmake
#   OUTPUT       output file (default: default-disabled-features.txt in the working dir)
#   FEATURES_DIR features root (default: <repo>/features, repo = parent of this dir)

include("${CMAKE_CURRENT_LIST_DIR}/FeatureStaging.cmake")

if(NOT DEFINED FEATURES_DIR)
    get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
    set(FEATURES_DIR "${_repo}/features")
endif()
if(NOT DEFINED OUTPUT)
    set(OUTPUT "default-disabled-features.txt")
endif()

file(GLOB_RECURSE _ini_paths LIST_DIRECTORIES false "${FEATURES_DIR}/*/Shaders/Features/*.ini")
set(_names "")
foreach(_ini IN LISTS _ini_paths)
    file(READ "${_ini}" _content)
    string(STRIP "${_content}" _content)
    feature_ini_stage("${_content}" _is_alpha _is_beta)
    if(_is_alpha OR _is_beta)
        get_filename_component(_n "${_ini}" NAME_WE)
        list(APPEND _names "${_n}")
    endif()
endforeach()

list(SORT _names)
string(REPLACE ";" "\n" _text "${_names}")
file(WRITE "${OUTPUT}" "${_text}\n")
message(STATUS "Wrote Alpha/Beta feature list (${_text}) to ${OUTPUT}")
