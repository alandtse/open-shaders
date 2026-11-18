# NativePBR eight-MRT + `u8` feasibility probe

Status: **isolated, unverified, opt-in source experiment**. It is not part of the Skyrim plugin, does not install hooks, and cannot affect game draws.

## Phase 1: compile, bind, read back

Run this on the Windows machine with the actual graphics driver, from an **x64 Native Tools Command Prompt for Visual Studio 2022** in the repository root:

```bat
cl /nologo /std:c++20 /EHsc /W4 tools\nativepbr-uav-probe\Probe.cpp /Fe:NativePBRUavProbe.exe /link d3d11.lib d3dcompiler.lib dxgi.lib
NativePBRUavProbe.exe > NativePBRUavProbe.log 2>&1
type NativePBRUavProbe.log
```

The probe creates its own hardware D3D11 device; it requests feature level 11_1, falling back to 11_0 only to report whether the experiment is unavailable. It prints the actual feature level and whether `ID3D11Device1` exists. It calls the Windows FXC-compatible `D3DCompile` entry point for `vs_5_0` and `ps_5_0` without skipping validation.

The pixel shader declares eight `SV_Target0..7` outputs **and** `RWTexture2D<uint>` at `register(u8)`. The test creates eight separate render targets, a separate `R32_UINT` UAV, and a depth-stencil view; it binds all of them using `OMSetRenderTargetsAndUnorderedAccessViews(8, ..., 8, 1, ...)`. It queries the bound resources, draws one fullscreen triangle, copies GPU results into CPU-readable staging textures, and validates all 16 identity pixels and all eight MRT images independently.

`EXITCODE=0` means all Phase 1 checks passed **on the standalone device**. A nonzero exit code means Phase 1 failed. Preserve the full log, particularly the compiler diagnostic and the printed feature level. Do **not** interpret a compilation or device creation failure as evidence about depth ordering.

This test does not use Skyrim's device/context, its actual render targets, or its shader cache. Both D3D11 device-creation paths in the plugin request feature level 11_1 (`src/Hooks.cpp` and `src/Features/Upscaling.cpp`), but an actual runtime/device capability assertion still requires inspecting the game session. The isolated test's synthetic target 0 and 1 formats are not assertions about the game's dynamically configured resources.

## Phase 2: not validated

Even if Phase 1 passes, the following require separate controlled game/device tests before enabling any producer path:

1. Depth/stencil rejection versus shader UAV side effects, with and without `earlydepthstencil`.
2. Two overlapping native draws and later rejected native draws, with readback of final identity and depth.
3. Native draw subsequently occluded by an ordinary non-native draw; check whether the UAV contains a **stale** native identity and define a final-visibility rejection mechanism.
4. Alpha `discard`, alpha-to-coverage, MSAA and stereo/VR behavior.
5. Per-frame clear, draw-ordinal rollover and exact draw-table generation association.
6. Cross-API resource sharing, DX11 producer signal / DX12 queue wait, and retention until consumer completion.
7. GPU pass timing against the existing input-only and sidecar-replay paths.

Pixel-shader UAV accesses are **not** ordered like RTV color writes and cannot be assumed to respect final raster visibility. Do not advance the single-draw ABI or eliminate sidecar replay based only on an `EXITCODE=0` here. The eight conventional RTVs must remain bound, and the native output path must stay disabled until Chat 1 approves a visibility-correct integration.

Relevant API specifications:

- [D3D11.3 Functional Specification — 64 combined graphics UAV/RTV slots for D3D11.1+](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
- [OMSetRenderTargetsAndUnorderedAccessViews](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargetsandunorderedaccessviews)
