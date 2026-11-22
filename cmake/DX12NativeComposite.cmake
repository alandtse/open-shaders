# Native Composite uses the same DXC compiler and shader delivery as Native Scene.
if(NATIVE_SHADOW_DXC)
    set(_native_composite_source
        "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ShadowLayer.hlsl6"
    )
    set(_native_composite_dir "${CMAKE_CURRENT_BINARY_DIR}/NativeComposite")
    set(_native_composite_dxil "${_native_composite_dir}/ShadowLayer.dxil")
    add_custom_command(
        OUTPUT "${_native_composite_dxil}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_native_composite_dir}"
        COMMAND
            "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${_native_composite_dxil}" "${_native_composite_source}"
        DEPENDS "${_native_composite_source}"
        VERBATIM
    )
    add_custom_target(DX12NativeCompositeDXIL DEPENDS "${_native_composite_dxil}")
    add_dependencies(${PROJECT_NAME} DX12NativeCompositeDXIL)
    if(DEFINED AIO_DIR)
        add_custom_command(
            TARGET ${PROJECT_NAME}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders/NativeComposite"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_native_composite_dxil}" "${AIO_DIR}/Shaders/NativeComposite/ShadowLayer.dxil"
            VERBATIM
        )
    endif()
    install(
        FILES "${_native_composite_dxil}"
        DESTINATION "Shaders/NativeComposite"
        COMPONENT Shaders
    )
endif()
