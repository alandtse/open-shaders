# devbench-api — header-only cross-plugin API for SKSE consumers.
# Installs the MIT API header + its companion .cpp (compiled into the consumer via the
# config's INTERFACE_SOURCES).
#
# Pinned to a published commit. devbench-api isn't in the official vcpkg registry, so
# consumers add this directory to VCPKG_OVERLAY_PORTS (see README). To ship a newer API
# revision, bump REF to the new commit and SHA512 to its archive hash.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO alandtse/devbench
    REF 7a9f0c4a16b2fb9202e20de419ff0f0aeee18b10
    SHA512 95ad248cf45b5e1234844a407033ae12001e481f267efd37a0e3873c8e2710235f0518ececcc0dd3a78cf66c0a38a192b169c67c6e9211030ade3eedc6249b87
    HEAD_REF main
)

# MIT API glue → header to include/, source to share/ (referenced by the config target).
file(
    INSTALL "${SOURCE_PATH}/include/DevBenchAPI.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include"
)
file(
    INSTALL "${SOURCE_PATH}/include/DevBenchAPI.cpp"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/src"
)

# CMake package config — defines DevBench::API.
file(
    INSTALL "${CMAKE_CURRENT_LIST_DIR}/devbench-api-config.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
)

# The API glue is MIT (not the GPL-3.0 plugin) — ship that as the port copyright.
file(
    INSTALL "${SOURCE_PATH}/include/DevBenchAPI.LICENSE.txt"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
    RENAME
    copyright
)
