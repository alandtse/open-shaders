# ImGuiVRHelper — Design & Migration Plan

## Status

Draft. Captures the design conversation that produced the decision to extract
SCS's VR overlay/input infrastructure into a separate SKSE plugin
(`ImGuiVRHelper`) shared across the Skyrim VR modding ecosystem. The name is
deliberately generic — the helper has no SCS-specific surface and is intended
to glue any ImGui-using SKSE mod to the headset.

## Background

SCS solved "make ImGui-based mod settings visible and interactive in the HMD."
The implementation lives across:

-   [src/Features/VR.h](../../src/Features/VR.h) / [src/Features/VR.cpp](../../src/Features/VR.cpp) — the `VR : OverlayFeature` singleton
-   [src/Features/VR/](../../src/Features/VR/) — `Input.cpp`, `InSceneOverlay.cpp`, `OverlayDrag.cpp`, `OpenVRDetection.{cpp,h}`, `SettingsUI.cpp`, `StereoBlend.cpp`, `WandPointing.cpp`
-   [src/Utils/Input.h](../../src/Utils/Input.h) — `InputDeviceType`, `InputCombo`, JSON serialization
-   [src/Utils/VRUtils.h](../../src/Utils/VRUtils.h) / [src/Utils/VRUtils.cpp](../../src/Utils/VRUtils.cpp) — OpenVR helpers, matrix conversions, `OpenVRContext`, OpenComposite-compatible pose query

The Skyrim VR ecosystem has multiple ImGui-using SKSE mods. They each carry
their own overlay-rendering and input-handling stack, or they ship without VR
support at all. This document plans extracting SCS's stack into a shared host
plugin so any ImGui-using SKSE mod can register and become VR-interactive
without rebuilding the same plumbing.

## Problem & Goals

**Problem.** The pieces that make ImGui work in VR — OpenVR overlay
submission, controller laser raycast, button-combo handling, drag-to-reposition,
in-scene compositing (renders the overlay quad directly into each eye's
texture via an `IVRCompositor::Submit` hook) — are non-trivial and
currently locked inside SCS. Other VR-aware mods either reinvent them or skip
VR support.

**Goals.**

1. Ship a standalone SKSE plugin (the _helper_) that owns all VR overlay and
   input infrastructure, exposed as a versioned C++ interface.
2. Let any ImGui-using SKSE mod register with the helper at startup and
   become VR-interactive with a small, ABI-stable surface.
3. Survive Dear ImGui version drift across clients indefinitely. The helper
   does not link against any client's ImGui; clients render into helper-owned
   render targets using whatever ImGui version they ship.
4. Reduce SCS to a regular client of the helper, with no overlay/input code
   of its own.

**Non-goals.**

1. Conflict resolution between competing ImGui mods on the desktop. The
   ecosystem already arbitrates this; the helper does not.
2. Hosting a flatscreen ImGui pipeline. If the helper is not installed, each
   client continues to ship its own desktop ImGui hook unchanged.
3. Cross-client docking, drag-between-windows, or shared `ImGuiContext`. Each
   client is sandboxed in its own panel.
4. A shader/rendering API. SCS's stereo blend, depth-buffer culling, etc.
   stay in SCS.

## Architecture

```
+------------------------+         +------------------------+
| Client Mod (e.g. SCS)  |         | Other Client Mod       |
|                        |         |                        |
| - own ImGui context    |         | - own ImGui context    |
| - own DrawSettings()   |         | - own DrawSettings()   |
| - own JSON config      |         | - own JSON config      |
+----------+-------------+         +----------+-------------+
           |  ImGuiVRHelperAPI handshake (SKSE messaging, once)
           |  IImGuiVRHelperInterfaceNNN* (vtable, per-frame)
           v                                  v
+--------------------------------------------------------------+
| ImGuiVRHelper.dll (SKSE plugin)                                 |
|                                                              |
|  - OpenVR runtime detection (SteamVR / OpenComposite)        |
|  - IVROverlay submission + transform math                    |
|  - In-scene fallback render (runtimes without IVROverlay)    |
|  - Wand laser raycast -> panel UV                            |
|  - Controller state polling -> per-client Frame              |
|  - Combo matching + interactive recording modal              |
|  - Grip-to-drag overlay repositioning                        |
|  - Helper's own settings panel (offsets, scale, attach mode) |
|  - Per-client render targets (helper-owned ID3D11Texture2D)  |
+--------------------------------------------------------------+
                              |
                              v
                    OpenVR / SkyrimVR / D3D11
```

### Two coexisting client paths

**Texture-handoff path.** A client gets a helper-owned `ID3D11RenderTargetView`
sized for its panel and renders its own ImGui frame into it each tick. The
helper composites that texture onto each eye's render target via an
`IVRCompositor::Submit` hook (the _in-scene compositing_ path SCS uses
today). Clients never touch OpenVR. This rendering path is universal —
it works on SteamVR, OpenComposite, and any other runtime that drives
its eye textures through `IVRCompositor::Submit`, because the hook
fires before whatever the runtime does next.

**Raw VR input subscription path.** The helper invokes a per-frame callback
on each registered client with a flat data struct containing both controllers'
pose, axes, button held/pressed/released bitmasks, and the wand-pointer UV
within the client's panel. The client decides what to do — drives its own
`AddMousePosEvent`/`AddKeyEvent` calls into its own ImGui IO, fires app-level
hotkeys, runs gesture logic, etc.

Most clients use both paths. They render into the helper's RTV and consume
raw input from the callback to drive their own ImGui. Mouse synthesis is not
part of the helper because every client we care about already has its own
input mapping logic.

### Client render modes

Two distinct ways a client can use the helper for output. Both share the
same input pipeline above; the difference is where the client's pixels end
up in the headset.

**Panel mode (v1).** Client renders into a fixed-size offscreen RTV that
the helper composites as a positioned 2D overlay quad in 3D space (HMD-
attached or controller-attached). This is what SCS's existing menu uses
today: a 1920×1080 RTV submitted via `IVROverlay::SetOverlayTexture`. The
overlay floats in front of the user, the wand laser interacts with it,
and the user can drag-to-reposition it with grip. Designed for settings
panels, debug consoles, and menus.

**HUD mode (planned, post-v1).** Client renders into an eye-render-sized
RTV with transparent background, and the helper composites that RTV onto
each eye's render target before submit. This is the right shape for mods
that use ImGui as a transparent layer across the whole screen — e.g.
displaying subtitles positioned above an actor's head, nameplates,
diegetic indicators, or always-on debug overlays. The client does its own
world→screen projection (using the HMD pose and eye projection matrices
the helper exposes) and emits ImGui draw commands at the resulting
screen coords; transparent pixels pass through unchanged.

The two modes are not mutually exclusive — a single client mod could
register two clients (one panel, one HUD) for different purposes.
Selecting the mode happens at `RegisterClient` time via a bit in the
`flags` parameter.

The helper's panel-mode infrastructure (overlay textures, drag,
positioning) is independent of the HUD-mode infrastructure. HUD mode
specifically depends on:

-   The eye-submit hook (already required by the in-scene panel fallback
    path, so this is largely shared work)
-   Eye projection matrices exposed in the per-frame `Frame` struct or via
    a new accessor (`IImGuiVRHelperInterface002::GetEyeProjection(eye, out)`
    in a future API revision)
-   A per-eye RTV the client renders into, sized to match the game's eye
    render dimensions

Implementation slots in after the v1 panel work is done; the panel
machinery doesn't move and clients written for panel mode keep working
unchanged.

### Helper-owned responsibilities

-   All OpenVR API calls (`IVRSystem`, `IVROverlay`, `IVRCompositor`).
-   Both overlay textures (`menuTexture`, `menuControllerTexture`) plus their
    RTVs and SRVs.
-   The `IVRCompositor::Submit` hook that drives in-scene compositing —
    the primary (and currently only) path SCS uses to put the menu in
    the headset (`InstallSubmitHook` in `Features/VR/InSceneOverlay.cpp`).
    Note: SCS declares `vr::VROverlayHandle_t` members but never calls
    `IVROverlay::CreateOverlay`/`SetOverlayTexture` — the in-scene path
    is the actual rendering route, not a fallback.
-   The per-frame tick driving overlay transform updates, controller polling,
    and per-client `on_frame` callbacks. Implemented as an `IDXGISwapChain::Present`
    vtable detour (slot 8) installed at `BSGraphics::Renderer::InitD3D` time —
    same pattern as SCS today.
-   Combo matching, including the modal "press buttons now" recording overlay
    used by both helper-internal binds and client-requested binds.
-   Wand laser intersection math (`ComputeWandIntersection` from
    `Features/VR/WandPointing.cpp`).
-   Drag-to-reposition state machine (`OverlayDragState` and friends).
-   The helper's own settings ImGui panel (offsets, scale, attach mode, mouse
    speed, default combo timeouts). Helper links its own ImGui — it is the only
    context inside the helper.

### Why a separate plugin

-   **Single Present hook.** Multiple mods hooking `IDXGISwapChain::Present`
    fight each other. With the helper installed, only it hooks; clients tick
    off its callback.
-   **Single OpenVR overlay handle.** `IVROverlay::CreateOverlay` per client
    works but is wasteful and has Z-order issues. One pair of overlays
    (HMD-attached and controller-attached) is sufficient.
-   **Single drag/repositioning UI.** Users expect to grab the menu in one
    consistent way. Per-client implementations disagree.
-   **One settings panel for VR overlay placement.** Users configure offsets
    once, every client benefits.

## Public API

### Handshake (SkyrimVRESL pattern)

Adopt the pattern from
[SkyrimVRESL](https://github.com/alandtse/SkyrimVRESL/blob/master/src/SkyrimVRESLAPI.h),
itself credited to [HIGGS](https://github.com/adamhynek/higgs). The handshake
uses SKSE messaging exactly once; subsequent calls are direct vtable dispatch.

1. Helper exposes a public C++ header (`ImGuiVRHelperAPI.h`) that clients copy or
   submodule. The header is dependency-light: SKSE messaging types and basic
   integer types only. No ImGui, no OpenVR.
2. Helper registers a SKSE messaging listener for `kMessage_GetInterface =
0x...` (randomly generated, fixed for the helper's lifetime). On receipt,
   it fills `message.GetApiFunction = &Internal_GetApiFunction`.
3. Client calls `ImGuiVRHelperPluginAPI::GetImGuiVRHelperInterface001()` once after
   `kPostLoad`. The stub dispatches the handshake message to plugin name
   `"ImGuiVRHelper"`, then calls `GetApiFunction(1)` and casts the result to
   `IImGuiVRHelperInterface001*`. The pointer is cached for the process lifetime.
4. Subsequent calls go through the vtable. No SKSE messaging is involved.

### Versioned interface

```cpp
// ImGuiVRHelperAPI.h — public header, ships with helper, included by clients
#pragma once
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <cstdint>
#include <d3d11.h>

#include "ImGuiVRHelperTypes.h"  // wire structs only — no ImGui, no OpenVR

namespace ImGuiVRHelperPluginAPI
{
    constexpr const auto ImGuiVRHelperPluginName = "ImGuiVRHelper";

    struct ImGuiVRHelperMessage
    {
        enum : uint32_t { kMessage_GetInterface = 0x... /* randomly generated */ };
        void* (*GetApiFunction)(uint32_t revisionNumber) = nullptr;
    };

    struct IImGuiVRHelperInterface001
    {
        virtual uint32_t GetBuildNumber() = 0;

        // Lifecycle
        virtual uint32_t RegisterClient(const char* name, OnFrameFn on_frame,
                                        void* user, uint32_t flags) = 0;
        virtual void     UnregisterClient(uint32_t client_id) = 0;

        // Texture handoff (helper-owned RTV)
        virtual bool     GetPanel(uint32_t client_id, PanelHandle* out) = 0;

        // Raw input + pointer
        virtual bool     GetPointer(uint32_t client_id, float* u, float* v,
                                    uint32_t* device_idx) = 0;

        // Combos (helper does the matching; clients describe binds with InputCombo)
        virtual ComboId RegisterCombo(uint32_t client_id, const InputCombo* keys,
                                      size_t n, float timeout_s) = 0;
        virtual bool    ComboFired(ComboId) = 0;
        virtual void    StartComboRecording(uint32_t client_id, const char* label,
                                            ComboRecordedFn on_done, void* user,
                                            float timeout_s) = 0;
        virtual void    CancelComboRecording(uint32_t client_id) = 0;

        // Focus / visibility
        virtual bool     IsOverlayVisible() = 0;
        virtual void     RequestFocus(uint32_t client_id) = 0;
        virtual void     ReleaseFocus(uint32_t client_id) = 0;

        // Haptics back-channel
        virtual void     TriggerHaptic(uint32_t client_id, uint32_t haptic_token,
                                       uint32_t duration_us, float frequency,
                                       float amplitude) = 0;

        // Migration helper for porting clients with prior-life settings
        virtual bool     ImportLegacySettings(const char* json_blob) = 0;
    };

    IImGuiVRHelperInterface001* GetImGuiVRHelperInterface001();
}
```

Future revisions extend by inheritance (`IImGuiVRHelperInterface002 :
IImGuiVRHelperInterface001 { virtual NewMethod() = 0; };`). Helper's
`GetApiFunction` returns the highest version requested it can support, or
`nullptr` if asked for a version above its build.

### Wire data structures

All wire structs live in `ImGuiVRHelperTypes.h` and contain only:

-   Plain integer / float types
-   `ID3D11*` COM pointers (safe across DLLs because Skyrim provides one
    shared `ID3D11Device`)
-   The `InputCombo` / `InputDeviceType` types from [src/Utils/Input.h](../../src/Utils/Input.h)
    (lifted unchanged into the helper's public headers)

Every struct begins with `uint32_t abi_version; uint32_t struct_size;` for
forward-extension. Clients zero-fill any tail beyond their compiled size.

```cpp
// ImGuiVRHelperTypes.h — wire-stable, no implementation dependencies
//
// Licensed under LGPL-3.0-or-later. See api/COPYING.LESSER.
#pragma once
#include <cstdint>
#include <d3d11.h>

namespace ImGuiVRHelperPluginAPI
{
    inline constexpr uint32_t kInputAbiVersion = 1;

    struct Pose {
        float pos[3];                // meters, OpenVR standing-universe space
        float orient[4];             // quaternion w,x,y,z
        float vel[3], angvel[3];
        uint32_t valid;              // bit0 tracked, bit1 visible, bit2 has_velocity
    };

    // Wire-stable button enum. Owned by the helper, NEVER renumbered.
    // Maps from RE::BSOpenVRControllerDevice::Keys at the helper boundary.
    enum class Button : uint32_t {
        AX            = 0,   // RE Keys::kXA            (31)
        BY            = 1,   // RE Keys::kBY            (1)
        Menu          = 2,   // RE Keys::kMenu          (9)
        System        = 3,
        TriggerClick  = 4,   // RE Keys::kTrigger       (7)
        GripClick     = 5,   // RE Keys::kGrip          (2)
        StickClick    = 6,   // RE Keys::kStick         (33)
        PadClick      = 7,   // RE Keys::kTouchpad      (32)
        Shoulder      = 8,
        Reserved9     = 9,
        // ... reserve 16+ slots
    };

    struct Hand {
        uint32_t connected;
        uint32_t controller_kind;    // 0 unknown, 1 index, 2 oculus_touch,
                                     // 3 wmr, 4 vive, 5 cosmos, ...
        Pose pose;
        uint32_t buttons_held;       // bitmask of (1u << Button::X)
        uint32_t buttons_pressed;    // edges this frame
        uint32_t buttons_released;
        uint32_t buttons_touched;    // capacitive
        float    trigger;            // 0..1 analog
        float    grip;               // 0..1 analog
        float    stick_x, stick_y;   // -1..1
        float    pad_x, pad_y;       // -1..1
        uint32_t haptic_token;       // opaque; pass back to TriggerHaptic
    };

    struct Frame {
        uint32_t abi_version;        // == kInputAbiVersion
        uint32_t struct_size;
        float    dt;
        Pose     hmd;
        Hand     left, right;
        uint32_t flags;              // bit0 client_has_focus
                                     // bit1 overlay_visible
                                     // bit2 client_pointer_in_panel
    };

    struct PanelHandle {
        uint32_t                width;
        uint32_t                height;
        ID3D11RenderTargetView* rtv; // helper-owned; valid until UnregisterClient
    };

    using ComboId         = uint32_t;
    using OnFrameFn       = void (*)(const Frame*, void* user);
    using ComboRecordedFn = void (*)(const struct InputCombo*, size_t n, void* user);
}
```

### Public types lifted from SCS

[src/Utils/Input.h](../../src/Utils/Input.h) becomes a public header of the
helper unchanged. It already has the right shape: packed `device << 16 | key`
into `uint32_t`, JSON serialization with ADL-discoverable hooks, and
backward-compatible single-int-vs-array handling. Clients that store user
keybinds in their own JSON keep using `std::vector<InputCombo>` and the
existing `to_json`/`from_json` overloads.

[src/Utils/VRUtils.h](../../src/Utils/VRUtils.h) becomes an internal header of
the helper (not exported). Its OpenVR types must not leak into the public API.

## Component Inventory

### What moves to the helper

| Source                                                                             | Destination                           | Notes                                                                                        |
| ---------------------------------------------------------------------------------- | ------------------------------------- | -------------------------------------------------------------------------------------------- |
| [src/Features/VR/Input.cpp](../../src/Features/VR/Input.cpp)                       | `helper/src/Input.cpp`                | rewires from `Menu::KeyEvent` to `Frame` queue                                               |
| [src/Features/VR/InSceneOverlay.cpp](../../src/Features/VR/InSceneOverlay.cpp)     | `helper/src/InSceneOverlay.cpp`       | universal in-scene compositing via `IVRCompositor::Submit` hook (the primary rendering path) |
| [src/Features/VR/OverlayDrag.cpp](../../src/Features/VR/OverlayDrag.cpp)           | `helper/src/OverlayDrag.cpp`          | grip-to-drag state machine                                                                   |
| [src/Features/VR/WandPointing.cpp](../../src/Features/VR/WandPointing.cpp)         | `helper/src/WandPointing.cpp`         | laser raycast → panel UV                                                                     |
| [src/Features/VR/OpenVRDetection.{cpp,h}](../../src/Features/VR/OpenVRDetection.h) | `helper/src/OpenVRDetection.{cpp,h}`  | runtime probing                                                                              |
| [src/Utils/VRUtils.{h,cpp}](../../src/Utils/VRUtils.h)                             | `helper/src/internal/VRUtils.{h,cpp}` | internal-only, OpenVR types                                                                  |
| [src/Utils/Input.h](../../src/Utils/Input.h)                                       | `helper/api/ImGuiVRHelperInput.h`     | **public** API header                                                                        |
| Portions of [src/Features/VR/SettingsUI.cpp](../../src/Features/VR/SettingsUI.cpp) | `helper/src/SettingsUI.cpp`           | only the VR-overlay-glue settings; SCS's other settings stay                                 |
| `Features/VR.h` members listed below                                               | `helper/src/Overlay.{h,cpp}`          | overlay handles, textures, RTVs, drag state, wand state                                      |

`VR` class members that move:

```
menuOverlayHandle, menuControllerOverlayHandle
menuTexture, menuRTV, menuControllerTexture, menuControllerRTV
OverlayRenderContext (struct)
RecreateOverlayTexturesIfNeeded, SubmitOverlayFrame
SubmitHMDOverlay, SubmitControllerOverlay, HideAllOverlays
OverlayDragState (struct), UpdateOverlayDrag, CanPerformDrag,
  UpdateActiveDrag, TryStartNewDrag
SetFixedOverlayToCurrentHMD, UpdateFixedWorldPositioning
WandIntersectionState, ComputeWandIntersection,
  ComputeWandIntersectionForOverlayType, UpdateCursorFromWandPointing
OverlayWorldPosition (struct), fixedWorldOverlayPosition
InSceneResources (struct), InitInSceneResources, RenderInSceneOverlay,
  InstallSubmitHook
DetectOpenVRInfo, IsOpenVRCompatible, OpenVRInfo (struct)
ComboSequence (struct), menuOpenCombo, menuCloseCombo
isCapturingCombo, currentComboType, currentComboName,
  recordedCombo, comboStartTime, comboTimeout, recordingButtonControllers
ProcessVREvents, ProcessVRButtonEvent, UpdateControllerState,
  ProcessThumbstickScroll, ProcessControllerInputForImGui
ResetComboRecording, ApplyRecordedCombo, GetGripPressed
primaryControllerState, secondaryControllerState, lastKnownLeftHandedMode
vrControllerEventLog, VRControllerEventLog (struct)
savedPlayerWorldPos
Config struct (kOverlayWidth/Height, scale matrix, default offsets/timeouts)
Settings fields: VRMenuScale, VRMenuPositioningMethod, attachMode,
  VRMenuAttachController, VRMenu{Offset,ControllerOffset}{X,Y,Z},
  VRMenuControllerDiagnosticsTestMode, mouseDeadzone, mouseSpeed,
  EnableWandPointing, dragHighlightColor, VRMenu{Open,Close}Keys,
  VROverlay{Open,Close}Keys, comboTimeout, kAutoHideSeconds,
  EnableDragToReposition, VRMenuAutoResetDistance
```

### What stays in SCS

The shrunken `VR` class becomes a regular shader feature:

```
class VR : public Feature {           // no longer OverlayFeature
  Reset, SetupResources, ClearShaderCache
  EarlyPrepass, PostPostLoad, DataLoaded
  EnableDepthBufferCullingExterior/Interior, MinOccludeeBoxExtent
  gDepthBufferCulling, gMinOccludeeBoxExtent, UpdateDepthBufferCulling
  EnableStereoBlend + all StereoBlend* members and methods
  StereoBlendCB struct
  stereoOpt (VRStereoOptimizations)
  AnyScreenSpaceEffectLoaded
};
```

[src/Features/VR/StereoBlend.cpp](../../src/Features/VR/StereoBlend.cpp) and
[src/VRStereoOptimizations.h](../../src/VRStereoOptimizations.h) stay.

[src/Menu.cpp](../../src/Menu.cpp), [src/Menu/](../../src/Menu/), and all
`Feature::DrawSettings` impls stay. `globals::menu->IsEnabled` becomes a local
flag SCS owns; the helper never reads it.

### Tricky / shared

**`Settings` struct split.** The current `VR::Settings` mixes shader-feature
config with overlay/input config. After the split:

-   _Shader-side, stays in SCS:_ `EnableDepthBufferCullingExterior/Interior`,
    `MinOccludeeBoxExtent`, all `StereoBlend*` fields.
-   _Overlay-side, moves to helper:_ everything else listed above.

The helper persists its own settings in its own JSON file
(`Data\SKSE\Plugins\ImGuiVRHelper.json`). On first run after split, SCS reads the
old combined JSON, copies overlay-side fields into the helper's JSON via
`IImGuiVRHelperInterface::ImportLegacySettings(json)`, and strips them from its
own. One-shot migration. Document in CHANGELOG.

**`globals::menu->IsEnabled` reference in `IsOverlayVisible`.** Inverts:
helper exposes `IsOverlayVisible()`, SCS calls into helper to ask. SCS no
longer leaks its menu-open flag.

**Combo recording UI.** Moves into the helper. Helper renders a modal "press
buttons now" panel on top of all clients during capture, eats input until
combo done or timeout, returns the result to the requesting client via
`ComboRecordedFn`. While recording is active, the helper temporarily holds
focus regardless of which client requested it; prior focus is restored on
completion or cancel.

## Lifecycle & Timing

Because clients register early via SKSE messaging (and we assume any client
that ships against the helper does so before `kPostLoad` completes), the
helper does not need lazy initialization paths.

| Stage                           | Helper                                                                                                                                                                                                   | Client                                                                          |
| ------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| `kPostLoad`                     | listener for `kMessage_GetInterface` registered                                                                                                                                                          | dispatches handshake, caches `IImGuiVRHelperInterface*`, calls `RegisterClient` |
| `kPostPostLoad`                 | —                                                                                                                                                                                                        | calls `RegisterCombo` for any startup binds                                     |
| `kInputLoaded`                  | —                                                                                                                                                                                                        | —                                                                               |
| `kDataLoaded`                   | OpenVR probe → IVROverlay setup → texture allocation → `BSGraphics::Renderer::InitD3D` thunk installs `IDXGISwapChain::Present` and `Submit` hooks                                                       | —                                                                               |
| First `Present` after data load | begins per-frame tick: poll controllers → invoke `on_frame` for each client → resolve focus → render helper UI if visible → composite client RTVs → `IVROverlay::SetOverlayTexture` → original `Present` | starts receiving `on_frame` callbacks; renders into `GetPanel` RTV              |

Hook order matters. The helper's `Present` detour must run _after_ any
clients that legitimately draw to the desktop swapchain (e.g., SCS's own
flatscreen ImGui menu when running on flatscreen). On VR, the helper's job is
to capture the per-frame moment to refresh its overlay textures, not to
intercept what clients draw to the desktop. Conventionally, install via
`Detours::X64::DetourClassVTable` slot 8 at `BSGraphics::Renderer::InitD3D`
time, same point SCS uses today.

## Build & Distribution

**Template repo.** The helper will adapt the structure of
[Intellightent](https://github.com/alandtse/Intellightent), an SKSE plugin
that uses xmake + CommonLibSSE-NG with a small, modern footprint. The
Intellightent layout is:

-   `xmake.lua` at root (no CMake)
-   `lib/commonlibsse-ng/` as a git submodule
-   `src/` with `pch.h`, `Version.h.in` template, `main.cpp`, feature `.{cpp,h}`
-   `add_rules("commonlibsse-ng.plugin", { name, author, description })` for
    SKSE plugin metadata
-   DLL naming via `set_basename("<name>")` — Intellightent uses a `-ng`
    suffix as a legacy-disambiguation convention from older SE/AE-only
    plugins; new plugins don't need it
-   `SkyrimPluginTargets` env var for auto-deploy on build (semicolon-separated)
-   C++23, `set_warnings("allextra")`, `releasedbg` default mode, `/DEBUG`
    linker flag for PDBs

**Repository layout** (adapting Intellightent):

```
ImGuiVRHelper/
├── xmake.lua
├── README.md
├── COPYING
├── EXCEPTIONS
├── lib/
│   └── commonlibsse-ng/              # submodule, VR-targeted runtime
├── api/                              # PUBLIC — copied or submoduled by clients
│   ├── ImGuiVRHelperAPI.h               # versioned interface declaration
│   ├── ImGuiVRHelperTypes.h             # wire structs (no ImGui, no OpenVR)
│   ├── ImGuiVRHelperInput.h             # InputCombo, InputDeviceType (lifted unchanged)
│   └── ImGuiVRHelperAPI.cpp             # client-side handshake stub
├── src/
│   ├── pch.h
│   ├── Version.h.in
│   ├── main.cpp                      # SKSE plugin entry, message listener
│   ├── HelperImpl.{h,cpp}            # IImGuiVRHelperInterface001 implementation
│   ├── ClientRegistry.{h,cpp}        # per-client_id state
│   ├── Overlay.{h,cpp}               # IVROverlay handles, textures, transforms
│   ├── Input.cpp                     # controller polling → Frame queue
│   ├── InSceneOverlay.cpp            # primary path: IVRCompositor::Submit hook + eye-render compositing
│   ├── OverlayDrag.cpp               # grip-to-drag
│   ├── WandPointing.cpp              # laser raycast
│   ├── OpenVRDetection.{cpp,h}       # runtime probing
│   ├── SettingsUI.cpp                # helper's own ImGui settings panel
│   └── internal/
│       └── VRUtils.{h,cpp}           # internal OpenVR helpers, matrix math
└── docs/
```

**Build system.** xmake, following Intellightent's `xmake.lua` template:

```lua
set_xmakever("2.8.2")
set_config("rex_ini", true)
includes("lib/commonlibsse-ng")

set_project("ImGuiVRHelper")
set_license("GPL-3.0")
local version = "1.0.0"
local ver = version:split("%.")
set_version(version)

set_languages("c++23")
set_warnings("allextra")
set_policy("package.requires_lock", true)
add_rules("mode.debug", "mode.releasedbg")
set_defaultmode("releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- xmake-requires.lock managed deps
add_requires("openvr")
add_requires("imgui", { configs = { dx11 = true, win32 = true } })
add_requires("nlohmann_json")
add_requires("directxtk")  -- SimpleMath used by VRUtils

target("ImGuiVRHelper")
    add_deps("commonlibsse-ng")
    add_packages("openvr", "imgui", "nlohmann_json", "directxtk")
    set_basename("imgui-vr-helper")
    add_shflags("/DEBUG", { force = true })
    set_configvar("VERSION_MAJOR", tonumber(ver[1]))
    set_configvar("VERSION_MINOR", tonumber(ver[2]))
    set_configvar("VERSION_PATCH", tonumber(ver[3]))
    set_configvar("VERSION_STRING", version)
    add_rules("commonlibsse-ng.plugin", {
        name = "ImGuiVRHelper",
        author = "Skyrim Community Shaders contributors",
        description = "VR overlay/input glue for ImGui-based SKSE mods",
    })
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_files("api/ImGuiVRHelperAPI.cpp")  -- so the plugin's own internal handshake fn is built
    add_includedirs("src", "api")
    set_pcxxheader("src/pch.h")
    add_configfiles("src/Version.h.in")
    after_build(function(target)
        local deploy_dirs = os.getenv("SkyrimPluginTargets")
        if not deploy_dirs then return end
        for _, dir in ipairs(deploy_dirs:split(";")) do
            dir = dir:trim()
            if dir ~= "" then
                local dest = path.join(dir, "SKSE", "Plugins")
                os.mkdir(dest)
                os.cp(target:targetfile(), dest)
                if os.isfile(target:symbolfile()) then
                    os.cp(target:symbolfile(), dest)
                end
                print("Deployed to " .. dest)
            end
        end
    end)
```

Note that SCS itself uses CMake. SCS will consume the helper as a submodule
but does not need to drive its build — clients only need the `api/*.h`
headers at compile time (no link dependency on the DLL). xmake-vs-CMake
mismatch is a non-issue.

**Distribution model.** Helper ships as `imgui-vr-helper.dll` to
`Data\SKSE\Plugins\`. Clients add an optional dependency in their FOMOD or
documentation: "Install ImGuiVRHelper to enable in-headset menus."

**Submodule path during development.** Per user's preference, SCS will pull
the helper as a git submodule for development:

```
extern/ImGuiVRHelper/                    # submodule
   api/ImGuiVRHelperAPI.h                # included by SCS at compile time
   api/ImGuiVRHelperTypes.h
   api/ImGuiVRHelperInput.h
   api/ImGuiVRHelperAPI.cpp              # compiled into SCS to provide the handshake stub
```

SCS's `extern/ImGuiVRHelper/api/` is the _only_ path it touches; SCS adds
`api/ImGuiVRHelperAPI.cpp` to its own build to supply the client-side handshake
implementation. The helper's internals (`src/`, `lib/`) are never included
from SCS. Other client mods do the same.

## Licensing

The helper uses a split license that lets non-GPL clients consume the public
API without GPL infection while keeping the core copyleft.

**Layout:**

| Path                   | License                                 | Notes                               |
| ---------------------- | --------------------------------------- | ----------------------------------- |
| `COPYING`              | GPL-3.0-or-later                        | Full GPL text, copied from SCS      |
| `COPYING.LESSER`       | LGPL-3.0-or-later                       | Full LGPL text, applies to `api/`   |
| `EXCEPTIONS.md`        | GPL linking exception                   | Copied from SCS, applies to core    |
| `api/COPYING.LESSER`   | (link to root)                          | Short pointer / SPDX header         |
| `api/*.h`, `api/*.cpp` | LGPL-3.0-or-later                       | SPDX headers; clients vendor freely |
| `src/**`               | GPL-3.0-or-later WITH Modding-Exception | SPDX headers in each file           |

**Why this split.** Clients have varied licensing — some are GPL, some are
MIT, some are closed-source. Forcing GPL on everyone who calls
`RegisterClient` would defeat the helper's purpose as an ecosystem-wide
bridge. LGPL on the API headers + `.cpp` handshake stub means a closed-source
client can vendor those files (and link the SKSE plugin DLL via the
runtime-resolved interface pointer) without their own code becoming GPL.
The core implementation under `src/` stays GPL, so improvements to the
helper itself flow back.

**SCS-style modding exception.** Copy [SCS's EXCEPTIONS.md](../../EXCEPTIONS.md)
verbatim into the helper repo. The text already says "this Program" generically
and addresses linking against Skyrim itself ("Modded Code") and modding
libraries with incompatible licenses ("Modding Libraries"). Same situation
applies to the helper. No edits needed beyond confirming licensee/copyright
attribution at the top.

**File headers.** Each source file gets an SPDX identifier:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-SCS-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper authors. See COPYING and EXCEPTIONS.md.
```

For API files:

```cpp
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper authors. See api/COPYING.LESSER.
```

The `LicenseRef-SCS-Modding-Exception` is a custom SPDX reference matching
how SCS's exception is identified; alternatively use a project-specific
ref like `LicenseRef-ImGuiVRHelper-Modding-Exception` and have the
`EXCEPTIONS.md` define it.

**Practical implications for clients.**

-   A GPL-3.0 client (like SCS itself) consumes the helper with no caveats.
-   An MIT or proprietary client vendors `api/*.h` and `api/ImGuiVRHelperAPI.cpp`
    (LGPL); their own code stays under their own license. The LGPL "must
    allow relinking" clause is satisfied trivially because the API talks to
    the helper DLL through SKSE messaging — the DLL is replaceable.
-   No client links the helper's `src/` code at all. The DLL is loaded at
    runtime by SKSE; clients never have static or dynamic link dependency
    on it beyond the messaging handshake.

## Migration Plan

The migration is sized at roughly 6,000 lines moved + ~1,500 lines of new
glue code (registry, handshake, public headers, helper settings JSON).
Estimated as 5 PRs.

**PR 1 — helper repo skeleton.** Stand up `ImGuiVRHelper` repo following the
template once provided. SKSE plugin entry, public API headers
(`ImGuiVRHelperAPI.h`, `ImGuiVRHelperTypes.h`, `ImGuiVRHelperInput.h`),
`IImGuiVRHelperInterface001` declaration with stub implementations that log
and return success. Compiles, exports the handshake symbol via SKSE
messaging, ships nothing functional. Smoke-testable: any plugin can fetch
the interface and get back non-null.

> **PR-1 build notes (verified).** Three things that bit during the first
> build, worth pinning here so this is reproducible:
>
> 1. **Force the build platform.** Run xmake from Git Bash and it
>    auto-detects `mingw/x86_64`, which marks `directxtk` and `openvr` as
>    unsupported. Always configure with `xmake f -y -p windows -a x64 -m
releasedbg`. PowerShell or a VS Developer Prompt sidesteps this, but
>    the explicit flags are the safe default.
> 2. **Don't rely on a generated `Version.h` on the include path.** xmake's
>    `add_configfiles("src/Version.h.in")` writes to
>    `build/.gens/<target>/<plat>/<arch>/<mode>/Version.h`, which is _not_
>    auto-added to the include search list. Cleanest fix: skip the
>    template, pass numeric components via `add_defines` (e.g.
>    `add_defines("IMGUI_VR_HELPER_VERSION_MAJOR=" .. ver[1])`), and
>    stringify in C++ with the standard `#x` two-step macro. `set_configvar`
>    is still called so the SKSE plugin manifest from
>    `commonlibsse-ng.plugin` rule gets the version.
> 3. **`SKSE::Init(a_skse)` already initializes logging.** Don't hand-roll
>    a spdlog file sink — calling `spdlog::sinks::basic_file_sink_mt`
>    requires extra includes that aren't in the SKSE PCH and the manual
>    setup is fragile. Just call `SKSE::Init` and use `logs::info(...)`.

**PR 2 — lift the OpenVR machinery.** Copy
`Features/VR/{Input,InSceneOverlay,OverlayDrag,WandPointing,OpenVRDetection}.{cpp,h}`
plus `Utils/VRUtils.{h,cpp}` and `Utils/Input.h` verbatim. Sever
include-paths back to SCS — main work is replacing `Menu::KeyEvent`,
`globals::*`, `OverlayFeature`, `Feature` with helper-internal equivalents.
Wire up the per-frame tick (Present-vtable detour at `InitD3D`) and the
`on_frame` dispatcher that just iterates a `std::vector<ClientState>` and
calls each registered `OnFrameFn`. Texture creation, `IVROverlay::SetOverlayTexture`,
in-scene compositing (universal, works on SteamVR + OpenComposite), drag,
wand pointing all functional. Helper has no clients
yet, so visually it's invisible.

**PR 3 — helper's own settings panel + combo recording.** Port the
VR-overlay-glue portions of `SettingsUI.cpp`. Helper renders its own ImGui
(its own context, internal-only) using the same Present hook tick. Combo
recording flow exposed as `StartComboRecording`. Helper's settings persist
to `Data\SKSE\Plugins\ImGuiVRHelper.json`. At this point the helper is
self-contained: install it, get a settings panel in VR for offsets/scale/etc.,
even with no clients.

**PR 4 — port SCS to be the first client.** In SCS:

-   Delete the lifted files.
-   Shrink `VR : OverlayFeature` to `VR : Feature` with only shader bits.
-   Add `extern/ImGuiVRHelper` submodule, include `ImGuiVRHelperAPI.h`.
-   In `Menu::Init`, fetch interface via handshake, call `RegisterClient` with
    SCS's `on_frame` lambda, store `client_id`.
-   In `Menu::DrawOverlay`, call `helper->GetPanel(client_id, &panel)`,
    `OMSetRenderTargets(panel.rtv)`, render ImGui as today, restore state.
-   In SCS's `on_frame` callback, drive ImGui's `AddMousePosEvent`,
    `AddMouseButtonEvent`, `AddMouseWheelEvent` from `Frame` data using
    the existing logic from `Features/VR/Input.cpp` (now in the helper but
    conceptually mirrored on the client side for ImGui binding).
-   Replace `VRMenu{Open,Close}Keys` reads with `RegisterCombo` + `ComboFired`
    polling.
-   Remove SCS's own `IsOverlayVisible` reference; ask the helper.
-   One-shot migration: read old combined JSON, send overlay-side fields to
    helper via `ImportLegacySettings`, strip them from SCS's JSON.
-   Bump SCS feature `.ini` versions; CHANGELOG entry.

End state for SCS: `extern/ImGuiVRHelper/api/ImGuiVRHelperAPI.h` is the only file
touched in `extern/`, plus a few hundred lines in `Menu.cpp`/`State.cpp`/
`VR.{h,cpp}` for the integration. Net SCS code drops by ~6,000 lines.

**PR 5 — polish and harden.** Fallback paths when helper is missing
(GetMessagingInterface dispatch returns no GetApiFunction → SCS logs and
disables VR-overlay UI but keeps flatscreen unchanged). Helper API version
mismatch handling (helper too old → SCS falls back; helper too new → SCS
gets nullptr from `GetImGuiVRHelperInterface001()` because it requested an old
version, fall back). Stress-test the in-scene compositing path against
OpenComposite. Confirm haptics round-trip works. Verify combo recording
modal correctly restores focus.

## Decisions Made (in this conversation)

| Decision                                                                                                 | Rationale                                                                              |
| -------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| Helper does not link client ImGui; clients render into helper-owned RTVs                                 | Survives ImGui version drift indefinitely                                              |
| Two coexisting paths: texture handoff + raw VR input subscription                                        | Mouse synthesis alone can't serve mods like SCS that already have button-mapping logic |
| API uses SkyrimVRESL/HIGGS handshake pattern (SKSE messaging once → vtable forever)                      | Per-frame callbacks need direct fn ptrs; messaging too lossy/slow                      |
| Wire-stable button enum owned by helper (`ImGuiVRHelperPluginAPI::Button`)                               | OpenVR/`RE::BSOpenVRControllerDevice::Keys` may renumber                               |
| Versioned interface via inheritance (`IImGuiVRHelperInterface002 : 001`)                                 | Same as SkyrimVRESL; clean forward extension                                           |
| Combo recording lives in helper, exposed as API                                                          | Helper already needs it for its own binds; clients shouldn't reimplement               |
| Helper does its full init at `kDataLoaded`; no lazy hook installation                                    | Clients all register by `kPostLoad`, no race condition                                 |
| Public types from `Utils/Input.h` (`InputCombo`, `InputDeviceType`) lifted unchanged into public headers | Already JSON-clean and used in client persistence                                      |
| `Utils/VRUtils.h` is helper-internal, not public                                                         | Contains OpenVR types we don't want leaking through the API                            |
| Helper does NOT do mouse synthesis as a first-class feature                                              | Every target client has its own input mapping; mouse synth would double-fire           |
| One pair of overlays (HMD-attached, controller-attached), shared across clients                          | Z-order, drag, and user expectation all favor a single shared overlay                  |
| Helper owns the Present hook; clients tick off `on_frame`                                                | Avoids per-mod Present-hook collisions                                                 |

## Open Questions

1. ~~**Plugin name and DLL filename.**~~ Resolved — plugin name is
   `ImGuiVRHelper`, DLL is `imgui-vr-helper.dll`. Generic naming chosen
   so non-Skyrim or non-SCS-affiliated clients aren't put off.

2. **Combo recording UX when multiple panels are visible.** During capture,
   the helper takes focus; what happens to other clients' panels — frozen,
   dimmed, hidden? Easiest: dimmed but still rendered. Confirm with user.

3. **Per-client overlay vs single overlay.** Current plan: one HMD overlay
   composites all visible clients' panels stacked or tabbed. If two clients
   request `RequestFocus` at once, last-wins. Worth a UI sketch before PR 4
   — does the helper render tabs/spines for client switching, or do clients
   stack like ImGui windows?

4. **Migration timing for existing SCS users.** Once the helper ships, SCS
   without the helper installed loses VR-overlay UI. Either:
   (a) Hard dependency: SCS requires helper.
   (b) Soft dependency: SCS detects helper, uses it; otherwise VR menu falls
   back to current code path.
   (b) is friendlier but doubles the matrix. Recommend (a) for a clean
   release; consider (b) if user-impact is significant.

5. **Repo / submodule template.** Resolved — using
   [Intellightent](https://github.com/alandtse/Intellightent) as the
   structural template (xmake + CommonLibSSE-NG submodule). See **Build &
   Distribution** above.

## Future / Version Evolution

Things the v1 API explicitly does not include but should remain forward-
compatible with:

-   **HUD-mode rendering.** First-class transparent full-screen overlay
    for clients that want to draw subtitles/nameplates/diegetic indicators
    on top of the eye render rather than as a positioned panel. See the
    _Client render modes_ section above for the full design. Concretely,
    the v2 API would add: - A `kClientFlag_HUDMode` bit accepted by `RegisterClient`. Clients
    that pass it get an eye-render-sized RTV from `GetPanel` instead of
    the 1920×1080 panel surface. - `IImGuiVRHelperInterface002::GetEyeProjection(uint32_t eye, float
out[4][4])` — left/right eye projection matrices so clients can do
    their own world→screen positioning. - A new compositing pass on the eye-submit path that alpha-blends the
    HUD client's RTV onto each eye texture before the original Submit.
    Re-uses the same hook the in-scene panel fallback already needs. - A `Frame.eye_projection_dirty` flag (or equivalent) so clients can
    cache the matrices and only rebuild on FOV / IPD change.
-   **Per-client overlay attachment override.** A future client might want its
    panel attached to a specific controller while another stays HMD-attached.
    Add `IImGuiVRHelperInterface002::SetClientAttachMode(client_id, mode)`.
-   **Stereo / depth-aware panels.** Per-client opt-in to render at depth
    rather than as a screen-space overlay. Add via interface revision.
-   **Custom overlay shapes / curved panels.** Today everything is a flat
    quad. Curved or cylindrical surfaces are an extension.
-   **OpenXR backend.** `OpenVRDetection.cpp` already discriminates SteamVR vs
    OpenComposite. A future OpenXR pathway can be added without breaking the
    public API since the API never exposes OpenVR types.
-   **Multi-monitor / passthrough overlay.** Mixed-reality setups may want the
    same overlay rendered to a passthrough surface — extends submission, not
    the public API.

Each extension should arrive as `IImGuiVRHelperInterfaceNNN : IImGuiVRHelperInterface(NNN-1)`
so old clients keep working unchanged.

## References

-   [SkyrimVRESL API](https://github.com/alandtse/SkyrimVRESL/blob/master/src/SkyrimVRESLAPI.h) — handshake pattern reference
-   [HIGGS](https://github.com/adamhynek/higgs) — original interface pattern
-   [Intellightent](https://github.com/alandtse/Intellightent) — xmake/CommonLibSSE-NG plugin template
-   [Dear ImGui input event API](https://github.com/ocornut/imgui/blob/v1.87/imgui.h) — stable since 1.87
-   [OpenVR IVROverlay docs](https://github.com/ValveSoftware/openvr/wiki/IVROverlay-Overview)
-   Current SCS VR implementation:
    [VR.h](../../src/Features/VR.h),
    [VR.cpp](../../src/Features/VR.cpp),
    [Features/VR/](../../src/Features/VR/),
    [Utils/Input.h](../../src/Utils/Input.h),
    [Utils/VRUtils.h](../../src/Utils/VRUtils.h)
