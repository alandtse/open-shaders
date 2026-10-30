# Stages package and feature shaders into AIO/Shaders and defines prepare_shaders and
# validate_changed. Requires append_copy_if_different (CopyIfDifferent.cmake), AIO_DIR,
# AIO_LOCAL_FEATURE_PATHS, HLSL_FILES and the FFX/Streamline runtime variables.

# Only copy shaders when HLSL files change; copy individually so unchanged
# files do not get their timestamps updated.
file(
    GLOB_RECURSE _package_shaders
    LIST_DIRECTORIES FALSE
    "${CMAKE_SOURCE_DIR}/package/Shaders/*"
)
# Exclude test files from production packages
list(FILTER _package_shaders EXCLUDE REGEX "/Tests/")
set(_shader_copy_cmds)

# All (locally deployed) features' shaders are copied — cross-feature
# #includes need the full tree at compile time (e.g. DistantTree.hlsl
# pulls IBL/IBL.hlsli). Effects11 is excluded via AIO_LOCAL_FEATURE_PATHS
# when ENABLE_EFFECTS11 is off, since nothing else includes its shaders.
# Written to a file rather than passed as a `-D` list, same reason as
# FEATURE_PATHS_FILE above (Ninja mis-quotes it).
set(_feature_shader_paths "${FFX_RUNTIME_SHADER_ROOT}")
foreach(_fpath IN LISTS AIO_LOCAL_FEATURE_PATHS)
    if(EXISTS "${_fpath}/Shaders")
        list(APPEND _feature_shader_paths "${_fpath}/Shaders")
    endif()
endforeach()
set(_feature_shader_paths_list_file
    "${CMAKE_CURRENT_BINARY_DIR}/feature_shader_paths.txt"
)
string(
    REPLACE ";"
    "\n"
    _feature_shader_paths_file_content
    "${_feature_shader_paths}"
)
file(
    WRITE "${_feature_shader_paths_list_file}"
    "${_feature_shader_paths_file_content}"
)

set(_preserve_runtime_args)
if(SKIP_RUNTIME_DOWNLOADS)
    set(_preserve_runtime_prefixes_file
        "${CMAKE_CURRENT_BINARY_DIR}/preserve_runtime_prefixes.txt"
    )
    set(_runtime_payload_directories
        "${FFX_RUNTIME_RELATIVE_DIRECTORY}"
        "${STREAMLINE_RUNTIME_RELATIVE_DIRECTORY}"
        "${STREAMLINE_RUNTIME_DX12_RELATIVE_DIRECTORY}"
    )
    string(
        REPLACE "Shaders/"
        ""
        _runtime_payload_prefixes
        "${_runtime_payload_directories}"
    )
    string(
        REPLACE ";"
        "\n"
        _runtime_payload_prefixes
        "${_runtime_payload_prefixes}"
    )
    file(
        WRITE "${_preserve_runtime_prefixes_file}"
        "${_runtime_payload_prefixes}"
    )
    list(
        APPEND _preserve_runtime_args
        -DPRESERVE_RUNTIME_PREFIXES_FILE="${_preserve_runtime_prefixes_file}"
    )
endif()

set(_shader_cleanup_args
    -DMODE=SHADERS
    -DAIO_DIR="${AIO_DIR}"
    -DSOURCE_DIR="${CMAKE_SOURCE_DIR}"
    -DFEATURE_SHADER_PATHS_FILE="${_feature_shader_paths_list_file}"
    ${_preserve_runtime_args}
)
list(
    APPEND _shader_copy_cmds
    COMMAND
    ${CMAKE_COMMAND}
    ${_shader_cleanup_args}
    -P
    "${CMAKE_SOURCE_DIR}/cmake/CleanupStaleEntries.cmake"
)

append_copy_if_different(_shader_copy_cmds _package_shaders "${CMAKE_SOURCE_DIR}/package/Shaders" "${AIO_DIR}/Shaders")
append_copy_if_different(_shader_copy_cmds FFX_RUNTIME_FILES "${FFX_RUNTIME_SHADER_ROOT}" "${AIO_DIR}/Shaders")
# Streamline-Runtime.cmake's payload is a build-tree dir, not a FEATURE_PATH, so
# the glob below never discovers it -- wire it in explicitly, same as FFX above.
append_copy_if_different(_shader_copy_cmds STREAMLINE_RUNTIME_FILES "${STREAMLINE_RUNTIME_DIRECTORY}" "${AIO_DIR}/${STREAMLINE_RUNTIME_RELATIVE_DIRECTORY}")
append_copy_if_different(_shader_copy_cmds STREAMLINE_RUNTIME_DX12_FILES "${STREAMLINE_RUNTIME_DX12_DIRECTORY}" "${AIO_DIR}/${STREAMLINE_RUNTIME_DX12_RELATIVE_DIRECTORY}")

# Accumulated below and added to DEPENDS -- HLSL_FILES alone missed non-shader
# feature payload (e.g. bundled DLLs), silently pinning stale binaries forever.
set(_all_feature_shader_files)
foreach(_fpath IN LISTS AIO_LOCAL_FEATURE_PATHS)
    if(EXISTS "${_fpath}/Shaders")
        file(
            GLOB_RECURSE _feat_shaders
            LIST_DIRECTORIES FALSE
            "${_fpath}/Shaders/*"
        )
        list(FILTER _feat_shaders EXCLUDE REGEX "/Tests/")
        append_copy_if_different(_shader_copy_cmds _feat_shaders "${_fpath}/Shaders" "${AIO_DIR}/Shaders")
        list(APPEND _all_feature_shader_files ${_feat_shaders})
    endif()
endforeach()

add_custom_command(
    OUTPUT copy_shaders.stamp
    COMMAND
        ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders"
        ${_shader_copy_cmds}
    COMMAND ${CMAKE_COMMAND} -E touch copy_shaders.stamp
    DEPENDS
        ${HLSL_FILES}
        ${FFX_RUNTIME_FILES}
        ${STREAMLINE_RUNTIME_FILES}
        ${STREAMLINE_RUNTIME_DX12_FILES}
        ${_package_shaders}
        ${_all_feature_shader_files}
        ${CMAKE_SOURCE_DIR}/cmake/CleanupStaleEntries.cmake
        ${CMAKE_SOURCE_DIR}/cmake/PreservedRuntimePaths.cmake
    COMMENT "Copying changed shaders into AIO/Shaders"
)

# Standalone target for preparing shaders for CI validation
# This allows shader validation to run without waiting for the full build
add_custom_target(
    prepare_shaders
    DEPENDS copy_shaders.stamp
    COMMENT "Preparing shaders for validation"
)

# Incremental local validation: compile only the shaders affected by the
# current working-tree diff (vs HEAD). Mirrors the CI incremental path but
# derives the change set from git instead of a precomputed list. Override
# the config with -DVALIDATE_CHANGED_CONFIG=... (defaults to Flatrim).
set(VALIDATE_CHANGED_CONFIG
    "${CMAKE_SOURCE_DIR}/.github/configs/shader-validation.yaml"
    CACHE FILEPATH
    "hlslkit config used by the validate_changed target"
)
add_custom_target(
    validate_changed
    COMMAND
        ${CMAKE_COMMAND} -E env python
        "${CMAKE_SOURCE_DIR}/tools/validate_changed_shaders.py" --repo-root
        "${CMAKE_SOURCE_DIR}" -- hlslkit-compile --shader-dir
        "${AIO_DIR}/Shaders" --output-dir "${CMAKE_BINARY_DIR}/ShaderCache"
        --config "${VALIDATE_CHANGED_CONFIG}" --max-warnings 0
        --suppress-warnings X1519
    DEPENDS prepare_shaders
    USES_TERMINAL
    COMMENT "Validating only shaders affected by working-tree changes"
)
