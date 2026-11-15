if(DX12_NATIVE_SCENE_DXC)
    set(NATIVE_COMPOSITE_DXIL_DIR "${CMAKE_CURRENT_BINARY_DIR}/NativeComposite")
    set(NATIVE_COMPOSITE_DXIL "${NATIVE_COMPOSITE_DXIL_DIR}/AddLighting.dxil")
    set(NATIVE_COMPOSITE_SHADOW_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/ShadowLayer.dxil"
    )
    set(NATIVE_COMPOSITE_PBR_INPUT_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/NativePBRInput.dxil"
    )
    set(NATIVE_COMPOSITE_PBR_SURFACE_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/NativePBRSurfaceEval.dxil"
    )
    set(NATIVE_COMPOSITE_PBR_FINAL_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/NativePBRFinalComposite.dxil"
    )
    add_custom_command(
        OUTPUT "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}" "${NATIVE_COMPOSITE_PBR_INPUT_DXIL}" "${NATIVE_COMPOSITE_PBR_SURFACE_DXIL}" "${NATIVE_COMPOSITE_PBR_FINAL_DXIL}"
        COMMAND
            ${CMAKE_COMMAND} -E make_directory "${NATIVE_COMPOSITE_DXIL_DIR}"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${NATIVE_COMPOSITE_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/AddLighting.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${NATIVE_COMPOSITE_SHADOW_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ShadowLayer.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${NATIVE_COMPOSITE_PBR_INPUT_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRInput.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
            -Fo "${NATIVE_COMPOSITE_PBR_SURFACE_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRSurfaceEval.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
            -Fo "${NATIVE_COMPOSITE_PBR_FINAL_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRFinalComposite.hlsl6"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRFinalComposite.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/AddLighting.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ShadowLayer.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRInput.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRSurfaceEval.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRMaterial.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRBRDF.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ValidityPolicy.hlsli"
        VERBATIM
    )
    add_custom_target(
        NativeCompositeDXIL
        DEPENDS "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}" "${NATIVE_COMPOSITE_PBR_INPUT_DXIL}" "${NATIVE_COMPOSITE_PBR_SURFACE_DXIL}" "${NATIVE_COMPOSITE_PBR_FINAL_DXIL}"
    )
    add_dependencies(${PROJECT_NAME} NativeCompositeDXIL)
    register_feature_payload(NativeComposite FILES "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}" "${NATIVE_COMPOSITE_PBR_INPUT_DXIL}" "${NATIVE_COMPOSITE_PBR_SURFACE_DXIL}" "${NATIVE_COMPOSITE_PBR_FINAL_DXIL}" DESTINATION "Shaders/NativeComposite")
    install(
        FILES "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}" "${NATIVE_COMPOSITE_PBR_INPUT_DXIL}" "${NATIVE_COMPOSITE_PBR_SURFACE_DXIL}" "${NATIVE_COMPOSITE_PBR_FINAL_DXIL}"
        DESTINATION "Shaders/NativeComposite"
        COMPONENT Shaders
    )
endif()
