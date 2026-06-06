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
    REF 0b675699aa1f8b0fbd4b4fd6c4f9c44c873f6947
    SHA512 a1153d8c4ef307b62e6bd5bae74789ad978681b633d0c1b83e3707f101952591985f16cd712f0b5f6ff82b30884686ec868f0a74696c6863590d62be9446ef7e
    HEAD_REF main
)

# MIT API glue → header to include/, source to share/ (referenced by the config target).
file(INSTALL "${SOURCE_PATH}/include/DevBenchAPI.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${SOURCE_PATH}/include/DevBenchAPI.cpp"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/src")

# CMake package config — defines DevBench::API.
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/devbench-api-config.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

# The API glue is MIT (not the GPL-3.0 plugin) — ship that as the port copyright.
file(INSTALL "${SOURCE_PATH}/include/DevBenchAPI.LICENSE.txt"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
