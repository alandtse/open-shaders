# Lighting Shader Extension Points

Design for adding per-material features to the Lighting shader (first consumer: Detail Maps) without
spending a descriptor bit, a cbuffer slot and a hand-written engine hook on each one.

Status: proposal. Nothing here is implemented.

## Problem

Every Lighting feature that varies per draw currently reaches into three scarce, globally shared resources.

### Descriptor bits (`LightingShaderFlags`, `src/ShaderCache.h`)

| Bits      | Owner                                                                                    |
| --------- | ---------------------------------------------------------------------------------------- |
| 0-2, 9-23 | Engine                                                                                   |
| 24-29     | Engine technique (`0x3F & (descriptor >> 24)`, max technique 19, bit 29 never set today) |
| 3, 4, 5   | Shared with upstream (TruePbr, Deferred, LandscapeSeams)                                 |
| 30, 31    | Open Shaders (FurShells, FurShellsDepth)                                                 |
| 6, 7, 8   | Free (Bottled-Shaders uses them for its own flags)                                       |
| 29        | Free in practice, risky                                                                  |

Four bits remain. Each bit can double the permutations it applies to, so compile cost runs out before the
bit space does.

### Pixel-shader constant buffers (Lighting PS, measured from compiled `.pso`)

b0 PerTechnique, b1 PerMaterial, b2 PerGeometry, b3 LightLimitFix, b4 PerShader, b5 SharedData,
b6 FeatureData, b7 Skin, b8 LinearLighting, b10 FurShells, b11 AlphaTestRef, b12 PerFrame, b13 VR.
**b9 is the only free slot** (D3D11 allows 14).

Skin, LinearLighting and FurShells each own a slot for a small per-draw struct that they update and bind on
every draw. Slots are consumed by feature count, not by data size.

### Engine wiring

Variant selection is feature-specific. FurShells picks its variant in its own draw path
(`GetPixelShader(*shader, state->modifiedPixelDescriptor | FurFlag)`); Bottled-Shaders' Detail Maps adds
bespoke branches to `State.cpp` and `ShaderCache.cpp`. Neither can be reused by a third feature.

### Why Bottle's Detail Maps cannot be copied as-is

It sits on bit 31 (ours: FurShellsDepth) and cbuffer b8 (ours: LinearLighting), and its engine wiring names
the feature in central files, which `AGENTS.md` forbids. The shader code (`DetailMaps.hlsli`, the three
`Lighting.hlsl` hooks) ports cleanly once those are decoupled.

## Goals

1. A new per-material feature costs **no new descriptor bit** unless it truly changes the compiled code.
2. A new per-draw feature costs **no new cbuffer slot**.
3. Central engine files stay feature-agnostic (`Hooks.cpp`, `State.cpp`, `ShaderCache.cpp`).
4. Existing features migrate incrementally; nothing breaks if a phase stops early.
5. Slot and bit ownership is declared in one place and enforced by the build.

## Non-goals

Widening the descriptor to 64 bits, or restructuring TruePbr/Deferred bits shared with upstream.

## Design

### Phase 0: ownership registry and lint

One header, `src/LightingBudget.h`, lists every Lighting descriptor bit and PS/VS cbuffer slot with its
owner. `static_assert`s check that bit masks do not overlap and that `FurShells` and the other consumers
reference the registry constants instead of local literals.

A repo lint (`tools/`, run in CI with the existing architecture lints) compares the registry against
`register(bN)` declarations in the Lighting shader include closure, so a new `register(b9)` collision fails
CI rather than a runtime bind.

Cost: small, no behavior change. Delivers the "will we run out" answer as an enforced number.

### Phase 1: one shared per-draw feature-data buffer

Reserve **b9** as `PerDrawFeatureData`, a single constant buffer holding a fixed-size slot per participating
feature (for example 64 bytes each, indexed by a registry constant).

- A `Feature` virtual, `WritePerDrawData(const DrawContext&, std::span<std::byte> slot)`, is called once
  per Lighting draw for features that opt in via `WantsPerDrawData()`.
- The engine maps the buffer once per draw, lets each opted-in feature fill its slot, unmaps and binds b9.
  This is one `Map`/`Unmap` and one `PSSetConstantBuffers` per draw regardless of feature count, replacing
  the per-feature Map/Update/Bind that Skin, LinearLighting and FurShells each do today.
- If measured per-draw cost matters, switch to a ring buffer plus `PSSetConstantBuffers1` offsets; the
  GrassOptimizations feature already uses that windowing pattern.
- HLSL reads `PerDrawFeatureData.<Feature>` through a generated or hand-maintained `.hlsli` in
  `package/Shaders/Common/`.

Migration is optional and incremental: Skin (b7), LinearLighting (b8) and FurShells (b10) can move into b9
slots one at a time, each freeing a slot. Detail Maps is the first consumer and needs no migration.

Open question: Skin and LinearLighting are core features; confirm that the shared buffer's base type stays
core so non-core features can depend on it without breaking the feature-boundary rule.

### Phase 2: generic lighting-variant provider

Replace per-feature branches in `State.cpp`/`ShaderCache.cpp` with one `Feature` virtual:

```cpp
virtual std::uint32_t SelectLightingVariant(const LightingDrawInfo&) const { return 0; }
```

The returned bits are OR-ed into the descriptor at the single place that already builds
`modifiedPixelDescriptor`. `GenerateShaderPermutations` (existing) declares which variants must be
precompiled. A feature that returns 0 costs nothing; central code names no feature.

FurShells can adopt it later, which also retires its private `GetPixelShader` call.

### Phase 3: runtime branch before permutation bit

Policy, not code. A feature gets a descriptor bit only if the compiled output genuinely differs (a texture
sampler stage, a different code path that cannot be dynamically skipped). Anything gated by a material flag
or distance fade uses a uniform branch driven from the Phase 1 buffer. Detail Maps needs exactly one bit
(to compile the sample code at all) and one `TexDetail` SRV; its fade and enable flags are runtime.

Bits 6, 7, 8 are reserved for this tier. Bit 29 stays reserved against engine technique growth.

### Phase 4 (only if the 32-bit descriptor genuinely fills)

`MakeId` already packs the descriptor in bits 0-31, shader type in 32-59 (28 bits, mostly unused) and class
in 60-63. An `extension` field can live in the spare type bits without widening the ID. On-disk cache file
names also carry the descriptor, so a new field changes the cache key and needs a cache version bump. Defer
until a feature cannot be expressed as a runtime branch.

## Detail Maps port (first consumer)

Port in this order so each step is shippable on its own:

1. Phase 0 registry (no behavior change).
2. Phase 1 b9 buffer with Detail Maps as its only slot.
3. Phase 2 variant provider hook returning the single Detail Maps bit.
4. `DetailMaps.hlsli` and the `#if defined(DETAIL_MAPS) && !defined(MODELSPACENORMALS) && !defined(LANDSCAPE)`
   hooks in `Lighting.hlsl`, with Bottle's known bugs fixed (inverted fade sign; bit 31 and b8 collisions).
5. Rule files (`Rules/Default.json`, path-based) and the reload control.

Attribution: Bottled-Shaders is a downstream fork, so cherry-pick with `-x` where hunks apply cleanly. The
texture pack's CC0 ambientCG sources are compatible with our licensing.

## Verification

Each phase that touches runtime behavior is verified in game via devbench on VR plus one flat variant
(`AGENTS.md` testing rules). Phases 1 and 2 additionally need:

- `tools/verify-shader-refactor.ps1` showing identical DXBC for non-participating permutations.
- A Tracy GPU/CPU A/B of per-draw cost with Skin and LinearLighting migrated versus not, if Phase 1
  migration is pursued, normalized to the frame budget.

## Risks

| Risk                                                    | Mitigation                                                   |
| ------------------------------------------------------- | ------------------------------------------------------------ |
| Shared buffer becomes a contention point (map per draw) | Ring buffer plus `PSSetConstantBuffers1`; measure first      |
| Registry drifts from shaders                            | CI lint compares `register(bN)` to registry                  |
| Upstream edits `Lighting.hlsl` bit/slot usage           | Registry lint fails on a merge that reintroduces a collision |
| Feature-boundary rule violated by core buffer           | Base type is core; features only write their own slot        |
