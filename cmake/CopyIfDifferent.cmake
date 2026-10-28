function(append_copy_if_different _cmds _srcs _src_base _dst_base)
    foreach(_src IN LISTS ${_srcs})
        file(RELATIVE_PATH _rel "${_src_base}" "${_src}")
        set(_dst "${_dst_base}/${_rel}")
        get_filename_component(_dst_dir "${_dst}" DIRECTORY)
        list(
            APPEND ${_cmds}
            COMMAND
            ${CMAKE_COMMAND}
            -E
            make_directory
            "${_dst_dir}"
            COMMAND
            ${CMAKE_COMMAND}
            -E
            copy_if_different
            "${_src}"
            "${_dst}"
        )
    endforeach()
    set(${_cmds} ${${_cmds}} PARENT_SCOPE)
endfunction()
