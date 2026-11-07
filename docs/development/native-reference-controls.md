# PR #9 reference controls: native DX12 port

Source: [comparison/pre-rtcomposer](https://github.com/edgy-the-hedgy/open-shaders/pull/9).
This ports controls into the **new** isolated native lighting, early RT shadow-mask
producer and native compositor. It deliberately does **not** restore
`RTDirectLighting`, `DirectionalTrace`, RTComposer tiers, or old GI/RR families.

## Native Lighting

The feature now exposes persisted live controls, adjustable in its UI and
`configure` DevBench action:

- Common direct strength, diffuse/specular strengths and independent emission
  strength. The user-tested emission default is **1.0x**, which may
  double-count emissive lighting already present in the raster scene.
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

- The user-tested independent shadow layer is **enabled by default** with
  contribution 2.0x and blend 0.70. A separate Native Lighting enable switch
  remains off, preserving ownership of standalone shadowing.
- Contribution amplifies weighted occlusion (0..4x); blend mixes the effect
  independently (0..1). The final-image composition option is enabled by
  default, affecting raster and any native direct contribution.
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

1. Temporarily set shadow contribution to 0 and verify the original raster
   image is unchanged, then restore the tested default 2.0x.
2. With the early shadow mask enabled, set contribution to 0.25x and radius 0:
   matched pixels should darken but invalid/out-of-depth pixels stay raster.
3. Increase radius to 2 or 3 px; inspect edges for mask bleeding and read
   DX12 Native Scene / Native Composite diagnostics.
4. Compare Direct normal mode 0/1/2 and stochastic sampling 0/1 without
   changing unrelated validity lab or RR policy.
5. Watch `busyDeferrals` and independent `shadowSubmissions` when varying
   control values; no stale resources may be substituted.

## User-tested defaults — November 7, 2026

These defaults were transcribed from three supplied Skyrim settings screenshots.
They apply to fresh configuration fields and **Restore Defaults**; existing
persisted user/scene settings remain authoritative until reset. UI buttons
that reset light/direct strengths or shadow reach/falloff now use the same
settings-struct defaults, rather than obsolete constants.

| Feature | Control | Default |
| --- | --- | --- |
| Native Lighting | Enable native lighting | Off |
| Native Lighting | Diffuse / Specular | Both on |
| Native Lighting | Direct strength | 2.00x |
| Native Lighting | Diffuse intensity / Specular intensity | 1.00x / 1.00x |
| Native Lighting | Prepared emission strength | 1.00x |
| Native Lighting | Normal influence | Full material normals (True PBR) |
| Native Lighting | Radius multiplier | 4.00x |
| Native Lighting | Falloff transition width / exponent | 0.10 / 1.00 |
| Native Lighting | Direct ray-occlusion weight | 1.00 |
| Native Lighting | Stochastic emitter sampling | Off |
| Native Lighting | Emitter radius scale | 1.00x (inactive while sampling off) |
| Native Lighting | All prepared cell lights | On |
| Native Lighting | Automatic lighting VRAM budget (DXGI) | On (0 MiB override) |
| Native Lighting | Early shadow-mask culling | On |
| Native Lighting | Publish validity debug / Compare culling | Both off |
| Native Composite | Enable Native Composite / Add native direct lighting | Both on |
| Native Composite | Lighting strength | 1.000 |
| Native Composite | Enable independent shadow layer | On |
| Native Composite | Shadow contribution | 2.00x |
| Native Composite | Shadow blend | 0.70 |
| Native Composite | Shadow softness | 2.0 pixels |
| Native Composite | Shadow receiver tolerance | 16.00 |
| Native Composite | Preview independent shadow mask | Off |
| Native Composite | Apply shadows to final combined lighting | On |
| DX12 Native Scene | Load prepared scene / Produce RT shadow mask | Both on |
| DX12 Native Scene | Shadow all prepared point lights | On |
| DX12 Native Scene | Stochastic shadow emitter sampling | On |
| DX12 Native Scene | Shadow emitter radius multiplier | 0.200 |
| DX12 Native Scene | Shadow light reach multiplier | 2.00x |
| DX12 Native Scene | Shadow fade range fraction | 1.000 |
| DX12 Native Scene | Shadow falloff curve | 0.60 |
| DX12 Native Scene | Fade shadows near light reach | On |
| DX12 Native Scene | Shadow normal influence | Full material normals |
| DX12 Native Scene | Automatic native frame VRAM budget (DXGI) | On (0 MiB override) |

The tested screenshot has Native Lighting disabled even though Native
Composite's **Add native direct lighting** checkbox is on. Native Composite
will therefore apply the independent shadows while the native direct
lighting contribution remains absent; that is deliberate.
