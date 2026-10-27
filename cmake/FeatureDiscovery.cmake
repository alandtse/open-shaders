# Discovers feature folders and the subset staged into the local AIO tree.
# Sets FEATURE_PATHS and AIO_LOCAL_FEATURE_PATHS; ENABLE_EFFECTS11 must be set first.

file(GLOB FEATURE_PATHS LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/features/*)
# Drop empty leftover feature dirs (e.g. after a rename) so they do not create
# Package-* targets or get copied into AIO / install trees as empty folders.
set(_valid_feature_paths "")
foreach(_fpath IN LISTS FEATURE_PATHS)
    if(IS_DIRECTORY "${_fpath}")
        file(GLOB_RECURSE _feat_files LIST_DIRECTORIES FALSE "${_fpath}/*")
        if(_feat_files)
            list(APPEND _valid_feature_paths "${_fpath}")
        else()
            get_filename_component(_feat_name "${_fpath}" NAME)
            message(STATUS "Skipping empty feature directory: ${_feat_name}")
        endif()
    endif()
endforeach()
set(FEATURE_PATHS ${_valid_feature_paths})
unset(_valid_feature_paths)
unset(_feat_files)

# Effects11's sources aren't compiled in when ENABLE_EFFECTS11 is off (see the
# source filter above), so staging its ini/shaders to a local dev deploy only
# produces an orphaned-ini warning at runtime, never a working feature.
set(AIO_LOCAL_FEATURE_PATHS ${FEATURE_PATHS})
if(NOT ENABLE_EFFECTS11)
    list(FILTER AIO_LOCAL_FEATURE_PATHS EXCLUDE REGEX "/Effects11$")
endif()
