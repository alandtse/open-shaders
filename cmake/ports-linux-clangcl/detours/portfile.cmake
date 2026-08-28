vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO microsoft/Detours
    REF v${VERSION}
    SHA512 0a9c21b8222329add2de190d2e94d99195dfa55de5a914b75d380ffe0fb787b12e016d0723ca821001af0168fd1643ffd2455298bf3de5fdc155b3393a3ccc87
    HEAD_REF master
)

# Upstream ships only an NMake makefile (src/Makefile), which needs a real
# nmake.exe -- unavailable in an xwin sysroot (xwin stages SDK headers and
# import libs only, not the MSVC compiler/build-tools package). The library
# itself is plain portable C/C++, so build it via a small CMakeLists.txt
# instead (vendored alongside this portfile), compiling the exact same
# source-file set the upstream makefile does.
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
