# PR #9 reference controls: native DX12 port

Source: [comparison/pre-rtcomposer](https://github.com/edgy-the-hedgy/open-shaders/pull/9).
This ports controls into the **new** isolated native lighting, early RT shadow-mask
producer and native compositor. It deliberately does **not** restore
`RTDirectLighting`, `DirectionalTrace`, RTComposer tiers, or old GI/RR families.

## Native Lighting

The feature now exposes persisted live controls, adjustable in its UI and
`configure` DevBench action:

- Common direct strength, diffuse/specular strengths and independent emission
  strength. Emission is **off by default** to avoid double-counting raster
  emissive contribution; reference emission used a different scene-composition
  architecture.
- Normal influence: legacy flat/no direct specular, geometric normals, or
  material (True PBR) normals. Geometric mode uses world-space triangle edges.
- Point-light radius multiplier, falloff transition width, exponent. Inverse
  square energy remains `I/(distance² + sourceRadius²)`, with a shaped soft
  cutoff only near the configured radius.
- All prepared lights or one selected indexed light; the 32-light trace cap
  is enforced **only** for all-lights mode. Unsupported lights remain skipped.
- Independent shadow visibility strength (0 unshadowed, 1 blocker-occluded),
  optional frame-varying disk-emitter visibility, and emitter-radius scale.
  Center-light intensity and attenuation are not perturbed by the disk ray.

Settings are snapshotted per native dispatch, and their changes increment
the revision used to reset the native FSR history.

## Early native shadow-mask producer

`DX12 Native Scene` now supplies the shadow trace's previously hardcoded
falloff and stochastic constants from persisted runtime controls. Shadow
selection (all / one light), normal mode, radius cutoff, transition width,
falloff exponent, fade-near-reach, and emitter-radius sampling remain owned
by the early-mask producer, independent of Native Lighting.

The mask remains **raw**: red = weighted visibility, green = sample validity,
blue = native primary ray distance, alpha = instance ID + 1. Shadow layer
strength and blend do not affect the raw mask or experimental validity-lab
sampling. The raw mask and the lighting's BRDF are separate signals.

## Native Composite shadow layer

An independent `ShadowLayer.dxil` DX12 compute pass consumes the current
frame's **raw** shadow mask and captured raster depth. It uses the same
inverse-projection/view distance reconstruction as Native Lighting to refuse
unmatched receivers; incompatible pixels retain raster. A conservative
five-tap, instance-/depth-gated spatial softening is available from 0 to
32 px. No cross-geometry blur or prior-frame history is used.

- Shadow strength/blend are **off by default** (reference default strength
  zero); enabling the early mask alone therefore preserves the prior image.
- Strength and blend both scale the mask's effect on raster light.
- Shadow preview shows only depth-matched mask pixels.
- The compositor waits on both producers and retains shadow/raster snapshots
  until the compositor GPU fence completes. It also emits a distinct
  `shadowSubmitted` flag; direct-light submissions are not conflated with
  shadow-layer submissions.
- The RT shadow layer modulates the raster scene's RGB as a coarse lighting
  approximation. Unlike the old RT-composer, it cannot independently subtract
  only the original raster direct-lighting lobe. This is an explicit
  limitation, not a pixel-perfect old pipeline replacement.
- DXC shader is packaged and deployed beside `AddLighting.dxil`.

## Intentionally not ported yet

The old **sparse direct/shadow trace resolution** selectors and temporal
shadow-history accumulation depend on matching native reconstruction/history
buffers that are absent from the clean pipeline. Those controls are not
exposed as no-op sliders. Likewise old RR resolution/mode controls belong to a
future independent RR implementation; this commit does not touch RR guide
buffers or add old indirect dispatchers.

## Verification

The source changes and constant layouts were checked statically. The owner
must run the Visual Studio Release build, shader packaging/deployment and
Skyrim runtime comparison. Important experiments:

1. With the new shadow layer at default strength 0, verify original raster
   and direct lighting remain unchanged.
2. Enable early shadow mask and set layer strength 0.25, radius 0:
   matched pixels should darken but invalid/out-of-depth pixels stay raster.
3. Increase radius to 2 or 3 px; inspect edges for mask bleeding and read
   DX12 Native Scene / Native Composite diagnostics.
4. Compare Direct normal mode 0/1/2 and stochastic sampling 0/1 without
   changing unrelated validity lab or RR policy.
5. Watch `busyDeferrals` and independent `shadowSubmissions` when varying
   control values; no stale resources may be substituted.
