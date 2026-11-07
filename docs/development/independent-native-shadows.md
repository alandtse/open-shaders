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

## Same-frame recovery and shadow admission (November 2026)

The early shadow tracer may be invoked before the current-frame main-camera
snapshot is available. `DX12NativeScene::OnOpaqueRasterReady` now retries a
shadow dispatch after raster/depth capture **only if** no mask for that exact
frame has already been offered. The retry uses `DispatchShadow`, which keeps
the established camera-origin validation, interior-cell scope, prepared-scene
and material/light eligibility, DXGI budget and producer-fence safety.
No previous-frame mask is substituted. Counters
`shadowOpaqueRetryAttempts` and `shadowOpaqueRetryRecovered` distinguish
retries from successful late recovery.

Native Composite's independent shadow admission is now evaluated gate by gate
instead of a silent all-or-nothing Boolean. The **Independent shadow admission**
line shows the first failing condition or successful pass submission.
Separate **Shadow producer** status exposes upstream trace failures.
`GetDiagnostics()` exports `shadowAdmissionStatus`, `shadowProducerStatus`,
`shadowProducerStage`, `shadowProducer`, and both opaque-retry counters.

If `ShadowLayer.dxil` is missing, unreadable, invalid-sized or fails DX12
root-signature/PSO initialization, its specific failure is reported; it does
not silently disappear into the raster-only path. Shadow and lighting admission
remain independent. In particular, disabled Native Lighting is not a reason
to reject a valid current-frame shadow mask.

Owner validation: With Native Lighting disabled and independent shadows
enabled, check raw shadow submissions, late retry recoveries and
`shadowSubmissions`. If raw submissions advance but `shadowSubmissions`
does not, read the new admission reason rather than weakening frame/fence
identity checks.

## Physical raster depth allocation vs active render size

The DX11 raster capture stores the *physical* allocation size of its opaque
inputs and copied depth. The independent shadow mask and Native Composite are
indexed by the *active* dynamic render size. This means a larger allocation
must not be rejected solely because it has more unused pixels.

The shadow compositor now requires:

- Current-frame shadow mask **exactly** matching active composite width/height.
- Current-frame raster depth allocation **at least as large** as active width
  and height. The shadow shader samples only active pixel coordinates.
- The same source depth format, valid camera and producer fence gates as
  before. Smaller raster allocations still reject with an explicit reason.

This is a dimensions/admission change, **not** an adjustment to scene-vs-raster
depth values or depth reconstruction. If the admission result changes to
`Independent shadow layer submitted` but rendered shadows remain absent,
investigate the per-pixel receiver-distance rejection separately, using
depth/error instrumentation rather than loosening its tolerance by default.
