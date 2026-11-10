# GitHub Copilot Instructions

A digest of [`AGENTS.md`](../AGENTS.md), which is authoritative and wins on any conflict. It is
repeated here because Copilot Chat on GitHub.com reads this file but not `AGENTS.md`.

-   **Project:** SKSE plugin for DirectX 11 graphics on Skyrim SE/AE/VR, a fork of Community Shaders. The runtime identity stays `CommunityShaders` (DLL, settings path, log); the public name is "Open Shaders".
-   **PRs and git:** Conventional Commits `type(scope): description`, title <= 50 chars, body wrapped to 72, target `dev` (never `main`). Never force-push shared branches, hand-create `v*` tags, or bump the `CMakeLists.txt` version. Merge upstream, never cherry-pick.
-   **Comments:** none by default. Write one (2 lines at most) only when the WHY is non-obvious and a future edit would silently break without it. Describe present code only. Doxygen on public declarations.
-   **Code:** minimal churn (touch only what the change requires), no placeholders, reuse `Util::SetResourceName`, `Util::GetGameSettingValue` and the cached `globals::game::*` pointers. No feature may depend on a non-core feature.
-   **Graphics:** name every D3D11 resource with `Util::SetResourceName`; wrap every render pass entry point in `CS_GPU_PASS("Feature::Pass")`.
-   **VR:** non-VR is the primary path; keep VR divergence minimal (`globals::game::isVR` in C++, `#if defined(VR)` in HLSL).
-   **Build (Windows):** `./BuildRelease.bat ALL`. Linux/WSL can only cross-compile with `Linux-ClangCL`; shaders need Windows.
-   **Testing:** verify runtime-affecting changes in game via devbench on VR plus one flat variant (SE or AE).

Detailed workflows are under `docs/development/`.
