# Reverse Z: depth consumers that must be converted

`ReverseZ` (feature short name `ReverseZ`) reallocates the scene depth targets to
`R32G8X24_TYPELESS` / `D32_FLOAT_S8X24_UINT`, makes the camera publish a reversed
projection, and flips depth-stencil comparison, depth-bias and viewport depth range for
every pass drawing into a converted target. From then on the depth buffer holds
`1 - standardDepth` and every _shader that reads depth_ sees the inverted convention.

`EnableReverseZ` is restart-gated and ships **off**. Do not turn it on by default until it has
been validated on SE/AE **and** VR (`docs/development/release-validation.md`).

## Two rules

1. **Standard-space depth** (comparisons, linearization, thickness tests, sky/far-plane
   sentinels): wrap the raw read in `FrameBuffer::ToStandardDepth(...)`
   (`package/Shaders/Common/ReverseZ.hlsli`). It is identity when `REVERSE_Z` is undefined.
2. **Native-space depth** (anything fed back into `CameraProjInverse` /
   `CameraViewProjInverse`, or written out to another depth target): keep the raw read and
   wrap only a value that was already converted, with `FrameBuffer::ToNativeDepth(...)`.
   `Stereo::ConvertMonoUVToOtherEye` / `ResolveMonoUVForEye` (`Common/VRReproject.hlsli`)
   unproject their `z`, so they take native depth.

Both are `1 - x`, so a wrong choice is not a crash: it is a wrong pixel or a wrong fade.

`Common/ReverseZ.hlsli` provides the helper set: `ToStandardDepth` / `ToNativeDepth` (float and
vector forms), `ToStandardClipZ`, `ToStandardClip`, `OffsetClipDepth`, `IsReverseProjection`,
`FarPlaneDepth` / `NearPlaneDepth`, `FarPlaneClipZ`, the ordering helpers `NearerDepth`,
`FartherDepth`, `NearestDepth`, `FarthestDepth` and `IsNearerDepth`, and the
`SCENE_DEPTH_FORMAT` texture element type (`float` under `REVERSE_Z`, `unorm float` otherwise).
Everything is identity unless `REVERSE_Z` is defined, so wrapping a call is free when the
feature is off. C++ reads the same state through `ReverseZ::IsActive()`, `GetFarDepth()` and
`GetNearDepth()`.

## Status of the converted set

`ReverseZ::SetupDepthTargets()` converts `kMAIN`, `kMAIN_COPY`, `kDECAL_OCCLUSION`,
`kPOST_ZPREPASS_COPY`, `kPOST_WATER_COPY`. `kCUBEMAP_REFLECTIONS`, the shadow maps and the
VR-only targets keep the standard convention, so a consumer reading one of those needs no
conversion at all. The feature reverses every published camera whose target _is_ converted,
for both VR eyes.

## Handled: raster-time consumers

These write or test depth through a converted target and need no shader change; the feature's
detours cover them (comparison func, depth bias sign, viewport depth range, clear value).

| Consumer                                                                                                  | Note                                                                                                                   |
| --------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| Engine depth writes / tests on `kMAIN` (Lighting, Effect, Water, DistantTree, Grass, Sky, Particle, Tree) | Depth-stencil state and depth bias are flipped per pass by the feature.                                                |
| `ClearDepthStencilView` on a converted target                                                             | Clear value is mirrored.                                                                                               |
| Reflection cubemap face renders into a converted depth view                                               | The feature substitutes the cubemap's own (unconverted) depth view so the capture does not write into the scene depth. |

## Adding a new depth consumer

-   Read raw depth only through the rules above; never hard-code `1.0` as "far" or `0.0` as
    "near": use `FarPlaneDepth()` / `NearPlaneDepth()`.
-   Min/max reductions and nearer/farther tests flip meaning: use the ordering helpers. A Hi-Z or
    SPD chain that keeps the farthest depth seeds with `NearPlaneDepth()` and pads out-of-bounds
    texels with `FarPlaneDepth()`.
-   Declare depth textures bound to converted targets as `Texture2D<SCENE_DEPTH_FORMAT>`.
-   Stereo helpers that unproject (`Stereo::ConvertMonoUVToOtherEye`, `ResolveMonoUVForEye`,
    `ReprojectToOtherEye`) take native depth.
-   Shaders compiled through `Util::CompileShader` get `REVERSE_Z` automatically while the feature
    is active; engine shader types get it through `ReverseZ::HasShaderDefine`.

## Known open items

-   VR engine occlusion culling tests its OBB proxies against `kMAIN_DOWNSAMPLE`, so that target
    is converted with the others. Three engine pieces assume standard depth and are replaced while
    reverse-Z is active: the downscale pixel shader keeps `max` (the nearest sample under reverse-Z),
    so `OccluderDepthDownscalePS.hlsl` keeps `min`; the proxy vertex shader gives boxes reaching
    behind the camera a fixed clip depth of 0.0001 (the far plane under reverse-Z, which culled the
    near water cells), so `OccluderBoxVS.hlsl` uses 1.0. If culling misbehaves under reverse-Z, check
    these first.
-   The VR hidden-area mask mesh writes a fixed clip depth of 0 that bypasses the camera matrix, so it
    landed on the far plane and masked nothing. `HiddenAreaMeshVS.hlsl` writes 1.0 for it, applied to
    the only small non-indexed draw with no pixel shader bound.
-   `EnableReverseZ` ships off and is restart-gated; toggling it rebuilds the shader cache.
-   `VRStereoOptimizations/DepthScatterCS.hlsl` keeps an `#ifdef` for its `InterlockedMin`/`Max`
    on depth bits (no float helper applies).
-   Relative raw-depth thresholds in the VR stereo passes (`StencilCS`, `GBufferFillCS`,
    `VRPostProcessCS`, `StereoBlendCS`) keep their tuning; under `REVERSE_Z` their denominator is
    the nearer depth, which shifts the effective threshold.
-   `GBufferFillCS`'s epipolar fallback picks the farthest agreeing candidate in the standard
    convention only; it affects the zero-weight fallback path.
-   `BSImagespaceShaderWorldMapNoSkyBlur` stays on the vanilla shader: there is no replacement.
-   The VR hidden-area mask lands at `NearPlaneDepth()`; `ClearHMDMaskCS`, `bend_sss_gpu.hlsli` and
    SSGI's `prefilterDepths.cs.hlsl` all test that value.
-   Inherited from the flat original: `IsReverseDepthView` caches its verdict per DSV pointer until
    the next reallocation, and if reallocation fails, shaders still compile with `REVERSE_Z` while
    the targets stay standard.
