# Open Shaders SKSE API

This document explains how another SKSE plugin can talk to Open Shaders (plugin name
`CommunityShaders`) at runtime. The interface is binary-compatible with the sibling
Community Shaders fork's revision-3 API, extended by Open Shaders revision 5.
Plugins compiled against that fork's `include/VRAPI/CSinterface001.h` work unchanged
against this one.

## Handshake Details

-   Target plugin name: `CommunityShaders`
-   Message type: `0x43534150` (`"CSAP"`, `CSMessage::kMessage_GetInterface`)
-   Supported revisions: `1`, `2`, `3`, `4`, `5` (and `0` for "latest")
-   Build number: `getBuildNumber()` returns `9`

To acquire the API interface:

1. Register your plugin's SKSE messaging listener.
2. On `SKSE::MessagingInterface::kPostLoad` (or later), dispatch a `CSMessage` to
   `"CommunityShaders"` with type `0x43534150`.
3. Read back `CSMessage::GetApiFunction` and invoke it with the desired revision.
4. Null-check the result; a null pointer means the plugin is missing, too old, or the
   revision is unknown.

The bundled convenience helper does all of this:

-   `include/VRAPI/CSinterface001.h` (the interface contract; the only file consumers need)
-   `src/VRAPI/CSinterface001.cpp` (optional `GetCSInterface001()` fetch-and-cache helper)

### Consumer licensing

`CSinterface001.h` and `CSinterface001.cpp` are licensed under
LGPL-3.0-or-later (see `COPYING.LESSER`). All other files remain under
the project's default license. Consult the license text for the terms
that apply to your use.

## Threading Model

Getters and wind sampling are safe from any thread. Setters may also be called from any thread: the value is
staged atomically and applied on the render thread at the start of the next frame, exactly
as if it had been edited in the in-game menu. A getter called immediately after its setter
may still return the previous value until that frame boundary passes.

## Method Reference

### Global Features

-   `unsigned int getBuildNumber()`: Returns the build compatibility level (`9`).
-   `bool GetSSSEnabled()` / `void SetSSSEnabled(bool enabled)`: Screen Space Shadows
    (`SSS` means Screen Space Shadows, not Subsurface Scattering). Takes effect live.
-   `bool GetSSGIEnabled()` / `void SetSSGIEnabled(bool enabled)`: Screen Space Global
    Illumination. Takes effect live. Unlike the in-game checkbox, toggling through the
    API triggers an SSGI shader recompile.
-   `bool GetVolumetricLightingExteriorEnabled()` / `void SetVolumetricLightingExteriorEnabled(bool enabled)`:
    Exterior volumetric lighting (god rays). Live on SE/AE; on VR the value is saved but
    only applies after a game restart (VR pre-allocates VL render targets at boot).
-   `bool GetLightLimitFixContactShadowsEnabled()` / `void SetLightLimitFixContactShadowsEnabled(bool enabled)`:
    Light Limit Fix contact shadows. Takes effect live.

### Upscaler Configuration

-   `UpscalePreset GetUpscalePreset()` / `void SetUpscalePreset(UpscalePreset preset)`:
    Shared DLSS/FSR render-scale preset. Supported values: `kNativeAA`, `kQuality`,
    `kBalanced`, `kPerformance`, `kUltraPerformance`. `kHoshipa` and `kUltraQuality`
    exist in the ABI but have no matching quality mode in this build; setting them logs
    a warning and is ignored. While render-at-upscaled-resolution is engaged the change
    is restart-gated; the getter reports the preset actually rendering.
-   `DLSSProfile GetDLSSProfile()` / `void SetDLSSProfile(DLSSProfile profile)`:
    DLSS model preset (DLSS only; does not affect FSR). Supported: `kJ`, `kK`, `kL`,
    `kM`. `kF` is not available in this build's DLSS integration; setting it logs a
    warning and is ignored. Takes effect live. When the in-menu preset is `Default`
    (automatic selection), the getter reports `kJ`, which is what automatic selection
    resolves to.
-   `UpscaleMethod GetUpscaleMethod()` / `void SetUpscaleMethod(UpscaleMethod method)`:
    Upscaler backend (`kNone`, `kTAA`, `kFSR`, `kDLSS`). On systems without DLSS,
    `kDLSS` coerces to `kFSR`. Restart-gated while render-at-upscaled-resolution is
    engaged; the getter reports the active method.
-   `bool GetRenderAtUpscaleResEnabled()` / `void SetRenderAtUpscaleResEnabled(bool enabled)`:
    The VR render-at-upscaled-resolution (Render Scale Mode) request. The setting is
    restart-gated: it is saved immediately but engages/disengages at the next game start.
-   `bool GetRenderAtUpscaleResActive()`: Whether the render-scale hook is actually
    active in the current session (as opposed to merely requested).

### ABI-Compatible Render-Scale Methods

This build has no live render-scale transition staging: restart-gated settings apply at
the next game start rather than through a runtime relatch. The following entry points are
kept for ABI compatibility and are safe to call with any argument:

-   `void SetVRUpscalingTransitionProfile(bool renderScaleModeEnabled, UpscalePreset preset, DLSSProfile profile)`:
    No transition is staged. If all arguments are supported values, the corresponding
    settings are written together (restart-gated ones apply at next boot); otherwise the
    whole call is ignored with a warning.
-   `void SetVRUpscalingTransitionProfileForMethod(UpscaleMethod method, bool renderScaleModeEnabled, UpscalePreset preset, DLSSProfile profile)`:
    Same semantics, with the explicit method included.
-   `uint32_t GetVRUpscalingApplyBlockReasons()`: Always returns `0` (never blocked).
-   `bool IsVRUpscalingProfileApplyAllowed()`: Always returns `true`.

The advisory `CSVRRenderScaleTransitionFade*` constants in the header are retained for
source compatibility only; they have no meaning here.

### Wind Sampling

-   `bool SampleWind(const WindVector* positions, WindSample* samples, uint32_t count)`:
    Samples multiple world positions from one immutable wind simulation snapshot. The
    call returns `false` until the first snapshot is available or when its arguments are
    invalid. `baseVelocity` is the weather-driven wind before procedural gusts;
    `gustVelocity` is the combined broad-gust and fine-wind delta for compatibility;
    `transientVelocity` contains impacts,
    shouts, wingbeats, and other transient forces; `finalVelocity` is their sum.
    `ambientGust` and `transientIntensity` expose normalized response values, and
    `frameId` identifies the shared snapshot used for the whole batch. Positions use
    Skyrim world-space units. Velocity vectors encode wind direction and Open Shaders'
    normalized response magnitude; they are not calibrated world units per second.

    Batch related objects in one call. Open Shaders briefly locks once to copy the
    published snapshot, then releases the lock before evaluating the positions. A
    single batch avoids lock and ABI overhead for every object. The maximum batch size
    is `CSWindMaximumBatchSize` positions.

-   `SampleWindExcludingHavokImpulses` (revision 4): Returns `wind`, the full visual sample,
    and `windExcludingHavokImpulses` from the same frame. The latter is base wind plus local
    gusts and fine wind plus only transients **not** marked as native physics. The velocity
    vectors encode normalized wind responses, not measured object velocities
    or forces.

    "Native physics" describes a transient whose triggering game event already
    applies its own force to Skyrim Havok objects. Open Shaders keeps that
    transient in `wind.transientVelocity` and `wind.finalVelocity` for grass,
    trees, and other visuals, but omits its wind proxy from `windExcludingHavokImpulses` so
    a Havok consumer does not apply a second push for the same event. This is a
    source-level tag, not a query of whether a particular object was hit.
    Consumers whose objects do not receive the native impulse can use the full
    `wind.finalVelocity` instead.

    The built-in routers mark Unrelenting Force, impacts with positive force in
    their projectile or explosion records, and VR thrown-weapon impacts as
    native. Other sources remain eligible for external physics pushes. The
    sampler does not automatically discover every Havok event. Both sampling
    methods are available in revision 4.

-   `SampleWindComponents` (revision 5): Takes the same world-position batch and returns
    `WindSampleComponents`. `baseVelocity`, `gustVelocity`, and `fineVelocity` are
    independent contributions. `gustVelocity` contains only the broad gust delta;
    `fineVelocity` contains the signed, continuously traveling slow and detail bands.
    `transientVelocity` includes all visual impacts, while
    `transientVelocityExcludingHavokImpulses` includes only non-native impacts.
    `finalVelocity` is base + gust + fine + full transients;
    `windExcludingHavokImpulses` uses the non-native transient contribution instead.
    All values come from the same immutable frame, with the same units, batch limits,
    thread safety, and failure behavior as the existing methods. `reserved` is zero.

    A simple integration can replace its existing wind input with `finalVelocity`
    (or its horizontal magnitude for a scalar input). An authored integration can use:

    ```cpp
    response = sample.baseVelocity * baseSensitivity
             + sample.gustVelocity * gustSensitivity
             + sample.fineVelocity * fineSensitivity
             + sample.transientVelocityExcludingHavokImpulses * impactSensitivity;
    ```

    The expression illustrates component weighting; `WindVector` is a plain ABI
    struct, so consumers use their own vector arithmetic. Visual consumers can select
    the full transient component instead. Fine wind already receives ambient and
    positive-gust amplitude coupling: do not multiply it by gust intensity again
    unless that is an intentional material response. Gust deltas can be negative
    during lulls; very large custom gains may require a consumer-specific bound.
    This API supplies wind input for flags, signs, and other integrations; each mod
    retains its own animation or physics response.

    Request revision 5 before calling the appended method. Revisions 1–4 retain their
    vtable slots and the 64-byte and 80-byte wind sample layouts; the new sample is
    104 bytes. `openshaders.pluginapi` exposes all three samplers for devbench inspection.

## Compatibility Guidance

-   Always null-check the API pointer and treat a missing API as optional integration.
-   New virtual methods are only ever appended to the interface; never reordered.
-   Settings changed through this API are persisted with Open Shaders' own settings when
    the user saves them in the menu; the API does not force a save itself.
-   Setters targeting a feature the user disabled at boot are ignored (and logged); the
    corresponding getter continues to report the stored value.
