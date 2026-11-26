# Native Scene SM6.5 RayQuery. FXC still handles only early DX11 depth capture.
if(NOT DEFINED NATIVE_SHADER_SOURCE_ROOT)
    set(NATIVE_SHADER_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
find_program(
    NATIVE_SHADOW_DXC
    NAMES dxc.exe dxc
    HINTS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
)
if(NATIVE_SHADOW_DXC)
    set(_shadow_source
        "${NATIVE_SHADER_SOURCE_ROOT}/features/DX12 Native Scene/Shaders/DX12NativeScene/ShadowOffload.hlsl6"
    )
    set(_shadow_offload_contract
        "${NATIVE_SHADER_SOURCE_ROOT}/package/Shaders/Common/NativeShadowOffload.hlsli"
    )
    set(_shadow_dir "${CMAKE_CURRENT_BINARY_DIR}/DX12NativeScene")
    set(_shadow_dxil "${_shadow_dir}/ShadowOffload.dxil")
    add_custom_command(
        OUTPUT "${_shadow_dxil}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_shadow_dir}"
        COMMAND
            "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3
            ${NATIVE_SHADER_VALIDATION_FLAGS} -I
            "${NATIVE_SHADER_SOURCE_ROOT}/features/DX12 Native Scene/Shaders/DX12NativeScene"
            -I "${NATIVE_SHADER_SOURCE_ROOT}/package/Shaders"
            -Fo "${_shadow_dxil}" "${_shadow_source}"
        DEPENDS "${_shadow_source}" "${_shadow_offload_contract}"
        VERBATIM
    )
    add_custom_target(DX12NativeShadowDXIL DEPENDS "${_shadow_dxil}")
    add_dependencies(${PROJECT_NAME} DX12NativeShadowDXIL)
    if(DEFINED AIO_DIR)
        set(_shadow_aio_dxil
            "${AIO_DIR}/Shaders/DX12NativeScene/ShadowOffload.dxil"
        )
        add_custom_command(
            OUTPUT "${_shadow_aio_dxil}"
            COMMAND
                ${CMAKE_COMMAND} -E make_directory
                "${AIO_DIR}/Shaders/DX12NativeScene"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different "${_shadow_dxil}"
                "${_shadow_aio_dxil}"
            DEPENDS "${_shadow_dxil}"
            COMMENT "Staging DXIL ShadowOffload.dxil into the AIO shader tree"
            VERBATIM
        )
        add_custom_target(DX12NativeShadowAIO DEPENDS "${_shadow_aio_dxil}")
        add_dependencies(DX12NativeShadowAIO DX12NativeShadowDXIL)
        if(TARGET prepare_shaders)
            add_dependencies(DX12NativeShadowAIO prepare_shaders)
        endif()
        if(TARGET PREPARE_AIO)
            add_dependencies(DX12NativeShadowAIO PREPARE_AIO)
        endif()
    endif()
    install(
        FILES "${_shadow_dxil}"
        DESTINATION "Shaders/DX12NativeScene"
        COMPONENT Shaders
    )
else()
    message(
        WARNING
        "DXC not found; Native Scene raw shadowmask bytecode unavailable"
    )
endif()
