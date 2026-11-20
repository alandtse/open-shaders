# The geometry companion is an isolated .NET executable. The SKSE plugin only
# starts it for bounded tile requests; normal C++ builds need no .NET SDK.
if(AUTO_PLUGIN_DEPLOYMENT AND WIN32)
    find_program(DX12_RESOURCES_DOTNET NAMES dotnet.exe dotnet)
    if(NOT DX12_RESOURCES_DOTNET)
        message(
            FATAL_ERROR
            "DX12 Resources deployment requires the .NET 10 SDK"
        )
    endif()
    set(_dx12_companion_project
        "${CMAKE_SOURCE_DIR}/tools/dx12-resources-prep/DX12ResourcesPrep.csproj"
    )
    set(_dx12_companion_output
        "${CMAKE_BINARY_DIR}/dx12-resources-prep/publish"
    )
    file(
        GLOB _dx12_companion_sources
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/dx12-resources-prep/*.cs"
    )
    add_custom_command(
        OUTPUT "${_dx12_companion_output}/DX12ResourcesPrep.exe"
        COMMAND
            "${DX12_RESOURCES_DOTNET}" publish "${_dx12_companion_project}"
            --configuration Release --runtime win-x64 --self-contained true
            -p:PublishSingleFile=true -p:PublishReadyToRun=false --output
            "${_dx12_companion_output}"
        DEPENDS "${_dx12_companion_project}" ${_dx12_companion_sources}
        COMMENT "Build DX12 geometry and light companion"
        VERBATIM
    )
    add_custom_target(
        BUILD_DX12_RESOURCES_COMPANION
        DEPENDS "${_dx12_companion_output}/DX12ResourcesPrep.exe"
    )
    add_custom_target(
        DEPLOY_DX12_RESOURCES_COMPANION
        DEPENDS BUILD_DX12_RESOURCES_COMPANION
    )
    foreach(_dx12_deploy_root $ENV{CommunityShadersOutputDir})
        add_custom_command(
            TARGET DEPLOY_DX12_RESOURCES_COMPANION
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E make_directory
                "${_dx12_deploy_root}/SKSE/Plugins/CommunityShaders/DX12Resources/bin"
            COMMAND
                ${CMAKE_COMMAND} -E copy_directory "${_dx12_companion_output}"
                "${_dx12_deploy_root}/SKSE/Plugins/CommunityShaders/DX12Resources/bin"
            VERBATIM
        )
    endforeach()
    if(TARGET DEPLOY_ALL)
        add_dependencies(DEPLOY_ALL DEPLOY_DX12_RESOURCES_COMPANION)
    endif()
endif()
