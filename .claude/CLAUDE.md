# CLAUDE.md

This file provides guidance to coding agents when working with code in this repository.

## Quick Checklist (Most Violated Rules)

-   **PR Title & Commits:** Use Conventional Commits (`type(scope): description`), title <= 50 chars, body wrapped to 72 chars/line. Target branch must be `dev` (never PR directly to `main`).
-   **PR Title Correction:** If a PR title becomes stale before merging, fix it with: `gh pr edit <num> --title "..."` (PR title becomes the release commit message).
-   **Comments:** Max 1-2 lines inline. Explain _why_, not _what_. Describe present code only (never absent/removed code, except for regression-risk warnings). No mid-function tutorials.
-   **Comment Invariants:** Do not add "see commit/PR" pointers or name one-off incidents/tools (e.g. "the RenderDoc CTD"). State the invariant and stop.
-   **Minimal Churn:** Do not reformat unrelated code or rename adjacent variables outside the PR scope.
-   **DRY Review:** Check new code against existing shared utilities codebase-wide (e.g. `SetResourceName`, `GetGameSettingValue`, `isVR` cache, serialize/format/filesystem helpers).
-   **DirectX Naming:** Name every D3D11 resource using `Util::SetResourceName`. Canonical implementation is in `Utils/D3D.cpp`; never duplicate the GUID or re-implement inline.
-   **VR Maintenance:** Keep VR divergence to the absolute minimum necessary. Resolve merge conflicts in favor of keeping VR.
-   **Git Safety:** Never force-push/rebase shared branches (`main`, `dev`, `hotfix/*`). Never manually create `v*` tags, hand-modify `CMakeLists.txt` version, or run release workflow on `hotfix/X.Y.x` for the current line.
-   **Upstream Sync:** Merge, never cherry-pick. Land sync PRs as merge commits, never squash. Verify ancestry after merging.

---

## Fork Identity & Logo Policy

-   **Keep as `CommunityShaders` (Do NOT rename C++ runtime identity):** Keep CMake `PROJECT_NAME`, DLL name, `SKSE/Plugins/CommunityShaders/` directory, `CommunityShaders.log`, ImGui window ID after `###`, asset paths under `package/Interface/CommunityShaders/`, and HLSL include paths.
-   **Use "Open Shaders" for public identity:** Use in in-game menu titles, READMEs/instructions, Nexus filenames, GitHub release names, and Welcome/FAQ/About text.
-   **Link Upstream Explicitly:** Link upstream references to `community-shaders/skyrim-community-shaders` (Nexus 86492). Do not link dead `doodlum` paths. Open Shaders' own Nexus is 180419.
-   **AIO Bundling Semantics:**
    -   The Nexus upload workflow ships **ONLY** the AIO archive; there is no per-feature matrix distribution.
    -   The per-feature matrix upload is gated on `autoupload = true` (in the feature's `.ini` `[Info]` section).
    -   Use `aio = true` (with `autoupload = false`) to bundle a third-party feature redistributed with permission without uploading it standalone.
    -   A runtime-core feature (`IsCore()` returns `true`) must **also** carry a `CORE` marker file, or its shaders are excluded from the AIO bundle and it ships broken (e.g. shared includes).
    -   Partition logic is handled by `feature_in_aio` in `CMakeLists.txt`. For local development, configure `AIO_INCLUDE_NON_AUTOUPLOAD=ON` to include everything.
-   **No Logo:** `cs-logo.png` is intentionally absent (non-GPL). Do not "fix" this or restore upstream assets. Logo draws are null-safe fallback (retries colored fallback in `IconLoader.cpp` and gates draw on null check in `Menu.cpp`, `MenuHeaderRenderer`, and `HomePageRenderer`).
    -   _Exception to comment rules:_ A regression-risk warning naming removed code so a future maintainer doesn't restore it (e.g. "do not restore cs-logo.png") is load-bearing and stays.

---

## Comment & Documentation Standards

-   **Doxygen:** Use Doxygen-style comments for all public declarations and API methods (especially graphics-related functions).
-   **Concise Inline Comments:** Keep body/inline comments to **1–2 lines max**. Go longer only for extraordinary reasons (load-bearing invariants, gotchas, or "don't revert this" warnings). Long-form explanations belong in the commit message or PR description, not in the code.
-   **Focus on Rationale:** Comments must explain _why_ something is done, not _what_ the code does. Do not add an explicit "see commit/PR" pointer (the VCS history already links each line to its commit) or name transient tools/issues ("the RenderDoc CTD").
-   **Present Code Only:** Comments must describe the present file's code. Do not comment on absent or deleted code (e.g., "this constant was renamed from X"), as the deletion is not visible to the reader.

---

## Code Quality & Architecture Standards

-   **No Placeholders:** Never ship TODO, FIXME, or incomplete implementations unless explicitly requested for planning. Provide complete, working solutions with full error handling.
-   **Minimal Churn:** PRs must touch only the lines required for the change. Do not reformat unrelated code, clean up surrounding structures, or rename adjacent variables.
-   **Descriptive Naming:** Use domain-specific names that clearly indicate graphics/rendering purpose (e.g. `screenSpaceAmbientOcclusion` not `ssao`, `UpdateShadowCascades()` not `UpdateSC()`).
-   **Single Responsibility:** Each feature class must handle exactly one graphics technique. Break up C++ functions longer than ~200 lines into focused helper methods.
-   **Centralize Constants:** Extract magic numbers and UI theme settings to named constants in appropriate classes (e.g., `ThemeManager::Constants`).
-   **DRY Codebase-Wide:** Do not reinvent existing functionality. Check your changes against the codebase and use shared utility libraries in `src/Utils/` (e.g., `Serialize.h` for JSON, `Format.h` for strings, `FileSystem.h` for paths, `UI.h` for ImGui). Always reuse:
    -   `Util::SetResourceName` for resource naming.
    -   `Util::GetGameSettingValue` for reading game settings.
    -   The cached `globals::game::isVR` for runtime VR checks.
-   **ImGui Integration:**
    -   Always pair `ImGui::BeginTable()` with `ImGui::EndTable()`. Orphaned `TableNextColumn()` calls cause layout bugs and crashes.
    -   Use the RAII pattern for ImGui style changes; avoid manual save/restore states.
    -   Use central Theme constants for UI spacing and padding instead of hardcoded values.
    -   Use callbacks to access private methods from modular UI components rather than making methods public. Keep UI state managed centrally in the `Menu` class.
-   **Restart-Gated Config Fields:** Use `Util::Settings::BootSnapshot` + `kRestartFields` metadata to diff boot-latched vs selected settings values. This drives `Util::Text::RestartNeeded` banners and MCP/menu introspection (Upscaling acts as the canary).

---

## DirectX & D3D11 Resource Naming

-   **Debuggability:** Every D3D11 resource must be named for RenderDoc debuggability using `Util::SetResourceName(ptr, "Feature::ResourceDescription")` after raw `device->Create*` calls.
-   **Wrappers:** For wrapper types (`Texture2D`, `Buffer`, `ConstantBuffer` in `Buffer.h`), pass the name to the constructor. Views are named automatically.
-   **Conventions:** Use `"Feature::Name"` for the resource and `"Feature::Name SRV"` / `"Feature::Name UAV"` for views.
-   **Implementation:** The canonical implementation lives in `Util::SetResourceName` (`Utils/D3D.cpp`). Never duplicate the GUID or re-implement this call inline.

---

## VR & Cross-Platform Policy

-   **Non-VR is Primary:** Treat non-VR (flatrim: SE/AE) as the primary code path. VR is a minimal divergence kept to the absolute minimum necessary.
-   **Divergence Rules:**
    -   Diverge only where behavior is genuinely VR-specific (stereo projection, VR-only resources, different engine data paths).
    -   _C++:_ Use runtime checks. Universal binaries use runtime detection. Prefer the cached `globals::game::isVR` over calling `REL::Module::IsVR()` directly (reserve direct calls for early-init paths).
    -   _HLSL:_ Diverge via `#if defined(VR)` compiler checks per shader permutation.
    -   Keep VR branches small and localized. Avoid VR branching to work around side-effects; unify paths where possible.
-   _For detailed multi-runtime member offset and virtual relocations, see [Repository Architecture](../docs/development/architecture.md)._

---

## Performance & Profiling Rules

-   **Pass Instrumentation:** Wrap every new render pass entry point with the `CS_GPU_PASS("Feature::Pass")` macro (RAII `ScopedGpuPass`). Do not use direct `TracyD3D11Zone` or `State::BeginPerfEvent` at pass entry sites.
-   **Sub-Dispatch Annotations:** Raw/legacy zones (such as `TracyD3D11Zone`) are only appropriate for sub-dispatches within a pass where profiler timer granularity is not required.
-   **Justifying Speedups:** PRs claiming performance speedups (or `perf:` commits) must state a measured number in their description:
    -   Justify at the PR level, not per commit.
    -   Normalize cost/savings as a percentage of the target frame budget (VR 90fps ≈ 11.1ms; flatrim 60fps ≈ 16.7ms). Do not quote raw wall-clock deltas.
    -   Measure GPU per-pass cost using Tracy (compile preset with `-DTRACY_SUPPORT=ON`, connect server to port 8086).
    -   A/B test a feature: toggle it (via in-game menu or devbench `openshaders.feature`), capture the same scene/camera both ways, and diff the zone times holding scene, resolution, and upscaler fixed.
    -   Micro-optimizations that cannot be isolated/benchmarked must be labeled `refactor:`, not `perf:`.

---

## Error Handling & Memory Management

-   **Memory Safety:** Follow RAII principles with C++23, using smart pointers for automatic resource management and the `bshoshany-thread-pool` for parallel operations.
-   **Graceful Degradation:** Features must disable cleanly on shader compilation failures or DirectX errors. Provide robust fallback rendering paths.
-   **Error Context:** Include relevant graphics state (current shader, buffer sizes, etc.) in error logs.
-   **User-Friendly Reporting:** Report errors through the ImGui interface with actionable guidance.

---

## Git & Release Invariants

-   **PR Branch Targets:** All PRs must target the `dev` branch. Never PR directly to `main`.
-   **Approval:** Never push to shared branches without explicit OK.
-   **Commit Message Formatting:** Use `type(scope): description`. Title <= 50 characters, body lines wrapped at 72 characters.
-   **Commit Versioning Traps:**
    -   Mislabeling a build/CI/test change as `fix:` burns a patch release on a non-user-visible change. Use `build:`, `ci:`, or `test:`.
    -   Mislabeling a refactor as `feat:` forces a minor bump.
    -   A performance improvement on internal code is `refactor:`, not `perf:`.
    -   `chore:` is a catch-all; prefer the specific type when one fits.
-   **Branch Lineage:** `main` must remain an ancestor of `dev`. Following a hotfix staging promotion, `dev` is auto-rebase-reconciled (force-pushed by the workflow) to absorb the `chore(release):` commit.
-   **Prohibitions (Never do these without explicit user direction):**
    -   Do not force-push or rebase `main`, `dev`, or any `hotfix/*` branch (except release workflow auto-reconciliations).
    -   Do not manually create tags matching `v*` (semantic-release owns these).
    -   Do not manually bump `CMakeLists.txt`'s `VERSION` field.
    -   Do not PR a feature branch directly into `main`.
    -   Do not run `Release: Semantic Version` on `hotfix/X.Y.x` for the current line (fails with "cannot be published as it is out of range"). Use `ff_target` into `main` instead.
-   **Upstream Sync Rules:**
    -   Pull changes from upstream `community-shaders/skyrim-community-shaders` by merge (`git merge <upstream-ref>`), never cherry-pick.
    -   Land sync PRs as merge commits, never squash.
    -   Resolve conflicts in favor of keeping VR. Revert upstream VR removals.
    -   Verify ancestry after landing: `git merge-base --is-ancestor <upstream-sha> HEAD` must pass.
-   _For conventional commit mappings, staging/RC workflows, release stages (Alpha/Beta ini flags), and manual packaging targets, see [Release Process](../docs/development/release-process.md) and [Upstream Sync Guide](../docs/development/upstream-sync.md)._

---

## Security & Input Validation

-   **Configuration:** Validate all `.ini` configuration files and user settings to prevent Skyrim startup crashes.
-   **Shaders:** Validate shader parameters and buffer sizes to prevent GPU driver crashes.
-   **Paths:** Sanitize and validate all file paths for texture and asset loading to prevent directory traversal.
-   **Bounds Checking:** Enforce bounds checking for buffer operations, especially during DirectX resource management.
-   **Limits:** Enforce reasonable limits on user-configurable values (texture sizes, buffer counts, etc.).

---

## Environment & Build Reference Summary

-   **WSL/Linux Note:** For Windows SDK compilation, run via PowerShell:
    `powershell.exe -Command "./BuildRelease.bat [PRESET_NAME]"`
-   **Primary Build Command:** `./BuildRelease.bat [PRESET_NAME]`
    -   _Presets:_ `ALL` (default), `SE`, `AE`, `VR`, `PRE-AE`, `FLATRIM`.
-   **clangd setup:** Generate compilation database after configuring `ALL`:
    `pwsh tools/gen-clangd-db.ps1`
-   **Shader Refactor Verification:**
    -   _Identical DXBC:_ Provable no-op (`tools/verify-shader-refactor.ps1`).
    -   _Legitimate op-reordering:_ Use runtime A/B frame diffing (`tools/taa-renderdoc-ab.py`).
-   **Feature Workflow:** Start from `template/`, implement the C++ class inheriting from `Feature` (`DrawSettings()`, `LoadSettings()`, `SaveSettings()`, and feature-specific rendering hooks), and register in appropriate source files and `globals::features`.
-   _For detailed setup, options, and commands, see [VSCode Setup](../docs/development/vscode-setup.md), [Development README](../docs/development/README.md), [Shader Development Workflow](../docs/development/shader-workflow.md), [In-game A/B Testing](../docs/development/shader-runtime-ab.md), and [Repository Architecture](../docs/development/architecture.md)._
