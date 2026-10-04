# Runtime feature toggles

## Current source changes

Feature availability (`loaded`) and the live rendering switch (`IsEnabled()`) are
separate. Installed, supported features load their settings, install hooks, and
retain their compiled support when their saved switch is off. Owned effect
textures and buffers are allocated when enabled and released when disabled. The existing
`Disable at Boot` JSON key is retained for settings compatibility; the menu now
applies that preference immediately. Devbench `feature(action="toggle")` changes
the live switch, and `feature(action="boot")` persists and applies it.
LLF, True PBR, Unified Water and Horizon Fix integration are exceptions: their
single feature toggles are latched before hook installation and require a restart
in both directions. Shader availability stays fixed across these toggles.

The shared feature buffer ends with a 128-bit mask. C++ and HLSL share feature
indices through `Common/RuntimeFeatureList.hlsli`. Render dispatches and shader
branches consult the live switch. Enabling a feature requests its render-thread
history reset. Availability remains stable when a switch changes, so it does
not change the shader-cache feature manifest or mark a pending cache rebuild.

The agreed design retains feature `#if` guards. Installed, supported features
are compiled into the precomputed cache independently of their saved live switch.
Runtime booleans select execution inside that fixed support. Installation,
VR/stage, and material-layout guards remain compile-time choices; removing those
guards is not part of this migration. IBL's Dynamic Cubemaps availability guard
is retained, with separate live checks for both features.

SSR no longer emits `ENABLESSR`. SSGI no longer emits GI, specular GI, temporal
denoiser, adaptive sampling, or resolution definitions. Those options are read
from its constant buffer. AO-only use releases GI-only radiance and lighting-history
textures and their views; re-enabling GI recreates them on the render thread.
Settings changes reset SSGI history instead of clearing its shader cache.
Grass carries both vertex paths and both lighting paths. An optimized draw marks
its per-draw buffer so a vanilla fallback can select the standard vertex path
even while the global optimization switch is on.

Deploying changed shader source and the new constant-buffer layout requires an
initial shader-cache refresh. The goal is to avoid additional rebuilds caused by
subsequent feature toggles. Changing the installed package, build capabilities,
shader source, or material/edition permutations can still require compilation.

## Additional toggle coverage

-   Grass Collision skips collection and GPU updates while off, and clears its
    deformation/velocity history when enabled.
-   Wind resets grass/tree simulation history on enabling and scene transitions.
-   Cloud Shadows skips native sky overrides; cloud relighting and procedural sun
    skip its texture reads while off. Enabling initializes new cloud captures.
-   Skylighting skips its native probe-occlusion pass while off, preserves the
    engine precipitation pass, and returns neutral diffuse/specular visibility.
-   Interior Sun's worldspace, shadow-culling and two-sided-render hooks now obey
    the main switch. Terrain Helper's displacement check also obeys its switch.
-   Terrain Blending restores the original engine terrain-fade preference while off.
-   Inverse Square Lighting's gameplay luminance hook selects the engine function
    while off.
-   Volumetric Lighting prepares VR support regardless of initial preferences,
    stops native volumetric rendering while off, and reapplies cell settings on
    enabling. Its exterior/interior enable options no longer carry restart metadata.
-   Exponential Height Fog drops temporal history when enabled again.
-   Light Limit Fix's toggle saves the next-launch state. Starting with it off skips
    LLF hook installation, the shadow scheduler, resource initialization and render
    maintenance. Lighting, effects, water, grass and particles use the compiled
    vanilla fallback paths. Changing the toggle leaves the current session intact.
-   Pending feature resets are drained before reflection/early-prepass shared data.
-   CPU feature-mask packing uses the explicit shared feature indices.
-   Feature tooltips and devbench `runtimeToggleNote` explain retained native support.

## Background compilation scheduling

Background mode skips bulk engine and PBR permutation requests. Shader lookups
from rendering request the variations they need, using the existing disk cache
when valid. Foreground startup retains full precomputation.

Switching to background mode discards queued bulk permutations and adjusts their
progress totals. A rendering request promotes an already queued bulk permutation
so it is retained. Queue insertion and mode changes use the same mutex, preventing
concurrent startup enumeration from adding more bulk work after the switch.
Already dispatched jobs finish; standalone feature initialization remains queued.
The boot preference, Skip Compilation key, and devbench backgroundCompile action
all use the same setter. The current post-sync source has not been rebuilt or
verified in game.

## Resource lifetime

The main feature switch queues a render-thread transition. At the next transition
boundary, disabling stops the contribution, removes D3D11 bindings to its owned
textures/buffers, and releases those objects. Starting disabled skips their setup;
enabling recreates them and resets histories. No timed retention period is used.
GPU work already submitted and independent engine owners can delay the driver's
actual reclamation after the feature releases its references.

Compiled shader programs, fixed feature defines, settings, and CPU bookkeeping
needed to resume remain available. Re-enabling does not invalidate the shader cache.
First use still needs a valid shader from the existing cache/loading path.

| Feature                                             | Owned resources released by the main switch                                                                                                                                                                      |
| --------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Dynamic Cubemaps                                    | Capture, inference, environment and BC6H textures, mip views, buffers, sampler and fallback cubemap.                                                                                                             |
| Image Based Lighting                                | Dynamic diffuse targets and static environment textures.                                                                                                                                                         |
| Screen Space GI                                     | Depth/normal/radiance, GI/AO/history textures, noise, constants and samplers. AO-only operation retains shared AO/depth/normal/history resources and releases the eight GI-only textures and radiance mip views. |
| Skylighting                                         | Probe volumes, copied shadow texture, update resources and sampler.                                                                                                                                              |
| Light Limit Fix                                     | Restart-required toggle; no LLF buffers or shadow backend are initialized when starting disabled.                                                                                                                |
| Grass Optimizations                                 | GPU buckets, indirect arguments, Hi-Z, constants and feature-held LOD mesh references. CPU captures stay available for rebuilding.                                                                               |
| Grass Collision / Wind                              | Simulation fields, buffers and samplers.                                                                                                                                                                         |
| Cloud / terrain / screen-space / volumetric shadows | Their owned maps, masks, copies, buffers and samplers. Engine shadow resources are not owned by these effects.                                                                                                   |
| Terrain Blending                                    | Private depth targets and state; original engine depth views are restored.                                                                                                                                       |
| Exponential Height Fog                              | Volume/history/depth textures, constants, samplers and borrowed shadow-map reference.                                                                                                                            |
| Subsurface Scattering                               | Intermediate targets and constants.                                                                                                                                                                              |
| Skin / Hair / Water Effects                         | Skin detail/material texture references, tangent-shift texture, caustics texture and Skin constants.                                                                                                             |
| Linear Lighting / CS Utility / Volumetric Lighting  | Owned constant buffers; engine-owned lighting targets remain engine-managed.                                                                                                                                     |
| HDR Display                                         | Private scene/output/UI/capture targets and constants; upgraded engine targets are restored. DX12's separate UI surface belongs to its active swapchain.                                                         |
| Upscaling                                           | D3D11 effect intermediates, constants and states when no persistent performance-mode or DX12 backend needs them; method changes release SDK resources.                                                           |
| Effects11                                           | Common, custom, effect, crop and volumetric textures/buffers. Compiled FX objects and their internal shader constants remain.                                                                                    |
| Post Processing                                     | Pipeline targets and all child effect textures/buffers. Individual child switches also release their own allocations; aperture assets follow their users.                                                        |
| VR                                                  | Classification/history, stereo-copy and near-clip probe/readback resources.                                                                                                                                      |
| Screenshot                                          | Cached preview texture; already requested screenshot writes finish and release their staging resources normally.                                                                                                 |

Allocation errors reaching the shared lifecycle disable the effect and release
partial resources. The post-processing child lifecycle has its own failure
fallback. These paths have not been exercised in game.

The [allocation budget](runtime-feature-memory-budget.md) describes enabled
resource payloads, not memory permanently spent merely by compiling support.

## Persistent engine support and remaining limits

-   Light Limit Fix's native shadow scheduler suppresses the engine shadow-mask
    pass. Its main toggle therefore requires a restart instead of uninstalling
    those patches live. The disabled startup path keeps the engine mask and native
    point-light constants; shared shaders branch to that path using the existing
    per-frame buffer. Compiled feature availability and cache keys do not change.
    Menu controls show the next-launch selection and pending restart. Devbench
    exposes `toggleRequiresRestart`, `requestedEnabled`, and `restartPending`;
    `enabled` continues to report the current session. For restart-required features,
    `toggle` also saves the preference, as `boot` does. LLF diagnostics expose `backendEnabled`.
-   Unified Water's restart-required switch skips water hooks, native patches,
    replacement meshes, flowmaps and cache setup when starting disabled. Both native
    and unified flowmap coordinate paths stay compiled into the water shaders.
-   True PBR's restart-required switch skips material hooks, configuration resource
    setup, default land replacement and rendering maintenance when starting disabled.
    Material-format shader permutations remain available in the precomputed cache.
-   Horizon Fix's restart-required switch controls only Open Shaders integration.
    The companion SKSE plugin loads independently; the current integration has only
    a far-water-distance getter and cannot stop its geometry generation or unload it.
    Starting with integration disabled skips that getter and uses normal far-plane
    water/fog shader behavior. Installing or removing the companion plugin still
    changes feature availability; toggling integration with it installed does not.
-   Upscaling performance mode, swapchain/frame-generation backends, shadow atlas
    dimensions, and other allocation-layout settings retain their existing
    restart requirements. The resource lifecycle does not reconstruct those backends at runtime.
-   A missing package, unsupported runtime, or failed initialization remains
    unavailable; a boolean cannot provide missing resources or platform support.

## Toggle diagnostics

`CommunityShaders.log` records `[ResourceLifetime]` at Info level for each
resource transition. Each transition has an `id`, feature, operation and phase.
No debug-layer installation or logging-level change is needed.

-   `allocated_observed` / `allocated_payload_bytes`: named D3D11 resources created
    inside the setup scope. Views do not count as extra texture storage.
-   `release_observed` / `release_payload_bytes`: unique resources encountered by
    the cleanup helper, including allocations first observed during release.
-   `destroyed` / `destroyed_payload_bytes`: resources whose attached lifetime
    notification has been released by D3D11. The notification never holds a
    reference to the resource itself. `pending` counts observed releases whose
    destruction has not yet been notified.
-   `known_before_survivors`: resources recorded in earlier scopes that were alive
    before this transition and remain alive afterward. A survivor with
    `release_observed=false` points to an allocation the cleanup did not visit.
-   `dxgi_local_bytes`, `dxgi_nonlocal_bytes`, and `dxgi_local_delta_bytes`: process
    usage snapshots for adapter node 0. A usage value of -1 means unavailable;
    a delta with an unavailable baseline is not evidence of zero memory change.
-   `observation_failures` and `unknown_release_sizes` make incomplete tracking
    visible. Byte counts are descriptor payload estimates, excluding alignment,
    shader/state objects and backend allocations outside this D3D11 tracker.

Delayed reports run after 1 and 120 rendered frames. Outstanding observed objects
continue reporting every 600 frames; up to 16 pending names and 16 known survivor
names are listed per sample. Monitoring is capped at 256 transitions, with a
warning if the oldest sample is dropped. Re-enabling does not add new allocations
to an older transition's survivor snapshot.

The notification uses D3D11's
[private-interface lifetime contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicechild-setprivatedatainterface).
Object destruction and descriptor payloads do not prove that the driver returned
that exact amount of resident VRAM. DXGI samples provide a separate usage signal;
other scene allocations can change it during the same interval. This is scoped
resource instrumentation, not an exhaustive device-wide leak detector.

`[GrassToggle]` records the first normal and optimized draw paths after a toggle,
including normal instance-group counts, failed group-index uploads, presence of
the fade buffer, and optimized bucket capacity. Normal fade uploads remain enabled;
optimized draws restore native input bindings afterward.

For a useful reproduction: load a grassy scene, toggle Grass Optimizations off,
leave it off for at least 120 rendered frames, then toggle it on and wait again.
Supply that run's log for review. Logs from other builds are not used to infer
which implementation produced a reported symptom.

## Review status

The C++ Release build and local CSVO deployment succeeded on 2026-10-02,
including the restart-required feature toggles. Deployment hashes were verified
and both Streamline directories preserved. These results precede the 2026-10-04
integration of `alandtse/open-shaders/dev` at `fce2e3052`; the current source has
not been rebuilt, shader-validated, or verified in game on VR and a flat runtime.

A reported blurry image after disabling Post Processing remains unresolved.
Resource destruction was observed in previously supplied logs for SSGI, Effects11,
Wind and Exponential Height Fog. Those observations do not verify the current
post-sync build or every transition. The work remains a draft.

### SSGI AO/GI allocation transitions

`EnableGI` drives GI texture allocation, including changes from presets and settings
loads. AO-only startup skips these textures. Switching to AO releases radiance,
temporary radiance, both Y/CoCg history pairs, both specular history textures, and
the radiance mip UAVs. Switching GI on recreates them, clears the new lighting
history, and resets temporal accumulation without replacing shared AO resources.
Both shader paths remain cached. Allocation failure releases partial GI resources
and falls back to AO.

`[ResourceLifetime] feature=ScreenSpaceGI/GI` reports these enable/disable
transitions and delayed destruction separately from the shared AO pool. These
transitions were included in the earlier build and deployment; verification of
the current post-sync source remains outstanding.
