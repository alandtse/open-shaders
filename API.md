# Open Shaders SKSE API

This document explains how another SKSE plugin can talk to Open Shaders (plugin name
`CommunityShaders`) at runtime. The interface is binary-compatible with the sibling
Community Shaders fork's revision-3 API: plugins compiled against that fork's
`include/VRAPI/CSinterface001.h` work unchanged against this one.

## Handshake Details

-   Target plugin name: `CommunityShaders`
-   Message type: `0x43534150` (`"CSAP"`, `CSMessage::kMessage_GetInterface`)
-   Supported revisions: `1`, `2`, `3` (and `0` for "latest")
-   Build number: `getBuildNumber()` returns `8`

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

## Threading Model

Getters are safe from any thread. Setters may also be called from any thread: the value is
staged atomically and applied on the render thread at the start of the next frame, exactly
as if it had been edited in the in-game menu. A getter called immediately after its setter
may still return the previous value until that frame boundary passes.

## Method Reference

### Global Features

-   `unsigned int getBuildNumber()`: Returns the build compatibility level (`8`).
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

## Compatibility Guidance

-   Always null-check the API pointer and treat a missing API as optional integration.
-   New virtual methods are only ever appended to the interface; never reordered.
-   Settings changed through this API are persisted with Open Shaders' own settings when
    the user saves them in the menu; the API does not force a save itself.
-   Setters targeting a feature the user disabled at boot are ignored (and logged); the
    corresponding getter continues to report the stored value.
