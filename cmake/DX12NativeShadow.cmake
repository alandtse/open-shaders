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
    set(_shadow_contracts
        "${_shadow_offload_contract}"
        "${NATIVE_SHADER_SOURCE_ROOT}/package/Shaders/Common/NativeShadowRT.hlsli"
        "${NATIVE_SHADER_SOURCE_ROOT}/features/DX12 Native Scene/Shaders/DX12NativeScene/ShadowTrace.hlsli"
    )
    set(_shadow_rt_source
        "${NATIVE_SHADER_SOURCE_ROOT}/features/DX12 Native Scene/Shaders/DX12NativeScene/ShadowRT.hlsl6"
    )
    set(_shadow_outputs "${_shadow_dxil}")
    foreach(_quality IN ITEMS Quality Performance)
        if(_quality STREQUAL "Quality")
            set(_quality_id 1)
        else()
            set(_quality_id 3)
        endif()
        set(_rt_dxil "${_shadow_dir}/ShadowRT${_quality}.dxil")
        add_custom_command(
            OUTPUT "${_rt_dxil}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_shadow_dir}"
            COMMAND
                "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3
                ${NATIVE_SHADER_VALIDATION_FLAGS} -D NATIVE_RT_QUALITY=${_quality_id}
                -I "${NATIVE_SHADER_SOURCE_ROOT}/package/Shaders"
                -Fo "${_rt_dxil}" "${_shadow_rt_source}"
            DEPENDS "${_shadow_rt_source}" ${_shadow_contracts}
            VERBATIM
        )
        list(APPEND _shadow_outputs "${_rt_dxil}")
    endforeach()
    add_custom_command(
        OUTPUT "${_shadow_dxil}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_shadow_dir}"
        COMMAND
            "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3
            ${NATIVE_SHADER_VALIDATION_FLAGS} -I
            "${NATIVE_SHADER_SOURCE_ROOT}/features/DX12 Native Scene/Shaders/DX12NativeScene"
            -I "${NATIVE_SHADER_SOURCE_ROOT}/package/Shaders"
            -Fo "${_shadow_dxil}" "${_shadow_source}"
        DEPENDS "${_shadow_source}" ${_shadow_contracts}
        VERBATIM
    )
    add_custom_target(DX12NativeShadowDXIL DEPENDS ${_shadow_outputs})
    add_dependencies(${PROJECT_NAME} DX12NativeShadowDXIL)
    if(DEFINED AIO_DIR)
        set(_shadow_aio_outputs)
        foreach(_dxil IN LISTS _shadow_outputs)
            get_filename_component(_name "${_dxil}" NAME)
            set(_aio_dxil "${AIO_DIR}/Shaders/DX12NativeScene/${_name}")
            add_custom_command(
                OUTPUT "${_aio_dxil}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders/DX12NativeScene"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dxil}" "${_aio_dxil}"
                DEPENDS "${_dxil}"
                VERBATIM
            )
            list(APPEND _shadow_aio_outputs "${_aio_dxil}")
        endforeach()
        add_custom_target(DX12NativeShadowAIO DEPENDS ${_shadow_aio_outputs})
        add_dependencies(DX12NativeShadowAIO DX12NativeShadowDXIL)
        if(TARGET prepare_shaders)
            add_dependencies(DX12NativeShadowAIO prepare_shaders)
        endif()
        if(TARGET PREPARE_AIO)
            add_dependencies(DX12NativeShadowAIO PREPARE_AIO)
        endif()
    endif()
    install(
        FILES ${_shadow_outputs}
        DESTINATION "Shaders/DX12NativeScene"
        COMPONENT Shaders
    )
else()
    message(
        WARNING
        "DXC not found; Native Scene raw shadowmask bytecode unavailable"
    )
endif()
