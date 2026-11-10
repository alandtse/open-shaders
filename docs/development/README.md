# open-RT: current native rendering map

This is the small, source-aligned technical index for `open-RT`, reviewed against the branch on **2026-11-10**. It describes code ownership, not a claim that all paths have passed in-game testing.

| Owner | Responsibility / authoritative source |
| --- | --- |
| `DX12NativeScene` | Prepared CELL publication, scene/BLAS/TLAS state, raster capture, native materials, runtime geometry, early independent shadow tracing: `src/Features/DX12NativeScene/` |
| `DX12Resources` | Interior identification, companion preprocessing, cache preparation, streaming requests: `src/Features/DX12Resources/`, `tools/dx12-resources-prep/` |
| `NativeLLF` | Prepared + observed light identity/unification, native scene light snapshots: `src/Features/NativeFeatures.{h,cpp}` |
| `NativeSCM` | Selected shadow-caster ownership and engine-to-native caster observation: `src/Features/NativeFeatures.{h,cpp}` |
| `NativePBR` | Captured TruePBR material snapshots and native-side bridge: `src/TruePBR/NativePBRRegistry.h`, `src/Features/DX12NativeScene/Material*.cpp` |
| `NativeLighting` | Independent DX12 direct diffuse/specular output and optional early shadow culling: `src/Features/NativeLighting/` |
| `NativeComposite` | Frame-scoped DX12 raster capture/composition, optional direct contribution and independent shadow layer: `src/Features/NativeComposite/` |
| `Upscaling` | FSR runtime interop and native-upscaler input/output contracts: `src/Features/Upscaling/` |

## Execution boundaries

1. Skyrim/DX11 supplies raster color, depth and observations. A native feature taking ownership does **not** imply the game's originating hooks move to DX12.
2. DX12 Resources prepares data; Native Scene publishes completed scene generations and handles acceleration resources and independent early shadow traces.
3. Native Lighting may dispatch native direct diffuse/specular against that scene. Its toggle does not gate the independent shadow layer.
4. Native Composite captures raster color and may apply native lighting/shadows. The upscaling/presentation boundary is still under development; do not state that all frames remain on DX12 afterward.
5. Trace-density controls can intentionally leave unevaluated samples. Producers preserve their native-sized resources; distinguish raw output from optional composite-side spatial operations.

## Active constraints

- Keep raster/legacy capture as an input baseline. Do not treat DX12 and DX11 as interchangeable execution owners.
- Keep feature switches independent; do not silently merge lighting, shadows, and PBR toggles.
- Native scene/light/material generations and consumer fences must be coherent across frame and scene transitions.
- DXGI local-memory telemetry and a shared headroom policy control native allocation decisions (`VideoMemoryBudget.h`). Do not invent a fixed guaranteed reserve.
- Native HLSL6/DXC assets coexist with legacy shader compilation. A native-only shader/compiler runtime is **not** documented as implemented.
- Dynamic geometry/character coverage, PBR residency, streamer readiness, and native upscaling require explicit runtime evidence. Do not infer success from code presence or diagnostics alone.
- FSR Ray Regeneration is outside the present minimal production path; do not document it as enabled or complete.

## Validate changes

Use the in-game feature panels and diagnostics to compare against source, with special attention to scene/TLAS publication, selected shadow submissions, native LLF/SCM light/caster matching, raw pixel density, composite results, and DXGI budget. Record branch/commit, test cell, toggles, observed frame behavior and relevant logs. In-game verification is owned by the tester, not inferred from compilation.

Build: from the repository root, run `cmake --preset ALL` and `cmake --build --preset ALL --config Release` using the Windows toolchain. For shader/C++ test targets, consult `CMakeLists.txt` and `tests/`. For companion data serialization, the retained [FORMAT.md](../../tools/dx12-resources-prep/FORMAT.md) is authoritative.

**Maintenance:** update this single index only when ownership or current behavior changes. Put debug histories and proposed designs in issues/PRs, not permanent docs.
