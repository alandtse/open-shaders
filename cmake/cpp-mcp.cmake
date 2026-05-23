# Build cpp-mcp (https://github.com/hkr04/cpp-mcp) from its vendored
# submodule as a static library target. Upstream has no install rules
# (PR #12 still open), so we drive its build ourselves — same pattern
# we use for FidelityFX-SDK and Streamline.
#
# Only the server-side translation units are compiled; the bundled
# stdio/SSE *client* implementations are intentionally omitted because
# we are exclusively a server.
#
# nlohmann_json ABI alignment:
#   cpp-mcp vendors nlohmann_json 3.11.3 in extern/cpp-mcp/common/json.hpp,
#   while vcpkg ships 3.12.0. Both versions wrap their public API in an
#   ABI-versioned inline namespace (`nlohmann::json_abi_v3_11_3` vs
#   `nlohmann::json_abi_v3_12_0`), so even though both files share the
#   same include guard (INCLUDE_NLOHMANN_JSON_HPP_), the symbol names
#   differ. If cpp-mcp's own TUs picked up the vendored copy and our
#   consumers picked up vcpkg's, `mcp::server::set_capabilities` and
#   `register_tool` would link-fail (LNK2001) with two different
#   ABI-tagged signatures.
#
#   Fix: patch mcp_message.h at configure time to use
#   `#include <nlohmann/json.hpp>` instead of `#include "json.hpp"`.
#   The patched copy is written to a build-tree mirror; the submodule
#   stays clean. Both cpp-mcp's own compilation and every consumer
#   then resolve to vcpkg's 3.12.0 → single ABI namespace, symbols
#   match, linker happy.

set(CPP_MCP_DIR "${CMAKE_SOURCE_DIR}/extern/cpp-mcp")
set(CPP_MCP_PATCHED_INC "${CMAKE_BINARY_DIR}/cpp-mcp-patched/include")

if(NOT EXISTS "${CPP_MCP_DIR}/src/mcp_server.cpp")
    message(FATAL_ERROR
        "cpp-mcp submodule missing. Run:\n"
        "  git submodule update --init --recursive extern/cpp-mcp")
endif()

find_package(Threads REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Patch mcp_message.h to use vcpkg nlohmann_json (see header comment).
# All other cpp-mcp headers are copied verbatim into the patched mirror
# so they live next to the patched header and find each other.
file(MAKE_DIRECTORY "${CPP_MCP_PATCHED_INC}")
file(GLOB _cpp_mcp_headers CONFIGURE_DEPENDS "${CPP_MCP_DIR}/include/*.h")
foreach(_hdr IN LISTS _cpp_mcp_headers)
    get_filename_component(_name "${_hdr}" NAME)
    file(READ "${_hdr}" _content)
    if(_name STREQUAL "mcp_message.h")
        # Fail fast if the expected include vanishes upstream — otherwise the
        # ABI mismatch would silently come back and only surface as an LNK2001
        # well into the link step.
        string(FIND "${_content}" "#include \"json.hpp\"" _json_inc_pos)
        if(_json_inc_pos EQUAL -1)
            message(FATAL_ERROR
                "cpp-mcp: expected `#include \"json.hpp\"` in mcp_message.h "
                "but did not find it. Upstream may have changed the include; "
                "review cmake/cpp-mcp.cmake and adjust the patch (see header "
                "comment for the ABI-alignment rationale).")
        endif()
        string(REPLACE
            "#include \"json.hpp\""
            "#include <nlohmann/json.hpp>"
            _content "${_content}")
    endif()
    file(WRITE "${CPP_MCP_PATCHED_INC}/${_name}" "${_content}")
endforeach()

add_library(cpp-mcp STATIC
    "${CPP_MCP_DIR}/src/mcp_message.cpp"
    "${CPP_MCP_DIR}/src/mcp_resource.cpp"
    "${CPP_MCP_DIR}/src/mcp_server.cpp"
    "${CPP_MCP_DIR}/src/mcp_tool.cpp"
)

# Order matters: patched mirror first so its mcp_message.h wins over the
# submodule's. `common/` is still needed for httplib.h (no ABI issue
# there — it's not shared with any vcpkg dep).
target_include_directories(cpp-mcp
    PUBLIC  "${CPP_MCP_PATCHED_INC}"
            "${CPP_MCP_DIR}/common"
)

target_compile_features(cpp-mcp PUBLIC cxx_std_17)

target_compile_definitions(cpp-mcp PUBLIC
    MCP_MAX_SESSIONS=10
    MCP_SESSION_TIMEOUT=30
    # cpp-mcp's vendored cpp-httplib pulls in <winsock2.h>. Skyrim/CLib's
    # transitive <Windows.h> defaults to the legacy <winsock.h>, which
    # conflicts (redefinition of sockaddr, WSAData, etc.). Tell Windows
    # headers to skip the legacy winsock so winsock2.h is the only one
    # in the build. PUBLIC so it propagates to every TU that links
    # cpp-mcp (including the PCH compilation of CommunityShaders).
    _WINSOCKAPI_
)

target_link_libraries(cpp-mcp PUBLIC
    Threads::Threads
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(cpp-mcp PRIVATE /utf-8 /bigobj /W0)
    target_compile_definitions(cpp-mcp PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(cpp-mcp PROPERTIES FOLDER "extern")
