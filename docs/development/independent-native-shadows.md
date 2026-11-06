# Independent native RT shadows

This design separates **shadow visibility production**, **direct-light
occlusion**, and **shadow contribution to the final image**.

## Ownership

| Owner | Role |
| --- | --- |
| DX12 Native Scene | Prepares the TLAS and independently dispatches a current-frame raw shadow visibility mask |
| Native Lighting | Computes native direct diffuse/specular with its own physically relevant visibility testing; can be disabled independently |
| Native Composite | Owns and applies an optional independent RT shadow layer, independently of whether Native Lighting supplied any radiance |

Enabling **Native Composite → Independent RT shadows → Enable independent
shadow layer** requests the prepared scene and the early shadow producer.
The Native Lighting enable toggle is not changed. The scene must still have
prepared geometry and supported lights.

## Contribution controls

- **Enable independent shadow layer:** off by default, regardless of Native Lighting.
- **Shadow contribution (0..4):** amplifies weighted occlusion. At 1 it matches
  the previous full-strength mask, at 2-4 it makes partly shadowed receivers
  substantially darker. Fully occluded pixels cannot be darker than black.
- **Shadow blend (0..1):** separately mixes the shadowed image with the original.
- **Shadow softness (0..32 px), receiver tolerance (0.1..64 units):** depth-
  and instance-constrained spatial filtering, not a temporal history filter.
- **Preview:** displays the mask without changing its raw DX12 producer signal.
- **Apply shadows to final combined lighting:** off = raster shadow first,
  then add native direct; on = compose direct first, then shadow the combined
  image. This is an intentionally artistic (potentially double-occluding)
  option that produces stronger shadows even with bright native illumination.

The compositor's pixel shading uses:

```text
occlusion = saturate((1 - weightedVisibility) * contribution)
outputVisibility = 1 - occlusion * blend
```

This replaces the old `saturate(strength * blend)` limit and permits shadow
contribution above 1 without negative RGB output. Both modes preserve current-
frame identity checks and producer/consumer fences.

## Migration and operation

The shadow layer's visual controls have moved from **DX12 Native Scene** to
**Native Composite**. DX12 Native Scene retains *only* mask producer settings:
prepared-light selection, emitter sampling, reach/falloff and normals.

The old `DX12 Native Scene.setShadowLayer` developer command still accepts
its previous tracing arguments. Its visual parameters are forwarded to the
new `NativeComposite.shadowLayer` command. Settings saved in the old
DX12 Native Scene fields are not silently interpreted as new compositor
settings; configure the new Shadow Layer controls once and save normally.
All settings persist with Native Composite afterward.

The **Native Lighting → Direct ray-occlusion weight** control is deliberately
still available for tuning the *native BRDF*. It is not the independent shadow
contribution slider.

## Suggested owner runtime verification

1. Disable Native Lighting, enable Native Composite and the independent RT
   shadow layer. Confirm it automatically enables the raw producer and that
   `shadowSubmissions` rises while `lightingSubmissions` remains unchanged.
2. At contribution 1, blend 1, compare against the former shadow layer.
   Increase contribution to 2 and 4 and inspect partial penumbrae and light
   corners for saturation.
3. Enable Native Lighting, then switch **Apply shadows to final combined
   lighting** on and off. Verify the former mode affects raster only, while
   the latter also darkens native direct illumination.
4. Turn shadow layer off while retaining Native Lighting. Verify the light
   signal continues as before and the independently produced raw mask may
   remain available for validity diagnostics.
5. Check consumer fences, frame tags, `busyDeferrals`, and VRAM budgeting.
   No previous-frame resources may be substituted.

Build and Skyrim runtime execution are left to the owner.
