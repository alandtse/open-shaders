# Content-based sync of AIO_DIR/Shaders into a deployed game install's
# Shaders folder. A file whose content is unchanged keeps its existing
# mtime, so the runtime shader disk cache and FileWatcher never see a
# spurious "source newer than cache" event for a shader that didn't change.
#
# Required: SRC_DIR (staged AIO/Shaders), DST_DIR (deployed Shaders folder).

if(NOT DEFINED SRC_DIR)
    message(FATAL_ERROR "SyncShaderDeploy.cmake requires SRC_DIR")
endif()

if(NOT DEFINED DST_DIR)
    message(FATAL_ERROR "SyncShaderDeploy.cmake requires DST_DIR")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/PreservedRuntimePaths.cmake")
read_preserved_runtime_prefixes(_preserve_runtime_prefixes)

file(MAKE_DIRECTORY "${DST_DIR}")

file(GLOB_RECURSE _src_files LIST_DIRECTORIES FALSE "${SRC_DIR}/*")
set(_src_rels)
foreach(_src IN LISTS _src_files)
    file(RELATIVE_PATH _rel "${SRC_DIR}" "${_src}")
    list(APPEND _src_rels "${_rel}")
    set(_dst "${DST_DIR}/${_rel}")
    get_filename_component(_dst_dir "${_dst}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dst_dir}")
    # In-process, not a subprocess per file. ONLY_IF_DIFFERENT does a real
    # content comparison; INPUT_MAY_BE_RECENT retries a source file that's
    # briefly locked right after being (re)written. A failure here must not
    # be silent, since it would leave the stamp marked done while the
    # deployed file is actually stale (e.g. a destination held open by a
    # running game).
    file(
        COPY_FILE "${_src}" "${_dst}"
        ONLY_IF_DIFFERENT
        INPUT_MAY_BE_RECENT
        RESULT _copy_result
    )
    if(NOT _copy_result EQUAL 0)
        message(FATAL_ERROR "Failed to deploy shader file: ${_src} -> ${_dst}: ${_copy_result}")
    endif()
endforeach()

# Remove deployed files with no matching staged source (renamed/deleted
# shaders), mirroring CleanupStaleEntries.cmake's stale-removal on the
# other hop.
file(GLOB_RECURSE _dst_files LIST_DIRECTORIES FALSE "${DST_DIR}/*")
foreach(_existing IN LISTS _dst_files)
    file(RELATIVE_PATH _rel "${DST_DIR}" "${_existing}")
    is_preserved_runtime_path(_preserved_runtime_path "${_rel}" "${_preserve_runtime_prefixes}")
    if(_preserved_runtime_path)
        continue()
    endif()
    list(FIND _src_rels "${_rel}" _idx)
    if(_idx EQUAL -1)
        file(REMOVE "${_existing}")
    endif()
endforeach()
