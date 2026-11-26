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
include("${CMAKE_CURRENT_LIST_DIR}/DX12ShadowVariants.cmake")
if(NATIVE_SHADOW_DXC)
    set(DX12_VARIANT_DEPENDENCIES
        "${NATIVE_SHADER_SOURCE_ROOT}/features/DX12 Native Scene/Shaders/DX12NativeScene/ShadowTrace.hlsli"
        "${NATIVE_SHADER_SOURCE_ROOT}/features/DX12 Native Scene/Shaders/DX12NativeScene/NativeShadowSampling.hlsli"
        "${NATIVE_SHADER_SOURCE_ROOT}/package/Shaders/Common/NativeShadowQuality.hlsli"
    )
    add_dx12_shadow_variants(DX12NativeScene "DX12 Native Scene" ShadowMask.hlsl6 ShadowOffload.hlsl6 ShadowMask ShadowOffload)
    set(_shadow_dxil ${DX12_VARIANT_OUTPUTS})
    set(_shadow_aio_dxil ${DX12_VARIANT_STAGED})
    add_custom_target(DX12NativeShadowDXIL DEPENDS ${_shadow_dxil})
    add_dependencies(${PROJECT_NAME} DX12NativeShadowDXIL)
    if(DEFINED AIO_DIR)
        add_custom_target(DX12NativeShadowAIO DEPENDS ${_shadow_aio_dxil})
        add_dependencies(DX12NativeShadowAIO DX12NativeShadowDXIL)
        if(TARGET prepare_shaders)
            add_dependencies(DX12NativeShadowAIO prepare_shaders)
        endif()
        if(TARGET PREPARE_AIO)
            add_dependencies(DX12NativeShadowAIO PREPARE_AIO)
        endif()
    endif()
else()
    message(WARNING "DXC not found; Native Scene raw shadowmask bytecode unavailable")
endif()
