# Shallow water surface restoration

Unified Water can restore a bounded surface layer over shallow water that remains
too transparent to show its material and reflections. This changes shading on
existing water geometry; it does not generate missing water tiles or repair cache
coverage. Restoration defaults to full strength `1`, matching CSX. Existing
explicit values, including zero strength, are preserved. No separate enable
checkbox is added.

## Controls and devbench

The ordinary Unified Water panel exposes **Edge Fade Depth**, **Shallow Fallback
Max Distance** and **Shallow Fallback Strength**. The existing **Debug** section
contains the five technical sliders and **Use Legacy Open Shaders Depth
Behaviour**. That diagnostic checkbox keeps the CSX key
`UseOpenShadersDepthBehaviour` and default `false`. Enabling it restores the
existing blend by suppressing the additional shallow layer; all selected values
are retained. The normal controls are disabled with a visible explanation while
the legacy option is on. Tint and optimized-mesh controls are outside this change.

Use the existing `openshaders.feature` tool with `shortName: "UnifiedWater"`.
`action: "get"` returns the selected controls. `action: "set"` accepts a partial
settings object, for example:

```json
{
  "action": "set",
  "shortName": "UnifiedWater",
  "settings": { "ShallowFallbackStrength": 0.5 }
}
```

Use `action: "diagnostics"` to inspect `shallowSurfaceReady`,
`legacyDepthBehaviour`, `selectedShallowStrength`, and `effectiveShallowStrength`. Missing or disabled
unified-water resources suppress the effective strength without changing the
saved selection. `action: "reset"` restores all nine defaults. Saving uses the
usual `openshaders.settings` operation; a live feature change does not save it.

| Setting | Default | Range | Effect |
| --- | ---: | ---: | --- |
| `ShallowFallbackStrength` | 1 | 0–1 | Strength of the extra surface layer |
| `DeepConnectionProbeReachUnits` | 768 | 0–768 | World-space reach of the two depth probes; zero disables their suppression |
| `DeepContextDepthUnits` | 192 | 32–384 | Depth that establishes a connection to deep water |
| `ShoreContactMinFadePixels` | 2 | 0–4 | Minimum adaptive shore fade width |
| `ShoreDepthBlendRangeUnits` | 10 | 0–20 | Shore contact fade depth |
| `ShallowSurfaceDepthRangeUnits` | 256 | 16–256 | Column depth over which restoration falls to zero |
| `ShallowFallbackMaxDistance` | 26000 | 0–65536 | Camera-distance limit; zero disables restoration |
| `DeepContextTransitionUnits` | 128 | 4–128 | Soft transition into deep-water suppression |
| `UseOpenShadersDepthBehaviour` | false | false/true | Debug-only legacy blend, suppressing restoration when true |

The UI and JSON paths protect the selected controls with a recursive mutex so
menu reset callbacks can safely reenter serialization. A separate, short-held
mutex publishes complete sanitized snapshots for rendering. Both UI sections
publish before returning; JSON loads and per-setting resets use the same setter.
The six Debug controls remain excluded from scene overrides.

The CPU publishes a 32-byte shader block with effective strength. Both C++ and
HLSL place it after Procedural Aurora and before optional Snow Cover in feature
buffer `b6`.
The raw native refraction weight is captured before the user water-muddiness
multiplier, so changing that multiplier does not create restoration eligibility.

## Depth and cost boundaries

Only above-water depth permutations participate. LOD and underwater shaders are
excluded, as is flat vertex-alpha depth. VR vertex-alpha depth uses scene depth
and the interpolated water surface position. The shader reconstructs native depth
with the matching eye's inverse view-projection, rejects foreground geometry and
invalid homogeneous coordinates, and excludes exact clear depth. With Horizon
Fix enabled, folded backdrop depth is also excluded. Representable far geometry
is retained when Horizon Fix is disabled.

An eligible pixel reads one primary depth sample and at most two additional point
samples, using the already-bound depth texture at `t7`. The midpoint must provide
valid evidence before a distant endpoint can suppress restoration. Probes stay
inside the active render extent and current eye; clamping cannot turn a missing
probe into evidence. The radius is capped at 512 pixels and the configured world
reach. Large projected radii lose authority, with an additional near-field fade
in VR. The surface blend weight is bounded at one half before deep-water and
shadow suppression. The existing material, ENB, lighting and fog paths are kept.

There is no new texture, depth copy, dispatch or rendering pass. Zero effective
strength skips the added depth loads; ordinary shader arithmetic and settings
publication still exist. These are source-level cost bounds, not measured GPU
timings.

## Validation

`tests/cpp/test_unified_water_shallow.cpp` tests the production settings type,
bounds, non-finite inputs, enabled default, legacy suppression and preservation of the selected strength
when runtime resources are unavailable. It is part of `cpp_tests`, run by the
**Run C++ unit tests** CI step.

The four `TestWaterShallowSurface*.hlsl` files test production helpers under
standard/reversed depth and flat/VR. They cover clear and Horizon Fix rejection,
far precision, native reconstruction, both-eye boundaries, distinct probes,
mandatory midpoint evidence, shoreline weighting and strength gating. They are
automatically discovered by `shader_tests`, run by the **Run shader unit tests**
CI step. Shader unit execution uses DXC/D3D12; production permutation validation
must also pass FXC/DX11.

Before considering runtime validation complete, record the following on VR and
one flat runtime, comparing strength `0` and default strength `1` at the same
camera, resolution, upscaler and lighting:

1. Shallow stream, deep connected water, steep bank, opposite bank, exposed shore
   and foreground object: restoration must remain on the water and fade at contact.
2. Standard and reversed depth, Horizon Fix on/off, far clear sky, folded horizon,
   both eyes and the stereo seam: no false far surface or cross-eye evidence.
3. Native and reduced/dynamic resolution, screen edges, near-field VR movement,
   volumetric shadows, vertex-color and non-vertex-color water: no eye mismatch or
   unstable shadow-driven layer disappearance.
4. ENB water on/off, fog, and water muddiness changes: preserve existing controls
   and keep eligibility independent of the user muddiness multiplier.
5. Resource unavailability or flowmap reset followed by recovery: diagnostics must
   show effective zero during failure and restore the selected strength afterward;
   saving during failure must retain the selection.
6. Zero strength and the Legacy diagnostic option: compare against the base shader
   and confirm no visible restoration change. Turn the Legacy option off and verify
   the saved strength returns. Measure enabled cost at default strength `1` with
   the existing water draw instrumentation.

Use curated devbench replays for repeatable motion where applicable. The outdoor
`GuardianStonesToWhiterun` recording is flat-only; select a reproducible VR water
scene separately and document it. Automated checks alone do not establish these
in-game results.
