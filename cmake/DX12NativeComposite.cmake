# Native Composite uses the same DXC compiler and shader delivery as Native Scene.
if(NATIVE_SHADOW_DXC)
    set(_native_composite_source
        "${NATIVE_SHADER_SOURCE_ROOT}/features/Native Composite/Shaders/NativeComposite/ShadowLayer.hlsl6"
    )
    set(_native_composite_dir "${CMAKE_CURRENT_BINARY_DIR}/NativeComposite")
    set(_native_composite_dxil "${_native_composite_dir}/ShadowLayer.dxil")
    add_custom_command(
        OUTPUT "${_native_composite_dxil}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_native_composite_dir}"
        COMMAND
            "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3 ${NATIVE_SHADER_VALIDATION_FLAGS} -Ges -Gis -Fo
            "${_native_composite_dxil}" "${_native_composite_source}"
        DEPENDS "${_native_composite_source}"
        VERBATIM
    )
    add_custom_target(
        DX12NativeCompositeDXIL
        DEPENDS "${_native_composite_dxil}"
    )
    add_dependencies(${PROJECT_NAME} DX12NativeCompositeDXIL)
    if(DEFINED AIO_DIR)
        set(_native_composite_aio_dxil
            "${AIO_DIR}/Shaders/NativeComposite/ShadowLayer.dxil"
        )
        add_custom_command(
            OUTPUT "${_native_composite_aio_dxil}"
            COMMAND
                ${CMAKE_COMMAND} -E make_directory
                "${AIO_DIR}/Shaders/NativeComposite"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different
                "${_native_composite_dxil}" "${_native_composite_aio_dxil}"
            DEPENDS "${_native_composite_dxil}"
            COMMENT "Staging DXIL ShadowLayer.dxil into the AIO shader tree"
            VERBATIM
        )
        add_custom_target(
            DX12NativeCompositeAIO
            DEPENDS "${_native_composite_aio_dxil}"
        )
        add_dependencies(DX12NativeCompositeAIO DX12NativeCompositeDXIL)
        if(TARGET prepare_shaders)
            add_dependencies(DX12NativeCompositeAIO prepare_shaders)
        endif()
        if(TARGET PREPARE_AIO)
            add_dependencies(DX12NativeCompositeAIO PREPARE_AIO)
        endif()
    endif()
    install(
        FILES "${_native_composite_dxil}"
        DESTINATION "Shaders/NativeComposite"
        COMPONENT Shaders
    )
endif()
