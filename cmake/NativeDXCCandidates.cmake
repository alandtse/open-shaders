if(NOT DX12_NATIVE_SCENE_DXC)
    add_custom_target(
        native_dxc_candidates
        COMMAND
            ${CMAKE_COMMAND} -E echo "DXC is required for native_dxc_candidates"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
else()
    set(_native_candidate_dir
        "${CMAKE_CURRENT_BINARY_DIR}/NativeDXIL/Candidates"
    )
    set(_native_capture_source
        "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/CaptureDepth.cs.hlsl"
    )
    set(_native_category_source
        "${CMAKE_CURRENT_SOURCE_DIR}/features/Upscaling/Shaders/Upscaling/NeuralRendering/CategoryAlphaCS.hlsl"
    )
    set(_native_capture_output
        "${_native_candidate_dir}/DX12NativeScene/CaptureDepth.dxil"
    )
    set(_native_category_output
        "${_native_candidate_dir}/Upscaling/CategoryAlpha.dxil"
    )

    add_custom_command(
        OUTPUT "${_native_capture_output}"
        COMMAND
            ${CMAKE_COMMAND} -E make_directory
            "${_native_candidate_dir}/DX12NativeScene"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${_native_capture_output}" "${_native_capture_source}"
        DEPENDS "${_native_capture_source}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${_native_category_output}"
        COMMAND
            ${CMAKE_COMMAND} -E make_directory
            "${_native_candidate_dir}/Upscaling"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders" -Fo
            "${_native_category_output}" "${_native_category_source}"
        DEPENDS
            "${_native_category_source}"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/NeuralRenderingCategory.hlsli"
        VERBATIM
    )
    add_custom_target(
        native_dxc_candidates
        DEPENDS "${_native_capture_output}" "${_native_category_output}"
    )
endif()

add_custom_target(native_shader_pass_2)
add_dependencies(native_shader_pass_2 native_shaders_only native_dxc_candidates)
