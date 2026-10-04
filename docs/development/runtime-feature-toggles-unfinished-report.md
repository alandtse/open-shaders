# Runtime feature toggle migration: unfinished work

Updated: 2026-10-04

Updated after the fixed-define design was accepted and additional runtime gates
were implemented. The shader cache is precomputed; shader prewarming is not an
outstanding task. See [current changes](runtime-feature-toggles.md#additional-toggle-coverage).

## Status

**This is a draft, with runtime verification incomplete.** The C++ Release
build and local deployment succeeded on 2026-10-02, including LLF, True PBR,
Unified Water and Horizon Fix restart toggles. The subsequent 2026-10-04 sync
with `alandtse/open-shaders/dev` at `fce2e3052` has not been rebuilt or checked
in game on VR and a flat runtime. Shader validation has not been requested.

**Known reported issue:** disabling Post Processing left the image blurry.
That report remains unresolved. Previously supplied resource logs showed SSGI
and several other features releasing observed allocations, but do not establish
correctness of the current post-sync source.

This report distinguishes missing implementation from implementation that was
written but has not been checked by compilation or in-game use. A remaining item
below is not a claim that it is impossible to implement.

## 1. Implementation I have not completed

| Area                                     | What the draft does                                                                                                                                                                                                             | What is still missing                                                                                                                                                                                    |
| ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Fixed feature support and live execution | Retains feature availability guards and adds runtime gates inside the compiled support.                                                                                                                                         | Keeping `#if` guards is now the agreed design, not unfinished conversion work. Full behavioral coverage still needs review and runtime verification.                                                     |
| Unified Water                            | Restart-required toggle skips hooks, patches, meshes and flowmaps on disabled startup. Native and unified shader coordinate paths stay compiled.                                                                                | Build and in-game verification on flat and VR.                                                                                                                                                           |
| Light Limit Fix                          | One restart-required toggle. Disabled startup skips LLF hooks, scheduler, allocations and render maintenance; compiled shaders select native lighting and shadow paths.                                                         | Build and in-game verification of enabled and disabled startup on flat and VR. Live backend reversal is no longer a requested behavior.                                                                  |
| True PBR                                 | Restart-required toggle skips material conversion hooks and resources on disabled startup; existing shader permutations remain compiled.                                                                                        | Build and in-game verification on flat and VR.                                                                                                                                                           |
| Horizon Fix                              | Restart-required integration toggle retains shader availability while the companion plugin remains installed.                                                                                                                   | The current integration cannot disable the companion plugin's own work. Runtime verification remains outstanding.                                                                                        |
| Upscaling and graphics backends          | Adds live gating while retaining startup support; performance mode retains its required upscaler.                                                                                                                               | A complete effect-off path for performance mode is not implemented. Changing backend selection or render-target layout live is separate from the agreed feature-toggle task.                             |
| Shadow backend configuration             | Retains the configured scheduler and atlas infrastructure.                                                                                                                                                                      | Live reconstruction of atlas dimensions and native light configuration is separate from feature toggling.                                                                                                |
| Resource lifetime                        | Delays owned effect textures/buffers until enabled, unbinds and releases them on the render-thread disable transition, and recreates/reset histories on enabling. Postprocessing child switches also control their allocations. | First-enable, repeated toggles and allocation failure have not been exercised in game. LLF native support, water/material replacements and active graphics backends retain the resources they still use. |
| User-facing status                       | Feature tooltips now describe runtime effects and retained support. Devbench exposes `enabled` and `runtimeToggleNote`; UW, LLF, PBR and Upscaling provide specific notes.                                                      | These notes describe existing limitations; they do not implement full native-backend reversal.                                                                                                           |

### Source evidence for the engine limitations

-   [UnifiedWater.cpp](../../src/Features/UnifiedWater.cpp):
    `DisableVanillaWaterLOD()` patches native paths; mesh attachment replaces water
    geometry. The current implementation describes those patches as irreversible.
-   [ShadowEngineHooks.cpp](../../src/Features/LightLimitFix/ShadowEngineHooks.cpp):
    the shadow-mask interception suppresses the vanilla pass, and the scheduler
    documents incompatible engine state when reverting to vanilla traversal.
-   [Upscaling.h](../../src/Features/Upscaling.h): restart-field metadata remains for
    frame generation, rendering resolution and backend-related preferences; some
    fields are conditional on the active configuration.
-   [ShadowCasterManager.h](../../src/Features/LightLimitFix/ShadowCasterManager.h):
    restart-field metadata remains for shadow atlas and native light handling.
-   [Water.hlsl](../../package/Shaders/Water.hlsl),
    [Lighting.hlsl](../../package/Shaders/Lighting.hlsl), and
    [DeferredCompositeCS.hlsl](../../package/Shaders/DeferredCompositeCS.hlsl):
    feature-named compile guards remain in the draft.

The remaining live switches retain their native support; LLF instead requires a restart. The new resource
lifecycle releases effect-owned allocations; reversing installed engine replacements
is outside that lifecycle. See the [resource table](runtime-feature-toggles.md#resource-lifetime).

## 2. Code written, but not established as working

The draft includes:

-   Separate availability and runtime-enabled state, a shared CPU/HLSL feature
    mask, saved live toggles, and render-thread transition callbacks.
-   Runtime SSR control replacing the `ENABLESSR` option.
-   Runtime SSGI controls replacing GI, specular GI, temporal denoiser, adaptive
    sampling and resolution options; GI-only resources now follow the live AO/GI selection.
-   Combined standard/optimized grass vertex paths and grass lighting/fallback
    pixel paths, including a per-draw selection marker.
-   Numerous shader fallback and CPU dispatch changes for lighting, shadows,
    materials, terrain, wetness and HDR.
-   Main-switch resource lifecycles across the effect implementations, shared D3D11
    unbinding, retained shader programs and settings, and postprocessing child cleanup.
-   Background-mode scheduling that drops queued bulk permutations while retaining
    rendering-demand requests; the Grass Collision helper declaration fix.

These are source edits, not verified results. In particular, I have not checked:

| Review area                | What still needs to be established                                                                                                         |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| C++ and HLSL compatibility | Compilation, CPU/HLSL buffer agreement, shader input/output signatures, register usage, and valid permutations for each supported runtime. |
| Grass                      | Correct standard versus optimized draw routing, depth/alpha passes, resource-failure fallbacks, and both VR eyes.                          |
| SSGI                       | Correct AO-only/GI/specular transitions, history resets, resolution changes, disabled-resource accesses, and stereo reprojection behavior. |
| Feature interactions       | Correct off-state lighting and resource bindings when dependent features are toggled in different combinations.                            |
| Lifecycle                  | Starting disabled, enabling for the first time, repeated toggles, scene/loading transitions, and initialization failures.                  |
| HDR and upscaling          | Correct output color space, UI composition, history and render-target behavior through transitions.                                        |
| Cache stability            | Actual evidence that live toggles leave the shader-cache keys/manifest unchanged and do not request new compilation.                       |

These review risks are additional to the reported Post Processing blur above.
No in-game validation of the current post-sync source has been performed.

## 3. Resource costs I have not measured

The [resource notes](runtime-feature-toggles.md#resource-lifetime) list what now
releases and what remains required. The [allocation budget](runtime-feature-memory-budget.md)
quantifies enabled texture/buffer payloads from source. It is not a measured VRAM
saving from the new cleanup paths.

I have not measured:

-   Additional VRAM or system memory, including VR and resolution scaling.
-   Startup time and initial shader compilation cost.
-   GPU/CPU overhead from larger shaders, runtime branches, and persistent native
    maintenance while a feature is disabled.
-   Behavior under allocation failure or memory pressure.

Resident VRAM savings and toggle latency still require runtime measurement.

## 4. Validation and delivery status

-   C++ Release build succeeded on 2026-10-02, before the latest dev sync.
-   No shader compilation or shader validation.
-   No automated tests or in-game VR/SE/AE verification of the post-sync source.
-   Local CSVO deployment succeeded with hash verification and both Streamline directories preserved.
-   Draft PR preparation includes formatting and translation checks; the work
    is not ready to merge.

Source inspection, a Git whitespace check, the C++ build and deployment checks
were performed. These do not establish shader correctness or runtime behavior.

Your supplied `AGENTS.md` rules prohibit shader compilation/validation and shader
tests without an explicit request. I followed those rules. They explain the
missing validation; they do not explain away the unfinished implementation above.

## 5. Work needed to finish

1. Review runtime toggle coverage while preserving fixed feature availability
   defines and required shader interface/material/edition permutations.
2. Preserve the documented native-support exceptions; their live switches do not
   uninstall engine replacements. Full reversal would be additional implementation.
3. Exercise dormant-resource initialization, failure handling and temporal resets.
4. Keep menu/API notes aligned with any subsequent native-backend changes.
5. When explicitly requested, build and validate the changed shaders, then check
   live toggles in VR and one flat runtime, including cache activity and memory
   use. Fix any failures before deployment.

An initial shader-cache refresh is expected when installing changed shader source
and the new buffer layout. The intended outcome is that later feature toggles do
not require another refresh; LLF, True PBR, Unified Water and Horizon Fix integration
intentionally require a restart. Live-toggle behavior has not yet been
established across all features.
