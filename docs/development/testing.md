# Automated Tests

Pick the layer by what the test needs to prove. In-game verification through devbench is separate and is covered in [AGENTS.md](../../AGENTS.md#testing).

| Layer        | Location        | Use for                                                                                                 | Runs in CI                                                 |
| ------------ | --------------- | ------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------- |
| Catch2 (C++) | `tests/cpp`     | Plugin logic: put it in a header, or a `.cpp` the test target links, and test the real code             | Windows C++ job: `CppUtilTests`, `ScreenshotDevBenchTests` |
| HLSL tests   | `tests/shaders` | Shader functions, run on the GPU with ShaderTestFramework                                               | Shader tests job                                           |
| Python       | `tests/*.py`    | Python and CMake tooling (`tools/`, `cmake/*.py`); each test must be listed in a CI step in the same PR | See the CI steps in `.github/workflows/_shared-build.yaml` |

## Rules

-   **Test the real code.** Include the production header or link the production `.cpp`. Do not copy a formula, lock, or algorithm into the test: the test then checks its own copy.
-   **Do not assert on source text.** No test reads `src/` and matches substrings or regexes to check how a function is written: a rename, clang-format, or an extracted helper breaks it without a behavior change, and it passes when the code is present but dead. The exception is an architecture lint for a repo invariant (for example, that scene-settings code names no feature), written as a Python test over the tree.
-   **Do not paste slices of `src/*.cpp` into a stub translation unit.** The `compile_and_run` harness in `test_scene_settings_runtime.py` does this and is legacy: its stubs drift from the sources and then fail to compile. Do not add users. When code needs a test, move the logic into a header or an injectable seam and use Catch2.
-   **Share data across languages.** HLSL parity tests include one table from both sides (see `package/Shaders/Tests/WindFieldParitySamples.hlsli`). List every file that feeds such a test in the `cpp_tests` path filter of `.github/actions/check-build-changes/action.yaml`, or a PR can pass one side only.
-   **A test that CI does not run will rot.** Name the workflow step that runs a new test in the PR description.

## Running locally

-   C++: `cmake --build build/ALL --target cpp_tests`, then `ctest --test-dir build/ALL -C Release -R CppUtilTests`.
-   C++ devbench screenshot tests: `cmake --build build/ALL --target screenshot_tests_devbench`, then `ctest --test-dir build/ALL -C Release -R ScreenshotDevBenchTests`.
-   Shaders: `cmake --build build/ALL --target run_shader_tests`.
-   Python: `cd tests && python -m unittest test_<name>`. The compile-based tests need `g++` or `clang++`; a few need the Windows imgui build and skip elsewhere.

## Adding a C++ test

1. Add `tests/cpp/test_<thing>.cpp` and list it in `tests/cpp/CMakeLists.txt`. List any production `.cpp` it needs next to the others.
2. Use `TEST_CASE` with `CHECK`/`REQUIRE` and `CHECK_THROWS`; do not roll your own check helper.
3. Code behind `DEVBENCH_BRIDGE_ENABLED` belongs in a file built into `screenshot_tests_devbench`.
