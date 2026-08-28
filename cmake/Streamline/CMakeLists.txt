cmake_minimum_required(VERSION 3.21)

add_library(Streamline INTERFACE)

target_include_directories(
  Streamline INTERFACE ${CMAKE_SOURCE_DIR}/extern/Streamline-DX12/include)

if(MSVC)
  target_compile_options(Streamline INTERFACE /wd5103)
endif()

if(CMAKE_HOST_UNIX)
  # extern/Streamline-DX12 is NVIDIA's own upstream repo (not one of our
  # forks), so this can't be fixed by patching the submodule directly and
  # upstreaming it the way other cross-compile fixes in this codebase are.
  # sl_pcl.h's `#if __cplusplus == 202302L` check for std::to_underlying
  # is unreliable under this toolchain: __cplusplus can read 202302L while
  # std::to_underlying is still absent from the translation unit (observed
  # with clang-cl cross-compiling against the MSVC STL). Patched at
  # configure time to always use the portable fallback implementation
  # instead -- identical behavior to std::to_underlying, per the file's
  # own comment. Idempotent: string(REPLACE) on already-patched content
  # (the search string no longer present) is a no-op, so this is safe to
  # run on every reconfigure without re-patching or erroring.
  # sl_pcl.h ships with CRLF line endings. file(READ)/file(WRITE) don't
  # preserve them (they round-trip through CMake's internal LF-only string
  # representation), so a naive read-replace-write here would rewrite every
  # line ending in the file and drown the real ~13-line change in a
  # whole-file diff. Normalize to LF for the match, then restore CRLF
  # before writing so only the intended lines actually change.
  set(_sl_pcl_h "${CMAKE_SOURCE_DIR}/extern/Streamline-DX12/include/sl_pcl.h")
  file(READ "${_sl_pcl_h}" _sl_pcl_h_content)
  string(REPLACE "\r\n" "\n" _sl_pcl_h_content "${_sl_pcl_h_content}")
  string(
    REPLACE
    "// c++23 has to_underlying implementation\n#if __cplusplus == 202302L\nusing to_underlying = std::to_underlying;\n#else\n// Return \`enum class\` member as value of underlying type (i.e. an int).  Basically same as:\n// static_cast<std::underlying_type_t<decltype(value)>>(value);\n// See c++23s std::to_underlying()\ntemplate<class T>\nconstexpr auto to_underlying(T value)\n{\n    return std::underlying_type_t<T>(value);\n}\n#endif"
    "// Always use this portable implementation rather than std::to_underlying\n// on this cross-compile toolchain (patched at configure time by\n// cmake/Streamline/CMakeLists.txt -- see there for why). Identical\n// behavior to std::to_underlying: static_cast<std::underlying_type_t<decltype(value)>>(value)\ntemplate<class T>\nconstexpr auto to_underlying(T value)\n{\n    return std::underlying_type_t<T>(value);\n}"
    _sl_pcl_h_content
    "${_sl_pcl_h_content}"
  )
  string(REPLACE "\n" "\r\n" _sl_pcl_h_content "${_sl_pcl_h_content}")
  file(WRITE "${_sl_pcl_h}" "${_sl_pcl_h_content}")
endif()
