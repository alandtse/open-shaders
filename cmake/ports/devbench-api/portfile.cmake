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
    REF 5bc4ad7c2a11063747ed4388405c30c3a3595c9b
    SHA512 0ca6ced4427904fbd3da6e22f7509564e0b3d35bbef1952e60e551196d20f78212e7f970c5e85bd3d84f936f0df3db600d05bfabb30508435d26287b60fdea08
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
