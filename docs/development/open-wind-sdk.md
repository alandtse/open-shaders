# Open Wind SDK dependency

Open Shaders obtains the public [Open Wind](https://github.com/kevdevrev/open-wind)
headers through CMake FetchContent.
`cmake/OpenWind.cmake` pins the source commit. Update its `GIT_TAG` to select
a different SDK revision; do not copy headers into this repository.

Only Open Wind's `api/` CMake directory is configured. Its `OpenWind::SDK`
interface target supplies public includes and a C++20 requirement. Open Shaders
already supplies DirectXTK for the renderer types. Open Wind's root project,
implementation, runtime dependencies and optional bridge DLLs are not built.
Runtime discovery still uses the exported interfaces in `OpenWind.dll`.

The SDK is GPL-licensed; its source repository contains `COPYING` and `NOTICE.md`.

## Local development

Set `FETCHCONTENT_SOURCE_DIR_OPENWIND` to an absolute Open Wind checkout path,
either with `-D` during configuration or in a gitignored `CMakeUserPresets.json`.
Point it at the repository root containing `api/` and `include/`. CMake then
uses those headers without downloading from Git. Keep each worktree's build
directory beneath its own source directory.
