vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wolfpld/tracy
    REF ef099b0555abce9534e92429503b2606493116d9
    SHA512 8b25c2a60b94ccfd5c462b15077947bb83f20fc63f1d10713234b4fa222c9c0f821752e1153e3a89f5d4dbcd3293c84f4fa1560ce7efe09d9ebb0f870b0ad9ff
    HEAD_REF master
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        on-demand TRACY_ON_DEMAND
        fibers	  TRACY_FIBERS
        verbose   TRACY_VERBOSE
    INVERTED_FEATURES
        crash-handler TRACY_NO_CRASH_HANDLER
)

set(TOOLS_OPTIONS "")
if("cli-tools" IN_LIST FEATURES OR "gui-tools" IN_LIST FEATURES)
    vcpkg_find_acquire_program(PKGCONFIG)
    list(APPEND TOOLS_OPTIONS "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        -DDOWNLOAD_CAPSTONE=OFF
        -DLEGACY=ON
        -DTRACY_ENABLE=ON
        ${FEATURE_OPTIONS}
    OPTIONS_RELEASE
        ${TOOLS_OPTIONS}
    MAYBE_UNUSED_VARIABLES
        DOWNLOAD_CAPSTONE
        LEGACY
)
vcpkg_cmake_install()

# Tracy's install() FILES list omits these common headers at this ref even
# though the client headers (e.g. TracyProfiler.hpp) include them; copy them
# so downstream code compiles.
file(INSTALL
    "${SOURCE_PATH}/public/common/TracyFormat.hpp"
    "${SOURCE_PATH}/public/common/TracyVersion.hpp"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/tracy/common")

vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME Tracy CONFIG_PATH lib/cmake/Tracy)

function(tracy_copy_tool tool_name tool_dir)
    vcpkg_copy_tools(
        TOOL_NAMES "${tool_name}"
        SEARCH_DIR "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/${tool_dir}"
    )
endfunction()

if("cli-tools" IN_LIST FEATURES)
    tracy_copy_tool(tracy-capture capture)
    tracy_copy_tool(tracy-csvexport csvexport)
    tracy_copy_tool(tracy-import-chrome import)
    tracy_copy_tool(tracy-import-fuchsia import)
    tracy_copy_tool(tracy-update update)
endif()
if("gui-tools" IN_LIST FEATURES)
    tracy_copy_tool(tracy-profiler profiler)
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
