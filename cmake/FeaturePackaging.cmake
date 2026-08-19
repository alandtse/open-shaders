function(feature_payload_component feature out_var)
    string(MAKE_C_IDENTIFIER "${feature}" _feature_key)
    set(${out_var} "FeaturePayload-${_feature_key}" PARENT_SCOPE)
endfunction()

function(register_feature_payload feature)
    cmake_parse_arguments(PAYLOAD "" "DESTINATION" "FILES" ${ARGN})
    if(PAYLOAD_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "Unknown feature payload arguments: ${PAYLOAD_UNPARSED_ARGUMENTS}"
        )
    endif()
    if(NOT PAYLOAD_DESTINATION OR NOT PAYLOAD_FILES)
        message(FATAL_ERROR "Feature payloads require DESTINATION and FILES")
    endif()

    feature_payload_component("${feature}" _component)
    install(
        FILES ${PAYLOAD_FILES}
        DESTINATION "${PAYLOAD_DESTINATION}"
        COMPONENT "${_component}"
    )

    string(MAKE_C_IDENTIFIER "${feature}" _feature_key)
    set_property(
        GLOBAL
        APPEND
        PROPERTY "FEATURE_PAYLOAD_DEPENDENCIES_${_feature_key}" ${PAYLOAD_FILES}
    )
endfunction()

function(feature_payload_dependencies feature out_var)
    string(MAKE_C_IDENTIFIER "${feature}" _feature_key)
    get_property(
        _dependencies
        GLOBAL
        PROPERTY "FEATURE_PAYLOAD_DEPENDENCIES_${_feature_key}"
    )
    set(${out_var} ${_dependencies} PARENT_SCOPE)
endfunction()

function(
    feature_package_commands
    out_var
    feature
    source
    stage
    archive
    tar_mode
)
    feature_payload_component("${feature}" _component)
    set(_commands
        COMMAND
        ${CMAKE_COMMAND}
        -E
        remove_directory
        "${stage}"
        COMMAND
        ${CMAKE_COMMAND}
        -E
        copy_directory
        "${source}"
        "${stage}"
        COMMAND
        ${CMAKE_COMMAND}
        --install
        "${CMAKE_BINARY_DIR}"
        --prefix
        "${stage}"
        --component
        "${_component}"
        COMMAND
        ${CMAKE_COMMAND}
        -E
        chdir
        "${stage}"
        ${CMAKE_COMMAND}
        -E
        tar
        ${tar_mode}
        "${archive}"
        --format=7zip
        --
        .
    )
    set(${out_var} ${_commands} PARENT_SCOPE)
endfunction()
