vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wolfpld/tracy
    REF 1f1738c221e71b05c7646adf39a6c595bebd6fe5
    SHA512 9c961e0bf1f765f18868878ae4a22fc50b555065555c10c5722f283c1b705748465bb958387cf78927743ab9ec9752253c1bd58fbab6abea69ec25545e4883f0
    HEAD_REF master
    PATCHES
        build-tools.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        on-demand TRACY_ON_DEMAND
        fibers	  TRACY_FIBERS
        verbose   TRACY_VERBOSE
    INVERTED_FEATURES
        crash-handler TRACY_NO_CRASH_HANDLER
)

vcpkg_check_features(OUT_FEATURE_OPTIONS TOOLS_OPTIONS
    FEATURES
        cli-tools VCPKG_CLI_TOOLS
        gui-tools VCPKG_GUI_TOOLS
)

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
