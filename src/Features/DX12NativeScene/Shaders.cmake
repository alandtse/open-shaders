find_program(
    DX12_NATIVE_SCENE_DXC
    NAMES dxc.exe dxc
    HINTS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
)
if(DX12_NATIVE_SCENE_DXC)
    set(DX12_NATIVE_SCENE_DXIL_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/DX12NativeScene"
    )
    set(DX12_NATIVE_SCENE_DXIL "${DX12_NATIVE_SCENE_DXIL_DIR}/ShadowTrace.dxil")
    file(
        GLOB_RECURSE DX12_NATIVE_SCENE_SHADER_DEPENDENCIES
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/*.hlsli"
    )
    add_custom_command(
        OUTPUT "${DX12_NATIVE_SCENE_DXIL}"
        COMMAND
            ${CMAKE_COMMAND} -E make_directory "${DX12_NATIVE_SCENE_DXIL_DIR}"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders" -Fo
            "${DX12_NATIVE_SCENE_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/ShadowTrace.hlsl6"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/ShadowTrace.hlsl6"
            ${DX12_NATIVE_SCENE_SHADER_DEPENDENCIES}
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/PBRMath.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/LightingCommon.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/PointLightFlags.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/BRDF.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/Math.hlsli"
        VERBATIM
    )
    # Compile the isolated DX12 compute-skinning candidate with the native
    # compiler, never the legacy FXC path. Runtime dispatch remains opt-in and
    # character rays cannot observe this shader until pose parity is validated.
    set(DX12_NATIVE_CHARACTER_SKIN_DXIL
        "${DX12_NATIVE_SCENE_DXIL_DIR}/CharacterSkinning.dxil"
    )
    add_custom_command(
        OUTPUT "${DX12_NATIVE_CHARACTER_SKIN_DXIL}"
        COMMAND
            ${CMAKE_COMMAND} -E make_directory "${DX12_NATIVE_SCENE_DXIL_DIR}"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${DX12_NATIVE_CHARACTER_SKIN_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/CharacterSkinning.hlsl6"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/CharacterSkinning.hlsl6"
        VERBATIM
    )
    # Isolated character TLAS RayQuery probe: compiled only with DXC 6.5.
    set(DX12_NATIVE_CHARACTER_RAY_DXIL
        "${DX12_NATIVE_SCENE_DXIL_DIR}/CharacterRayProbe.dxil"
    )
    add_custom_command(
        OUTPUT "${DX12_NATIVE_CHARACTER_RAY_DXIL}"
        COMMAND
            ${CMAKE_COMMAND} -E make_directory "${DX12_NATIVE_SCENE_DXIL_DIR}"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${DX12_NATIVE_CHARACTER_RAY_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/CharacterRayProbe.hlsl6"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Scene/Shaders/DX12NativeScene/CharacterRayProbe.hlsl6"
        VERBATIM
    )
    add_custom_target(
        DX12NativeSceneDXIL
        DEPENDS
            "${DX12_NATIVE_SCENE_DXIL}"
            "${DX12_NATIVE_CHARACTER_SKIN_DXIL}"
            "${DX12_NATIVE_CHARACTER_RAY_DXIL}"
    )
    add_dependencies(${PROJECT_NAME} DX12NativeSceneDXIL)
    register_feature_payload(DX12NativeScene
        FILES "${DX12_NATIVE_SCENE_DXIL}" "${DX12_NATIVE_CHARACTER_SKIN_DXIL}"
            "${DX12_NATIVE_CHARACTER_RAY_DXIL}"
        DESTINATION "Shaders/DX12NativeScene"
    )
    install(
        FILES
            "${DX12_NATIVE_SCENE_DXIL}"
            "${DX12_NATIVE_CHARACTER_SKIN_DXIL}"
            "${DX12_NATIVE_CHARACTER_RAY_DXIL}"
        DESTINATION "Shaders/DX12NativeScene"
        COMPONENT Shaders
    )
else()
    message(
        WARNING
        "DXC was not found; DX12 Native Scene shadow tracing bytecode will be unavailable"
    )
endif()
