# A configure that skipped the runtime download leaves the payload out of the
# source tree, so both stale-file removals must keep an already-deployed copy.

# Reads the directories a configure asked to preserve into _out_var, one per line
# (empty when none). A file, not a `-D` list: Ninja turns list semicolons into spaces.
function(read_preserved_runtime_prefixes _out_var)
    set(_prefixes)
    if(
        DEFINED PRESERVE_RUNTIME_PREFIXES_FILE
        AND EXISTS "${PRESERVE_RUNTIME_PREFIXES_FILE}"
    )
        file(STRINGS "${PRESERVE_RUNTIME_PREFIXES_FILE}" _prefixes)
    endif()
    set(${_out_var} ${_prefixes} PARENT_SCOPE)
endfunction()

# True when a Shaders-relative path sits under one of the _prefixes entries.
function(is_preserved_runtime_path _out_var _relative_path _prefixes)
    set(_preserved FALSE)
    foreach(_prefix IN LISTS _prefixes)
        string(FIND "${_relative_path}" "${_prefix}/" _prefix_at)
        if(_prefix_at EQUAL 0)
            set(_preserved TRUE)
        endif()
    endforeach()
    set(${_out_var} ${_preserved} PARENT_SCOPE)
endfunction()
