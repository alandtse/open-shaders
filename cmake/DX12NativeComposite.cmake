# Native Composite uses the same DXC compiler and shader delivery as Native Scene.
if(NATIVE_SHADOW_DXC)
    set(_native_composite_source
        "${NATIVE_SHADER_SOURCE_ROOT}/features/Native Composite/Shaders/NativeComposite/CompositeOffload.hlsl6"
    )
    set(_native_composite_dir "${CMAKE_CURRENT_BINARY_DIR}/NativeComposite")
    set(_native_composite_dxil "${_native_composite_dir}/CompositeOffload.dxil")
    set(_native_composite_outputs "${_native_composite_dxil}")
    set(_rt_composite_source
        "${NATIVE_SHADER_SOURCE_ROOT}/features/Native Composite/Shaders/NativeComposite/CompositeRT.hlsl6"
    )
    foreach(_quality IN ITEMS Quality Performance)
        if(_quality STREQUAL "Quality")
            set(_quality_id 1)
        else()
            set(_quality_id 3)
        endif()
        set(_rt_dxil "${_native_composite_dir}/CompositeRT${_quality}.dxil")
        add_custom_command(
            OUTPUT "${_rt_dxil}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_native_composite_dir}"
            COMMAND
                "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3
                ${NATIVE_SHADER_VALIDATION_FLAGS} -Ges -Gis -D NATIVE_RT_QUALITY=${_quality_id}
                -I "${NATIVE_SHADER_SOURCE_ROOT}/package/Shaders"
                -Fo "${_rt_dxil}" "${_rt_composite_source}"
            DEPENDS "${_rt_composite_source}" ${_shadow_contracts}
            VERBATIM
        )
        list(APPEND _native_composite_outputs "${_rt_dxil}")
    endforeach()
    add_custom_command(
        OUTPUT "${_native_composite_dxil}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_native_composite_dir}"
        COMMAND
            "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3
            ${NATIVE_SHADER_VALIDATION_FLAGS} -Ges -Gis
            -I "${NATIVE_SHADER_SOURCE_ROOT}/package/Shaders" -Fo
            "${_native_composite_dxil}" "${_native_composite_source}"
        DEPENDS "${_native_composite_source}" "${_shadow_offload_contract}"
        VERBATIM
    )
    add_custom_target(
        DX12NativeCompositeDXIL
        DEPENDS ${_native_composite_outputs}
    )
    add_dependencies(${PROJECT_NAME} DX12NativeCompositeDXIL)
    if(DEFINED AIO_DIR)
        set(_native_composite_aio_outputs)
        foreach(_dxil IN LISTS _native_composite_outputs)
            get_filename_component(_name "${_dxil}" NAME)
            set(_aio_dxil "${AIO_DIR}/Shaders/NativeComposite/${_name}")
            add_custom_command(
                OUTPUT "${_aio_dxil}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders/NativeComposite"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dxil}" "${_aio_dxil}"
                DEPENDS "${_dxil}"
                VERBATIM
            )
            list(APPEND _native_composite_aio_outputs "${_aio_dxil}")
        endforeach()
        add_custom_target(DX12NativeCompositeAIO DEPENDS ${_native_composite_aio_outputs})
        add_dependencies(DX12NativeCompositeAIO DX12NativeCompositeDXIL)
        if(TARGET prepare_shaders)
            add_dependencies(DX12NativeCompositeAIO prepare_shaders)
        endif()
        if(TARGET PREPARE_AIO)
            add_dependencies(DX12NativeCompositeAIO PREPARE_AIO)
        endif()
    endif()
    install(
        FILES ${_native_composite_outputs}
        DESTINATION "Shaders/NativeComposite"
        COMPONENT Shaders
    )
endif()
