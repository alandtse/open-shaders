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
# cppwinrt.exe's `-base` flag instead: it emits a self-contained winrt/base.h
# (including com_ptr) with no Windows Metadata input at all. (`-input local`
# looks like the more obvious choice, but it reads from %WinDir%\System32\
# WinMetadata, which is empty in a fresh Wine prefix and fails with "Invalid
# row index" -- verified against cppwinrt.exe 2.0.250303.1 itself, since the
# official port's own -help text doesn't spell out that distinction.)
find_program(WINE_EXECUTABLE NAMES wine)
if(NOT WINE_EXECUTABLE)
    message(FATAL_ERROR "${PORT}: cross-compiling from a Linux/macOS host needs `wine` on PATH to run cppwinrt.exe (base-headers-only generation, no Windows SDK required).")
endif()

file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/include")

# cppwinrt.exe's own argument parser treats a leading "/" as a Windows-style
# option switch, so a raw Unix path (e.g. "/Users/.../include") is rejected
# as an unrecognized option rather than read as a positional path -- Wine
# only translates paths inside Win32 file APIs, never in argv strings.
# `winepath -w` converts it to the "Z:\..." form the tool actually accepts.
find_program(WINEPATH_EXECUTABLE NAMES winepath)
if(NOT WINEPATH_EXECUTABLE)
    message(FATAL_ERROR "${PORT}: `winepath` (ships alongside `wine`) not found on PATH.")
endif()
vcpkg_execute_required_process(
    COMMAND "${WINEPATH_EXECUTABLE}" -w "${CURRENT_PACKAGES_DIR}/include"
    OUTPUT_VARIABLE CPPWINRT_OUTPUT_WINPATH_RAW
    WORKING_DIRECTORY "${CURRENT_PACKAGES_DIR}"
    LOGNAME "cppwinrt-winepath-${TARGET_TRIPLET}"
)
string(STRIP "${CPPWINRT_OUTPUT_WINPATH_RAW}" CPPWINRT_OUTPUT_WINPATH)

message(STATUS "Generating C++/WinRT base headers (-base, no Windows SDK)")
vcpkg_execute_required_process(
    COMMAND "${WINE_EXECUTABLE}" "${CPPWINRT_TOOL}" -base -output "${CPPWINRT_OUTPUT_WINPATH}" -verbose
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
