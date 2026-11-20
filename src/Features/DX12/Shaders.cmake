find_program(DX12_NATIVE_DXC NAMES dxc.exe dxc HINTS
    "$ENV{WindowsSdkVerBinPath}/x64"
    "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")

if(DX12_NATIVE_DXC)
    set(_dx12_native_dir "${CMAKE_CURRENT_BINARY_DIR}/DX12NativeComposite")
    set(_dx12_mask "${_dx12_native_dir}/PreparedShadowMask.dxil")
    set(_dx12_composite "${_dx12_native_dir}/RasterShadowComposite.dxil")
    foreach(_shader IN ITEMS PreparedShadowMask RasterShadowComposite)
        add_custom_command(
            OUTPUT "${_dx12_native_dir}/${_shader}.dxil"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_dx12_native_dir}"
            COMMAND "${DX12_NATIVE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
                -Fo "${_dx12_native_dir}/${_shader}.dxil"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Composite/Shaders/${_shader}.hlsl6"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Composite/Shaders/${_shader}.hlsl6"
            VERBATIM)
    endforeach()
    add_custom_target(DX12NativeCompositeDXIL DEPENDS "${_dx12_mask}" "${_dx12_composite}")
    add_dependencies(${PROJECT_NAME} DX12NativeCompositeDXIL)
    register_feature_payload("DX12 Native Composite"
        FILES "${_dx12_mask}" "${_dx12_composite}"
        DESTINATION "Shaders/DX12NativeComposite")
else()
    message(WARNING "DXC not found; native shadow mask and composite remain disabled")
endif()
