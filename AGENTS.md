# AGENTS.md

This file provides guidance to coding agents when working with code in this repository.
Claude Code loads it via the `@../AGENTS.md` import in `.claude/CLAUDE.md`; Copilot and other tools
are pointed here too. This is the single source of agent guidance: keep detail in one place.

Act as an experienced graphics-programming and Skyrim-modding engineer: performance-aware, compatible across SE/AE/VR, and delivering complete working solutions.

## Quick Checklist (Most Violated Rules)

Each rule is stated once, in the section named in brackets.

-   **PR title & commits:** Conventional Commits `type(scope): description`, title <= 50 chars, body wrapped to 72, target `dev` (never `main`). Fix a stale title with `gh pr edit <num> --title "..."` (the title becomes the release commit message). [Git & Release]
-   **Comments:** 0 lines by default, and at most 2 when the WHY is non-obvious. [Comments]
-   **Minimal churn:** touch only the lines the change requires. [Code Quality]
-   **DRY:** reuse shared utilities and cached `globals::game::*` pointers. [Code Quality]
-   **Feature boundaries:** no feature may depend on a non-core feature. [Code Quality]
-   **D3D naming:** name every D3D11 resource with `Util::SetResourceName`. [DirectX Naming]
-   **Perf instrumentation:** `CS_GPU_PASS` at every render-pass entry point, including ported code. [Performance]
-   **VR maintenance:** keep VR divergence minimal; resolve merge conflicts in favor of keeping VR. [VR]
-   **Verify in game:** runtime-affecting changes are verified via devbench on VR plus one flat variant. [Testing]
-   **Git safety & upstream sync:** never force-push shared branches, never hand-create tags or bump the version, merge upstream and never cherry-pick. [Git & Release]

---

## Fork Identity & Logo Policy

-   **Keep as `CommunityShaders` (do not rename the C++ runtime identity, so users can switch to or from upstream without losing settings):** CMake `PROJECT_NAME`, DLL name, `SKSE/Plugins/CommunityShaders/`, `CommunityShaders.log`, the ImGui window ID after `###`, asset paths under `package/Interface/CommunityShaders/`, and HLSL include paths.
-   **Use "Open Shaders" for public identity:** in-game menu titles, READMEs/instructions, Nexus filenames, GitHub release names, Welcome/FAQ/About text.
-   **Link upstream explicitly** to `community-shaders/skyrim-community-shaders` (Nexus 86492); never to dead `doodlum` paths. Open Shaders' own Nexus is 180419.
-   **AIO bundling:**
    -   The Nexus upload ships **only** the AIO archive; the per-feature matrix upload is gated on `autoupload = true` in the feature's `.ini` `[Info]` section.
    -   Use `aio = true` (with `autoupload = false`) to bundle a third-party feature redistributed with permission.
    -   A runtime-core feature (`IsCore()` is `true`) must also carry a `CORE` marker file, or its shaders are left out of the AIO bundle and it ships broken.
    -   Partition logic is `feature_in_aio` in `CMakeLists.txt`; for local development set `AIO_INCLUDE_NON_AUTOUPLOAD=ON`.
-   **No logo:** `cs-logo.png` is intentionally absent (non-GPL); do not restore upstream assets. Logo draws are null-safe (`IconLoader.cpp`, `Menu.cpp`, `MenuHeaderRenderer`, `HomePageRenderer`).

---

## Comments

-   **Default to none.** Write one only when the WHY is non-obvious to a reader of this file and it passes three gates: (1) name the concrete "X changes → Y breaks silently"; (2) it is not derivable from adjacent code, and for a magic number you extract a named constant instead; (3) it is not project common knowledge.
-   **A survivor is one sentence, fact + consequence, 2 lines at most** (3-4 only as a rare absolute ceiling). Design rationale, rejected alternatives, calculations, history, "see commit/PR" pointers, and one-off incident or tool names (e.g. "the RenderDoc CTD") go in the PR body. No mid-function tutorials.
-   **Describe present code only,** never absent or removed code. Exception: a regression-risk warning naming removed code so it is not restored (e.g. "do not restore `cs-logo.png`").
-   **Doxygen** for all public declarations and API methods, especially graphics-related ones; keep it concise.

---

## Code Quality & Architecture

-   **No placeholders:** no TODO/FIXME or incomplete implementations unless explicitly requested for planning.
-   **Minimal churn:** do not reformat unrelated code, rename adjacent variables, add or remove stray blank lines, or reorder or relocate existing code (prefer a forward declaration over moving a definition). An unreviewable diff is a violation even when functionally identical. Extracting a helper to satisfy the function-size rule is fine when scoped to the function you are already touching.
-   **Descriptive naming** that states rendering purpose (`screenSpaceAmbientOcclusion`, `UpdateShadowCascades()`).
-   **Single responsibility:** each feature class handles exactly one graphics technique. Split newly authored functions over ~200 lines into helpers; do not refactor an untouched function just to meet the limit.
-   **Centralize constants:** magic numbers and UI theme values go in named constants (e.g. `ThemeManager::Constants`).
-   **DRY codebase-wide:** check new code against `src/Utils/` (`Serialize.h`, `Format.h`, `FileSystem.h`, `UI.h`) and `bshoshany-thread-pool` before writing your own. Always reuse `Util::SetResourceName`, `Util::GetGameSettingValue`, and the cached `globals::game::*` pointers (`Globals.h`) instead of `RE::*::GetSingleton()` (e.g. `globals::game::player`, `globals::game::isVR`).
-   **Feature boundaries:** no feature, core or not, may depend on a _non-core_ feature (calling it, reading its settings, `#include`ing its HLSL, or branching on its `IsEnabled()`/`loaded`), guarded or not: a non-core feature can be absent from a build. Depending on an `IsCore()` feature is fine. Central engine files (`Hooks.cpp`, `State.cpp`/`.h`, `Globals.h`) stay feature-agnostic beyond registration (`globals::features`) and generic hook points (`GetActiveConstraints()`, `DrawSettings`, `SetupResources`, `Draw`, `Prepass`); do not add a branch, field or hook wired to one feature's private need. A new interception point is exposed as a virtual any feature can use (see `docs/new-feature-template/`).
-   **New shared HLSL concerns get their own `.hlsli`** in `package/Shaders/Common/` rather than growing an existing shared header: a broad header makes every consumer's shader validation re-run and risks upstream merge conflicts. This does not mandate splitting existing headers.
-   **ImGui:** pair `BeginTable()` with `EndTable()`; use RAII for style changes; use central Theme constants for spacing; reach private methods through callbacks and keep UI state in `Menu`.
-   **Restart-gated config fields:** use `Util::Settings::BootSnapshot` + `kRestartFields` to diff boot-latched vs selected values (drives `Util::Text::RestartNeeded` and MCP/menu introspection; Upscaling is the canary).

---

## DirectX Naming

-   Name every D3D11 resource for RenderDoc with `Util::SetResourceName(ptr, "Feature::ResourceDescription")` after raw `device->Create*` calls. Wrappers (`Texture2D`, `Buffer`, `ConstantBuffer` in `Buffer.h`) take the name in the constructor and name their views.
-   Use `"Feature::Name"` for the resource and `"Feature::Name SRV"` / `"Feature::Name UAV"` for views. The implementation is in `Utils/D3D.cpp`; never duplicate the GUID or re-implement it inline.

---

## VR & Cross-Platform Policy

-   **Non-VR (flatrim: SE/AE) is the primary path;** VR is the minimum divergence necessary. Diverge only for genuinely VR-specific behavior (stereo projection, VR-only resources, different engine data paths), and keep VR branches small and localized rather than working around side effects.
-   **C++:** runtime checks on universal binaries; prefer the cached `globals::game::isVR` over `REL::Module::IsVR()` (direct calls only in early-init paths). **HLSL:** `#if defined(VR)` per shader permutation.
-   **VR-only files** (compiled exclusively for VR, no SE/AE path, not a fork of an upstream shared file) are exempt only from mirroring non-VR structure; every other rule here still applies to them.
-   Multi-runtime member offsets and virtual relocations: [Repository Architecture](docs/development/architecture.md).

---

## Performance & Profiling

-   **Pass instrumentation:** wrap every render pass entry point, new or ported, with `CS_GPU_PASS("Feature::Pass")` (RAII `ScopedGpuPass`, `src/GpuPass.h`): it wires the internal profiler, Tracy CPU/GPU zones and RenderDoc/PIX annotation. Never hand-roll `State::BeginPerfEvent`/`EndPerfEvent` or raw `TracyD3D11Zone` at a pass entry. Raw zones are only for sub-dispatches where profiler granularity isn't needed.
-   **Ported code:** swap another fork's raw annotations for `CS_GPU_PASS` during the port, not in a follow-up. Give the runtime and fallback dispatch paths of a feature distinct pass names (`Feature::RuntimeDispatch` vs `Feature::HostDispatch`) so they can be A/B'd in Tracy.
-   **Justifying speedups:** a `perf:` PR states a measured number at the PR level, normalized to the frame budget (VR 90fps ≈ 11.1ms, flatrim 60fps ≈ 16.7ms), never a raw wall-clock delta. Measure GPU per-pass cost with Tracy (`-DTRACY_SUPPORT=ON`, server on port 8086). A/B by toggling the feature (menu or devbench `openshaders.feature`) on the same scene, resolution and upscaler. Micro-optimizations that can't be isolated are `refactor:`, not `perf:`.

---

## Error Handling & Security

-   Features disable cleanly on shader-compile or DirectX failures and fall back to a working render path rather than crashing or corrupting state.
-   Validate shader parameters and buffer sizes to avoid GPU driver crashes.

---

## Testing

-   **Verify runtime-affecting changes (UI, features, shader cache) in game via devbench** on **VR plus one flat variant (SE or AE)** before calling them done. A release candidate still gets the full per-edition pass in [release validation](docs/development/release-validation.md). Say in the PR which runtimes ran in game.
-   A new feature or settings surface ships with a devbench action in the same PR. Changing a devbench-exposed tool means updating its `RegisterTool` description and `inputSchema` in the same PR, since `GET /api/tools` and agents trust them.
-   **Shader refactors:** identical DXBC is a provable no-op (`tools/verify-shader-refactor.ps1`); legitimate op-reordering needs runtime A/B frame diffing (`tools/taa-renderdoc-ab.py`).

---

## Git & Release Invariants

-   **PRs target `dev`, never `main`;** never push to a shared branch without explicit OK. `main` must remain an ancestor of `dev`; after a hotfix promotion the workflow auto-rebase-reconciles `dev` (its force-push is the one allowed exception).
-   **Commit-type traps:** a build/CI/test change labeled `fix:` burns a patch release (use `build:`/`ci:`/`test:`); a refactor labeled `feat:` forces a minor; an internal perf win is `refactor:`; prefer a specific type over `chore:`.
-   **Never, without explicit user direction:** force-push or rebase `main`, `dev` or `hotfix/*`; create `v*` tags (semantic-release owns them); bump `CMakeLists.txt`'s `VERSION`; PR a feature branch into `main`; run `Release: Semantic Version` on `hotfix/X.Y.x` for the current line (use `ff_target` into `main`).
-   **Upstream sync:** merge from `community-shaders/skyrim-community-shaders`, never cherry-pick; land sync PRs as merge commits, never squash; verify ancestry with `git merge-base --is-ancestor <upstream-sha> HEAD`; resolve conflicts in favor of keeping VR. The per-commit intent review and the "upstream never tests VR" checks are in the [Upstream Sync Guide](docs/development/upstream-sync.md#conflict-resolution-guidelines).
-   Release stages, staging/RC workflows and packaging targets: [Release Process](docs/development/release-process.md).

---

## Environment & Build Reference

-   **Build:** `./BuildRelease.bat [PRESET_NAME]` (from WSL: `powershell.exe -Command "./BuildRelease.bat [PRESET_NAME]"`). Configure presets in `CMakePresets.json`: `ALL` (default, universal SE/AE/VR), `ALL-VS2022`, `ALL-DEBUG`, `Dev-Fast`, `PR`, `Linux-ClangCL` (plus build presets `Dev`, `Debug`, `Package`, `Shaders`).
-   **Local presets:** many devs keep a gitignored `CMakeUserPresets.json` (from `CMakeUserPresets.json.template`) with deploy-enabled variants such as `ALL-WITH-AUTO-DEPLOYMENT` (`AUTO_PLUGIN_DEPLOYMENT=ON`, deploys to the local SE/VR `Data` dirs via `CommunityShadersOutputDir`), the preferred preset for a local test deploy. Check for the file before assuming a preset doesn't exist; it isn't in `git grep`.
-   **Linux/macOS-host cross-compile** (build-only): `cmake --preset Linux-ClangCL && cmake --build --preset Linux-ClangCL`; see [Linux/macOS Cross-Compile](docs/development/linux-macos-cross-compile.md).
-   **clangd:** after configuring `ALL`, `pwsh tools/gen-clangd-db.ps1`.
-   **New feature:** start from `docs/new-feature-template/`, inherit `Feature` (`DrawSettings()`, `LoadSettings()`, `SaveSettings()`, rendering hooks), and register in `globals::features`.
-   **Fast iteration:** use `Dev-Fast` for C++-only changes, the `CommunityShaders` target alone under a `*-WITH-AUTO-DEPLOYMENT` preset for a DLL-only deploy, `COPY_SHADERS` for shader-only; use a separate build directory or worktree per branch for A/B testing. Details: [Shader Development Workflow](docs/development/shader-workflow.md#fast-iteration-without-paying-a-full-shader-recompile).
-   More: [VSCode Setup](docs/development/vscode-setup.md), [Development README](docs/development/README.md), [Shader Development Workflow](docs/development/shader-workflow.md), [In-game A/B Testing](docs/development/shader-runtime-ab.md).

---

## Maintaining This File

-   **Update in the same PR,** not a follow-up, when a convention here changes.
-   **State each rule once;** the checklist is an index, not a second copy.
-   **Audit every 3-6 months:** cut ALL-CAPS bans that restate a capable model's native taste or contradict current practice.
-   **Prefer a doc link over inline detail** once a section would exceed ~8 lines.
