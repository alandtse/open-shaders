# Native Scene SM6.5 RayQuery. FXC still handles only early DX11 depth capture.
find_program(NATIVE_SHADOW_DXC NAMES dxc.exe dxc
    HINTS "$ENV{WindowsSdkVerBinPath}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
if(NATIVE_SHADOW_DXC)
    set(_shadow_source "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/ShadowMask.hlsl6")
    set(_shadow_density "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/NativeTraceDensity.hlsli")
    set(_shadow_dir "${CMAKE_CURRENT_BINARY_DIR}/DX12NativeScene")
    set(_shadow_dxil "${_shadow_dir}/ShadowMask.dxil")
    add_custom_command(
        OUTPUT "${_shadow_dxil}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_shadow_dir}"
        COMMAND "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3
            -I "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene"
            -Fo "${_shadow_dxil}" "${_shadow_source}"
        DEPENDS "${_shadow_source}" "${_shadow_density}"
        VERBATIM)
    add_custom_target(DX12NativeShadowDXIL DEPENDS "${_shadow_dxil}")
    add_dependencies(${PROJECT_NAME} DX12NativeShadowDXIL)
    if(DEFINED AIO_DIR)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders/DX12NativeScene"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_shadow_dxil}" "${AIO_DIR}/Shaders/DX12NativeScene/ShadowMask.dxil"
            VERBATIM)
    endif()
    install(FILES "${_shadow_dxil}" DESTINATION "Shaders/DX12NativeScene" COMPONENT Shaders)
else()
    message(WARNING "DXC not found; Native Scene raw shadowmask bytecode unavailable")
endif()
