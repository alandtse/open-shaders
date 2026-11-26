function(add_dx12_shadow_variants prefix folder full_source offload_source full_name offload_name)
    set(source_dir "${NATIVE_SHADER_SOURCE_ROOT}/features/${folder}/Shaders/${prefix}")
    set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/${prefix}")
    foreach(retired IN ITEMS "${full_name}" "${full_name}Medium" "${full_name}Potato" "${offload_name}Quality" "${offload_name}Performance")
        file(REMOVE "${output_dir}/${retired}.dxil")
        if(DEFINED AIO_DIR)
            file(REMOVE "${AIO_DIR}/Shaders/${prefix}/${retired}.dxil")
        endif()
    endforeach()
    set(outputs)
    set(staged)
    foreach(mode IN ITEMS 1 3 2)
        if(mode EQUAL 1)
            set(name "${full_name}Quality")
            set(source "${full_source}")
        elseif(mode EQUAL 3)
            set(name "${full_name}Low")
            set(source "${full_source}")
        else()
            set(name "${offload_name}")
            set(source "${offload_source}")
        endif()
        set(output "${output_dir}/${name}.dxil")
        add_custom_command(
            OUTPUT "${output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
            COMMAND "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3
                ${NATIVE_SHADER_VALIDATION_FLAGS} ${ARGN} -D NATIVE_SHADOW_QUALITY=${mode}
                -I "${source_dir}" -I "${NATIVE_SHADER_SOURCE_ROOT}/package/Shaders"
                -Fo "${output}" "${source_dir}/${source}"
            DEPENDS "${source_dir}/${source}" ${DX12_VARIANT_DEPENDENCIES}
            VERBATIM
        )
        list(APPEND outputs "${output}")
        if(DEFINED AIO_DIR)
            set(destination "${AIO_DIR}/Shaders/${prefix}/${name}.dxil")
            add_custom_command(
                OUTPUT "${destination}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders/${prefix}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${output}" "${destination}"
                DEPENDS "${output}"
                VERBATIM
            )
            list(APPEND staged "${destination}")
        endif()
    endforeach()
    set(DX12_VARIANT_OUTPUTS "${outputs}" PARENT_SCOPE)
    set(DX12_VARIANT_STAGED "${staged}" PARENT_SCOPE)
    install(FILES ${outputs} DESTINATION "Shaders/${prefix}" COMPONENT Shaders)
endfunction()
