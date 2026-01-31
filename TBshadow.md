# TB Rectangular Shadow Artifact (VR) - Findings and Current Debug State

## Summary of the issue
- In VR with Terrain Blending (TB) enabled, a rectangular, HMD-moving dark "shadow" appears on the ground.
- Disabling TB removes the artifact.
- The artifact is not Screen Space Shadows (SSS) or Terrain Shadows; toggling those had no effect.

## Confirmed via RenderDoc
- The rectangle is visible in kSHADOW_MASK and is written during Utility pixel shadowmask passes (RenderShadowmask / Spot / PB / DPB).
- The shadowmask pass samples:
  - Slot 2 = TexDepthUtilitySampler
  - Slot 4 = TexShadowMapSamplerComp (kSHADOWMAPS)
  - Slot 14 (Lighting) = TexShadowMaskSampler (kSHADOW_MASK)
- Pixel history shows the offending region being written during Utility shadowmask passes, not later composites.

## What definitely affects the artifact
- Force White Shadow Mask (slot-14 override in Lighting pass)
  - Removes the rectangular shadow reliably.
  - This proves the artifact lives in kSHADOW_MASK (the shadowmask output) and not later lighting/composite stages.

## Key findings since the last update (depthblend changes excluded)

### 1) Depth source overrides do not fix the rectangle
- Slot-2 depth overrides (Engine Main, Engine Prepass, shadowmask depth copy) do not change the rectangle.
- Some slot-2 overrides produce a uniform dark overlay on the mesh that masks the rectangle, but the rectangle is still present underneath.
- This indicates the rectangle is not caused by the depth source itself.

### 2) Shadowmap slot overrides do not fix the rectangle
- Slot-4 overrides (force white shadowmap) did not remove the rectangle.
- Slot-5/6 overrides did nothing in any combination.

### 3) DPB debug modes did not change the artifact
- Force Fade + Visibility = 1 (DPB) had no visible effect.
- No Discard (DPB) had no visible effect.
- Variant debug (grayscale / color) was not visible, even though logs showed ShadowmaskDPB variants.

### 4) Disabling specific shadowmask variants narrows the culprit
- Disable ShadowmaskDPB (conditions: numShadowLights==0, kSHADOW_MASK bound, alphaTest==0)
  - Looks the same as Force White Shadow Mask (slot-14): rectangle disappears, but other shadowmask content is also lost.
  - Logs show DPB variant with numLights=0, shadowLights=0, alphaTest=0, pixDesc=0x1062002 when the rectangle appears.
- Disable Shadowmask (Base)
  - Reduces the main rectangle, but leaves faint new rectangular shapes (glossy/transparent) at different positions.
  - Other shadows in the scene appear mostly unaffected in this test scene.
  - Suggests the base shadowmask pass contributes to the footprint, but DPB likely amplifies or "fills" it.
- Disable ShadowmaskSpot / ShadowmaskPB
  - No visible effect.

### 5) Combination required to fully suppress the rectangle without dark overlay
- Skip ShadowmaskDPB + Skip Only When Shadowmask Bound + Clear Shadowmask When Bound
  - This combination removed the rectangle without the uniform dark overlay.
  - Any one toggle missing brought back the dark overlay or the rectangle.
  - Indicates stale or garbage data in kSHADOW_MASK is likely involved when DPB runs with no shadow lights.

### 6) Terrain depth offset in Utility VS materially affects the artifact
- Changing `vsout.PositionCS.z += 10.0` to `+= 5.0` (OFFSET_DEPTH path in Utility.hlsl) made a big difference in the rectangular shadow.
- This suggests the artifact strength is sensitive to the TB terrain depth bias used in the offset depth pass.

### 7) Final fix applied
- Hardcoded the OFFSET_DEPTH bias to `vsout.PositionCS.z += 1.25;` in Utility.hlsl.
- This removes the rectangular shadow artifact in testing.

## Current interpretation (based on tests)
- The rectangle is generated during Utility shadowmask passes (Base + DPB).
- The DPB pass is strongly implicated; it runs even when numShadowLights==0 and seems to write garbage or stale data into kSHADOW_MASK.
- The Base pass contributes to the rectangle footprint but is not the only source.
- Depth source swaps and shadowmap slot overrides do not solve the issue, so the problem is likely in shadowmask math, uninitialized inputs, or incorrect state during Utility shadowmask passes when no shadow lights are present.

## Current debug controls in TB (Developer Mode)
Only these two remain (everything else was removed after testing):
- Force White Shadow Mask (Debug)
  - Overrides Lighting slot-14 (kSHADOW_MASK) with a 1x1 white texture.
  - Only reliable control that removes the rectangle completely.
- Disable Shadowmask (Base) (Debug)
  - Clears kSHADOW_MASK to white and skips the base shadowmask draw when bound.
  - Reduces the rectangle but leaves faint, shifted rectangular remnants.

## Logging highlights (what matters)
- Shadowmask pass logs show the rectangle is written during Utility shadowmask passes.
- The DPB variant appears with: numLights=0, shadowLights=0, alphaTest=0, pixDesc=0x1062002.
- ShadowmaskRTV matching confirms these passes are writing into kSHADOW_MASK.

## Repro steps (current baseline)
1. Enable TB in VR.
2. Observe rectangular moving shadow on ground.
3. Toggle Force White Shadow Mask -> rectangle disappears.
4. Toggle Disable Shadowmask (Base) -> rectangle mostly disappears, faint rectangular remnants remain.

## Most likely root cause (current best guess)
When there are no shadow lights, the engine's Utility shadowmask path (Base + DPB) still writes to kSHADOW_MASK, likely using invalid inputs or stale data. TB changes the depth pipeline and exposes this, but the artifact itself is generated by the shadowmask passes.

## Where to look in code
- src/Globals.cpp
  - ID3D11DeviceContext_PSSetShaderResources::thunk (slot-14 override)
  - ID3D11DeviceContext_DrawIndexedInstanced::thunk (Disable Shadowmask Base)
- src/Features/TerrainBlending.cpp
  - DrawSettings() (debug toggles)
  - SetupResources() (shadowmaskWhite creation)
- package/Shaders/Utility.hlsl
  - Shadowmask math for RenderShadowmask / RenderShadowmaskDPB paths
