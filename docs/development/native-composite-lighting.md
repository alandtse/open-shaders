# Native Composite and direct lighting

Native Composite is an independently configured core feature. At the existing
pre-upscale hook it captures the completed raster scene and may add the current
Native Lighting diffuse and specular textures before publishing FinalSceneDX12.
The raster baseline remains in place, so this temporary additive policy can
double-count direct lighting. Intensity can be adjusted or the addition disabled.

Native Scene, Native Lighting, Native Composite and the runtime upscaler acquire
the same DX12 device for the engine's adapter. The scene and upscale queues remain
separate. Composite waits on the lighting producer fence and retains the lighting
resources until its own completion fence. FSR waits on that completion fence; its
existing DX12-to-DX11 bridge follows the upscale. None of these submissions alone
proves that lighting was visible in a presented frame.

Only validity value 1 is added. Missing, unsupported, mismatched, disabled or
unavailable lighting leaves the raster image unchanged. Alpha is preserved. The
legacy scene transfer is decoded and re-encoded with the existing 1.6 scene gamma;
linear values are added directly. Menu frames and configurations selecting ACEScg
fall back to raster pass-through until their color interpretation is established.
The current early shadow mask is not authoritative for lighting culling. VR uses
the existing rendering route and is outside this iteration.

Native Composite diagnostics distinguish capture attempts from submitted lighting
composites. Upscaling's `nativeLightingCompositeReturns` counts queued DX11 return
copies associated with a lighting composite, not GPU completion or visual proof.
