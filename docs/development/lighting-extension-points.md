# Lighting Shader Extension Points

How a per-material Lighting feature (first consumer: Detail Maps) gets per-draw data and a compiled
variant without spending a cbuffer slot or adding engine wiring that names the feature.

Status: implemented. Phase 0 (`src/LightingBudget.h`) and the Detail Maps port follow the design below,
which was revised after two independent reviews (deepseek and Gemini, with a rebuttal round).

## Resource budget

### Descriptor bits (`LightingShaderFlags`, `src/ShaderCache.h`)

| Bits      | Owner                                                    |
| --------- | -------------------------------------------------------- |
| 0-2, 9-23 | Engine                                                   |
| 3, 4, 5   | Shared with upstream (TruePbr, Deferred, LandscapeSeams) |
| 24-29     | Technique, decoded as `0x3F & (descriptor >> 24)`        |
| 30, 31    | Open Shaders (FurShells, FurShellsDepth)                 |
| 6         | Detail Maps (`kDetailMapsMask`)                          |
| 7, 8      | **Free: the only two bits left**                         |

Bit 29 is **not** usable. It is technique bit 5 and is decoded unmasked at about a dozen sites
(`Deferred.cpp`, `State.cpp`, `ShaderCache.cpp`, `LinearLighting.cpp`, `TruePBR.cpp`, `FurShells.cpp`,
`Wind.cpp`, `VanillaFresnel.cpp`, `LandscapeSeamsFix.cpp`). Setting it silently changes the technique.

Each bit can double the permutations it applies to, so compile cost binds before the bit space does.

### Constant buffers (measured from compiled `.pso`)

| Stage | Used                              | Free                       |
| ----- | --------------------------------- | -------------------------- |
| PS    | b0-b8, b10-b13 (b11 is flat only) | b9 (flat); b9 and b11 (VR) |
| VS    | b0-b8, b9/b10 (bones), b12        | b11                        |

No slot is free in both stages. `Common/Skinned.hlsli` declares b9/b10 unconditionally and
`Lighting.hlsl` includes it in every permutation, so b9 is occupied in the VS.

### Per-draw payload sizes

LinearLighting 16 B, Detail Maps 32 B, Skin 144 B, FurShells 704 B (bound at VS b7 and PS b10; it can
never join a shared PS buffer).

## What already exists (and the first draft missed)

- `Feature::HasShaderDefine(Type::Lighting)` / `GetShaderDefineName()` (`Feature.h`) is already a generic
  define hook used by many features. A descriptor bit with no matching define compiles nothing, because
  `AcquireSharedCompile` dedupes identical define sets.
- `PermutationCB` (b4) is uploaded per draw through a dirty check (`State::UpdatePermutationBuffer`) and is
  shared by Lighting, Utility, Grass and Sky. `ExtraShaderDescriptor` / `ExtraFeatureDescriptor`
  (`State.h`) are per-draw flag words with free bits and no permutation cost. Together they carry flags and
  scalars with zero extra binds.
- A core, feature-agnostic virtual-slot-0x6 hook for `BSLightingShader::SetupGeometry` is installed in
  `Hooks.cpp` and already calls `UpdateLightingShaderPermutation(pass)`. It is the pass-aware per-draw
  site; no new central hook needs inventing.
- `BSShader_BeginTechnique` and `State::ModifyShaderLookup` have no `RE::BSRenderPass*`, so they can
  consume a variant bit but never decide one. The load loops in `BSShader_LoadShaders` bypass
  `BeginTechnique`.

## Design

### Phase 0: ownership table, static asserts, reflection check

One header (`src/LightingBudget.h`) declares bit masks and cbuffer slots with owner, stage
(VS/PS) and VR/flat qualification, and marks bit 29 reserved. `static_assert`s check mask overlap and
that consumers use the named constants.

A source-text `register(bN)` lint cannot be authoritative: it cannot tell "declared" from "bound", and
`Skinned.hlsli` plus `FurShells.hlsli` collide textually while being stage-disjoint. The authoritative
check compiles Lighting for {VS, PS} x {flat, VR} x {base, FUR_SHELLS} and compares fxc reflection
"Resource Bindings" with the table, using the toolchain `tools/verify-shader-refactor.ps1` already uses. A
text pass may remain as a fast ownership pre-check with explicit exceptions.

### Phase 1: no shared buffer

The shared b9 per-draw buffer is cut. Detail Maps needs 32 B, which fits two `float4` appended to
`PermutationCB`; a shared buffer would cost every Lighting draw an extra `Map`/`Unmap`/bind for one
consumer, and `PSSetConstantBuffers1` windows must be multiples of 256 B so 64 B slots cannot be windowed.
b9 stays reserved for a future consumer that needs more than 16 B of per-draw PS data and cannot share b4.

Skin, LinearLighting and FurShells do not migrate.

### Phase 2: generic variant flag and define, feature-owned selection

`Feature` exposes `GetLightingVariantFlag()` and `GetLightingVariantDefine()`. `GetLightingShaderDefines`
masks every loaded feature's flag out of the descriptor before the engine call, then appends the define
of each flag that was set, so no central file names a feature.

The feature picks the variant itself. It installs its own `BSLightingShader` vfunc 0x6/0x7 hooks (the
FurShells pattern), and after the engine's `SetupGeometry` swaps in the pixel shader looked up with the
flag set, then restores the previous shader in 0x7. This handles per-frame decisions such as distance
fade. `GenerateShaderPermutations` declares the variants to precompile.

Variants that combine TRUE_PBR with a non-default technique do not exist in the engine; the feature's
eligibility check rejects them so they are neither precompiled nor requested at draw time.

### Phase 3: runtime branch before permutation bit (policy)

A feature takes a descriptor bit only when the compiled output genuinely differs. Enable flags, fade and
scalar parameters go through `ExtraFeatureDescriptor` and `PermutationCB`. Bits 7 and 8 are the only
remaining bits and bit 29 is reserved.

### Phase 4: wider shader key (dropped)

`GetDiskPath` keys on the 32-bit descriptor and `GetShaderString(hashkey)` keys on defines, so extending
`MakeId` alone keys neither and two variants would collide on one `.pso`. Revisit only if bits 7-8 run out.

## Detail Maps port

- Descriptor bit `1<<6`, a `DETAIL_MAPS` define, precompiled via `GenerateShaderPermutations`.
- 32 B of per-draw data as `float4 PerDrawFeatureData[2]` in `PermutationCB`, filled from the feature's
  own 0x6 `SetupGeometry` hook.
- `TexDetail` SRV at t23 (unused in the Lighting include closure), bound after the engine's `SetupGeometry`.
- `DetailMaps.hlsli` and the three `Lighting.hlsl` hooks under
  `#if defined(DETAIL_MAPS) && !defined(MODELSPACENORMALS) && !defined(LANDSCAPE)`.
- Do not copy Bottle's bit 31 or b8 (collide with FurShellsDepth and LinearLighting).
- The fade `saturate((FadeEnd - depth) * FadeInvRange)` is correct: `depth` is positive world distance at
  the `Lighting.hlsl` call site.
- Bottle is a downstream fork: cherry-pick with `-x` where hunks apply, keep its GPL-3.0 plus Modding
  Exception notice, and keep the CC0 ambientCG texture attribution.

## Verification

Runtime-affecting phases are verified in-game via devbench on VR plus one flat variant.
`tools/verify-shader-refactor.ps1` must show identical DXBC for permutations that do not use the new bit.

## Risks

| Risk                                                                | Mitigation                                                                                               |
| ------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| Table drifts from shaders                                           | Planned, not yet enforced: reflection check over the 8-compile matrix                                    |
| `PermutationCB` grows and every consumer (Utility, Grass, Sky) pays | Append only 32 B; measure upload cost before adding more                                                 |
| Variant bit lost between `GetRenderPasses` and `BeginTechnique`     | Same pattern as `LandscapeSeamsFix`; covered by the in-game check                                        |
| Upstream merge reintroduces a collision                             | Static asserts fail the build on bit-mask overlap; binding collisions await the planned reflection check |
