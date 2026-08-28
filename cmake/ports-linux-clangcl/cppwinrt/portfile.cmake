vcpkg_download_distfile(ARCHIVE
    URLS "https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/${VERSION}"
    FILENAME "cppwinrt.${VERSION}.zip"
    SHA512 ADF9EC7059A58B3E0EB0057DE52900692F58305AEE8BA708D265D273A81127978BEB9BF2599B00855B61B725D4E6EB06206B66897EAEAEF1AEC83948D60BC293
)

vcpkg_extract_source_archive(
    src
    ARCHIVE "${ARCHIVE}"
    NO_REMOVE_ONE_LEVEL
)

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
    set(CPPWINRT_ARCH win32)
else()
    set(CPPWINRT_ARCH ${VCPKG_TARGET_ARCHITECTURE})
endif()

set(CPPWINRT_TOOL "${src}/bin/cppwinrt.exe")

# The upstream vcpkg port requires a real Windows SDK (WindowsSDKDir /
# WindowsSDKVersion env vars + References/*.winmd) to generate OS-namespace
# projection headers (winrt/Windows.Foundation.h etc.) -- none of which an
# `xwin` sysroot provides (xwin stages headers/import libs only, not SDK
# metadata). This codebase only consumes winrt::com_ptr (a header-only COM
# smart pointer from the base library, no OS projections), so generate with
# cppwinrt.exe's `-input local` mode instead: it emits winrt/base.h and the
# winrt/impl/* support headers with no Windows Metadata input at all.
find_program(WINE_EXECUTABLE NAMES wine)
if(NOT WINE_EXECUTABLE)
    message(FATAL_ERROR "${PORT}: cross-compiling from a Linux/macOS host needs `wine` on PATH to run cppwinrt.exe (base-headers-only generation, no Windows SDK required).")
endif()

file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/include")
message(STATUS "Generating C++/WinRT base headers (-input local, no Windows SDK)")
vcpkg_execute_required_process(
    COMMAND "${WINE_EXECUTABLE}" "${CPPWINRT_TOOL}" -input local -output "${CURRENT_PACKAGES_DIR}/include" -verbose
    WORKING_DIRECTORY "${CURRENT_PACKAGES_DIR}"
    LOGNAME "cppwinrt-generate-${TARGET_TRIPLET}"
)

set(CPPWINRT_LIB "${src}/build/native/lib/${CPPWINRT_ARCH}/cppwinrt_fast_forwarder.lib")
file(INSTALL "${CPPWINRT_LIB}" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
if(NOT DEFINED VCPKG_BUILD_TYPE)
    file(INSTALL "${CPPWINRT_LIB}" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
endif()
file(INSTALL "${CPPWINRT_TOOL}" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/cppwinrt")

set(tool_path "tools/cppwinrt/cppwinrt.exe")
set(lib_name "cppwinrt_fast_forwarder.lib")

configure_file("${CMAKE_CURRENT_LIST_DIR}/cppwinrt-config.cmake.in"
  "${CURRENT_PACKAGES_DIR}/share/${PORT}/${PORT}-config.cmake"
  @ONLY)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${src}/LICENSE")
