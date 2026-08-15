cmake_minimum_required(VERSION 3.21)
project(dlssg-repro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(dlssg-repro main.cpp)
target_include_directories(dlssg-repro PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../extern/Streamline-DX12/include)
target_link_libraries(dlssg-repro PRIVATE d3d11 d3d12 dxgi d3dcompiler user32)
target_compile_definitions(dlssg-repro PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# Stage the Streamline runtime next to the exe (production v2.12.0 set vendored in the repo).
set(SL_DLL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../features/Upscaling/Shaders/Upscaling/StreamlineDX12)
foreach(dll sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.pcl.dll sl.reflex.dll nvngx_dlssg.dll)
    add_custom_command(TARGET dlssg-repro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SL_DLL_DIR}/${dll} $<TARGET_FILE_DIR:dlssg-repro>)
endforeach()

add_executable(drs-tool drs-tool.cpp)
target_include_directories(drs-tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_definitions(drs-tool PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
