# Native composite and upscale: source audit

This document describes the existing route, not an implemented native composite.
The references below refer to the source at the time of the audit; runtime behavior
and image equivalence require in-game validation.

## Frame order and capture boundary

`Upscaling::Main_PostProcessing::thunk` calls `PostProcessing::DrawBeforeUpscaling`,
then the neural-rendering prepass and its pre-upscale diagnostic capture. The
post-processing pass starts with `kMAIN` and, when it processes effects, copies
its result back to `kMAIN`. Neural rendering is also passed `kMAIN` as its color
resource and can write its resolved output back to that resource. The subsequent
optional frame-generation preparation copies motion and draws depth into its own
shared resources; it does not write `kMAIN`.

`PerformUpscaling` calls `Upscale`, whose encoder prepares masks and, when needed,
typed depth before FSR reads `kMAIN`. The candidate final-scene capture is therefore
inside `Upscale`, immediately before the FSR route consumes its color input, after
the pre-upscale hooks and encoder. For VR the existing FSR path splits `kMAIN` into
per-eye intermediate inputs before dispatch; a full-scene composite cannot simply
substitute for one of those per-eye resources. Foveated rendering has its own
preprocessing and region dispatch and must be handled explicitly.

After `Upscale`, `PerformUpscaling` runs `UpscaleDepth`, locks dynamic resolution,
and updates camera data. The thunk then captures the upscaled image, performs
applicable sharpening, composites rain, and invokes the engine's post-processing.
HDR framebuffer redirection surrounds the engine call. This confirms that the
post-ISHDR framebuffer is a later resource, not the FSR input.

`kMAIN`'s actual format and active dimensions must be queried per resource/frame.
Its transfer interpretation is configuration-dependent: pre-upscale post-processing
uses different copy gamma for menu/linear-lighting versus legacy rendering. A new
contract must describe the actual encoding rather than infer it from DXGI format.

## Current input and backend ownership

`GetEncodeInputs` reads the temporal-AA mask, forward normals, motion vectors, and
main depth on DX11. `Upscale` generates reactive and transparency masks and, for
the runtime FSR route, typed depth on DX11. Flat FSR receives `kMAIN`, the main
motion texture and these generated resources; VR receives per-eye intermediates.
The encoder derives the reactive mask from the two TAA-mask channels and takes the
transparency/composition mask from the forward normals/water buffer. Its typed depth
is copied from main depth; its VR outputs are indexed per eye.
The existing dispatch supplies jitter, motion scale, frame delta, camera parameters,
render/display sizes and a menu-history reset. These values must remain associated
with the same frame when moved into a native input bundle.

`FidelityFX::EnsureRuntimeUpscalerInterop` can create the existing swap-chain helper's
DX12 device and direct queue on the DX11 adapter without enabling frame generation.
Its FidelityFX contexts and wrapped resources use that device. Native Scene owns a
separate backend, so its resources cannot be assumed compatible with this route.

`DispatchRuntimeUpscalerSingle` copies five DX11 inputs into shared textures, signals
a shared fence on DX11 and waits on the DX12 queue. Its command list transitions the
shared inputs, dispatches the runtime FSR provider, transitions back, and signals a
completion fence. The DX11 context waits on that fence before copying the separate
display-resolution output into `kMAIN`, or into the PerfMode test texture when selected.
The host FSR fallback remains DX11. These are source-level observations, not proof
of successful GPU execution or frame generation.

## Work still required

Native Composite needs its own frame-scoped output and producer/consumer lifetime
contract on the runtime upscaler's device. The FSR dispatch must accept that output
directly, keep all guidance inputs frame-coherent, and preserve the existing provider,
fallback, VR, foveated, frame-generation, and output-bridge behavior. In particular,
resource reuse must be guarded by both producer and downstream GPU completion, and
failure after a GPU submission cannot safely fall through to a second dispatch
without establishing a complete-frame fallback. No such implementation or runtime
verification is claimed by this audit.
