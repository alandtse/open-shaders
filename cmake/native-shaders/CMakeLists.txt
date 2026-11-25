cmake_minimum_required(VERSION 3.25)
project(NativeShaderValidation LANGUAGES NONE)

# Reuse production shader build commands without configuring the Skyrim plugin.
get_filename_component(
    NATIVE_SHADER_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE
)
set(NATIVE_SHADER_VALIDATION_FLAGS -WX)
add_custom_target(${PROJECT_NAME})
include("../DX12NativeShadow.cmake")
include("../DX12NativeComposite.cmake")
if(NOT TARGET DX12NativeShadowDXIL OR NOT TARGET DX12NativeCompositeDXIL)
    message(FATAL_ERROR "DXC is required for native shader validation")
endif()
add_custom_target(
    native_shader_validation ALL
    DEPENDS DX12NativeShadowDXIL DX12NativeCompositeDXIL
)
