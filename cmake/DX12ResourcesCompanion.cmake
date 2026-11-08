# DX12ResourcesPrep is a separate .NET producer, never compiled within Skyrim.
# Only opt-in local Windows auto-deployment builds require its SDK.
if(AUTO_PLUGIN_DEPLOYMENT AND WIN32)
    find_program(DX12_RESOURCES_DOTNET NAMES dotnet.exe dotnet)
    if(NOT DX12_RESOURCES_DOTNET)
        message(FATAL_ERROR "DX12 Resources deployment requires the .NET 10 SDK (dotnet.exe).")
    endif()
    set(_dx12_prep_project "${CMAKE_SOURCE_DIR}/tools/dx12-resources-prep/DX12ResourcesPrep.csproj")
    set(_dx12_prep_build "${CMAKE_BINARY_DIR}/dx12-resources-prep")
    set(_dx12_prep_publish "${_dx12_prep_build}/publish")
    file(GLOB_RECURSE _dx12_prep_inputs CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/dx12-resources-prep/*.cs")
    list(APPEND _dx12_prep_inputs
        "${_dx12_prep_project}"
        "${CMAKE_SOURCE_DIR}/tools/dx12-resources-prep/packages.lock.json"
        "${CMAKE_SOURCE_DIR}/tools/dx12-resources-prep/LayoutPreview.html")
    add_custom_command(
        OUTPUT "${_dx12_prep_build}/publish.stamp"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dx12_prep_build}"
        COMMAND "${DX12_RESOURCES_DOTNET}" restore "${_dx12_prep_project}"
            --locked-mode --runtime win-x64
            --packages "${_dx12_prep_build}/nuget"
            --artifacts-path "${_dx12_prep_build}/artifacts"
        COMMAND "${DX12_RESOURCES_DOTNET}" publish "${_dx12_prep_project}"
            --configuration Release --runtime win-x64 --self-contained true
            --no-restore -p:PublishSingleFile=true -p:PublishReadyToRun=false
            --artifacts-path "${_dx12_prep_build}/artifacts"
            --output "${_dx12_prep_publish}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_dx12_prep_build}/publish.stamp"
        DEPENDS ${_dx12_prep_inputs}
        COMMENT "Build self-contained DX12ResourcesPrep (locked dependencies)"
        VERBATIM
    )
    add_custom_target(BUILD_DX12_RESOURCES_COMPANION
        DEPENDS "${_dx12_prep_build}/publish.stamp")
    add_custom_target(DEPLOY_DX12_RESOURCES_COMPANION
        DEPENDS BUILD_DX12_RESOURCES_COMPANION
        COMMENT "Deploy DX12ResourcesPrep alongside the Skyrim SKSE plugin")
    foreach(_dx12_deploy_root $ENV{CommunityShadersOutputDir})
        add_custom_command(TARGET DEPLOY_DX12_RESOURCES_COMPANION POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${_dx12_deploy_root}/SKSE/Plugins/CommunityShaders/DX12Resources/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_dx12_prep_publish}"
                "${_dx12_deploy_root}/SKSE/Plugins/CommunityShaders/DX12Resources/bin"
            COMMENT "Deploy DX12 Resources companion to ${_dx12_deploy_root}"
            VERBATIM)
    endforeach()
    if(TARGET DEPLOY_ALL)
        add_dependencies(DEPLOY_ALL DEPLOY_DX12_RESOURCES_COMPANION)
    endif()
endif()
