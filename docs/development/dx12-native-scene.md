# open-RT DX12 pipeline

Branch-specific implementation map, checked against `open-RT` source on 2026-11-10. This is an ownership reference, **not** a runtime validation report. For shared build and development guidance, use [the development index](README.md).

## Owners

| Component | Current responsibility | Source |
| --- | --- | --- |
| DX12 Resources | Interior discovery, companion CELL preparation, on-disk cache and streaming requests | `src/Features/DX12Resources/`, `tools/dx12-resources-prep/` |
| DX12 Native Scene | Prepared/current geometry, material residency, BLAS/TLAS generations, runtime capture, independent early ray-shadow mask | `src/Features/DX12NativeScene/` |
| Native LLF / SCM | Prepared + observed light unification; shadow-slot/caster ownership from Skyrim observations | `src/Features/NativeFeatures.{h,cpp}` |
| Native PBR | TruePBR capture/registry and native material bridge | `src/TruePBR/`, `src/Features/DX12NativeScene/Material*.cpp` |
| Native Lighting | Optional native direct diffuse/specular and early-shadow culling; separately controlled from independent shadows | `src/Features/NativeLighting/` |
| Native Composite | Frame-scoped DX12 raster composition with optional native direct lighting and independent shadow layer | `src/Features/NativeComposite/` |
| Upscaling | Runtime FSR interop and native frame input/output contracts | `src/Features/Upscaling/` |

## Current boundaries

1. Skyrim/DX11 still supplies raster pixels and selected scene, material, light and caster observations. Those inputs do not make their DX11 collection code native DX12.
2. Native Scene owns completed scene/TLAS and material generations. The shadow mask has its **own** dispatch and must not require Native Lighting to be enabled.
3. Native Lighting independently evaluates DX12 direct diffuse/specular. Selection, sparse trace-density modes and validity controls are runtime adjustable; raw skipped samples are not proof of evaluated pixels.
4. Native Composite combines available frame-scoped inputs on DX12. Resource and fence ownership must survive scene transitions and downstream consumers.
5. The DX12 runtime upscaler/FG boundary still uses interop. Do not describe the whole presentation path as DX12-only or FSR Ray Regeneration as completed.

## Implementation constraints

- Keep independent feature controls; no implicit renderer-family exclusions or hidden cross-feature activation.
- Preserve full-size native resources where trace-density modes sparsely dispatch rays. Distinguish producer raw masks from optional compositor-side filtering.
- Use shared DXGI local-memory budget/headroom policy in `src/Features/DX12NativeScene/VideoMemoryBudget.h`; do not invent fixed VRAM availability.
- Native HLSL6/DXC dispatch coexists with legacy shader compilation. Native-only compilation is not an established finished path.
- Test and log actual scene/TLAS publication, LLF/SCM identity matching, independent shadow submissions, density mode behavior, composite output and memory budget. Do not infer GPU correctness from successful compilation or nonzero counters.

This page replaces branch-specific progress diaries, one-off experiments and stale speculative audits. Update it only to reflect current source behavior.
