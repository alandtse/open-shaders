if(DX12_NATIVE_SCENE_DXC)
    set(NATIVE_LIGHTING_DXIL_DIR "${CMAKE_CURRENT_BINARY_DIR}/NativeLighting")
    set(NATIVE_LIGHTING_DXIL "")
    foreach(_mode IN ITEMS Direct DirectReverseZ)
        set(_output "${NATIVE_LIGHTING_DXIL_DIR}/${_mode}.dxil")
        set(_defines "")
        if(_mode STREQUAL "DirectReverseZ")
            set(_defines -D REVERSE_Z=1)
        endif()
        add_custom_command(
            OUTPUT "${_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${NATIVE_LIGHTING_DXIL_DIR}"
            COMMAND "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges ${_defines}
                -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
                -I "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders"
                -Fo "${_output}"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Lighting/Shaders/NativeLighting/Direct.hlsl6"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Lighting/Shaders/NativeLighting/Direct.hlsl6"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Lighting/Shaders/NativeLighting/ShadowCull.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Lighting/Shaders/NativeLighting/ValidityLab.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/materials.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/NativeTraceDensity.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/PBRMath.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/LightingCommon.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/BRDF.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/Math.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/ReverseZ.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/FrameBuffer.hlsli"
            VERBATIM
        )
        list(APPEND NATIVE_LIGHTING_DXIL "${_output}")
    endforeach()
    add_custom_target(NativeLightingDXIL DEPENDS ${NATIVE_LIGHTING_DXIL})
    add_dependencies(${PROJECT_NAME} NativeLightingDXIL)
    register_feature_payload(NativeLighting FILES ${NATIVE_LIGHTING_DXIL} DESTINATION "Shaders/NativeLighting")
    install(FILES ${NATIVE_LIGHTING_DXIL} DESTINATION "Shaders/NativeLighting" COMPONENT Shaders)
endif()
