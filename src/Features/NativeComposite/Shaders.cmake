if(DX12_NATIVE_SCENE_DXC)
    set(NATIVE_COMPOSITE_DXIL_DIR "${CMAKE_CURRENT_BINARY_DIR}/NativeComposite")
    set(NATIVE_COMPOSITE_DXIL "${NATIVE_COMPOSITE_DXIL_DIR}/AddLighting.dxil")
    set(NATIVE_COMPOSITE_SHADOW_DXIL "${NATIVE_COMPOSITE_DXIL_DIR}/ShadowLayer.dxil")
    add_custom_command(
        OUTPUT "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NATIVE_COMPOSITE_DXIL_DIR}"
        COMMAND "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${NATIVE_COMPOSITE_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/AddLighting.hlsl6"
        COMMAND "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${NATIVE_COMPOSITE_SHADOW_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ShadowLayer.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/AddLighting.hlsl6"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ShadowLayer.hlsl6"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ValidityPolicy.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/RasterCharacter.hlsli"
        VERBATIM
    )
    add_custom_target(NativeCompositeDXIL DEPENDS "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}")
    add_dependencies(${PROJECT_NAME} NativeCompositeDXIL)
    register_feature_payload(NativeComposite FILES "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}" DESTINATION "Shaders/NativeComposite")
    install(FILES "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}" DESTINATION "Shaders/NativeComposite" COMPONENT Shaders)
endif()
