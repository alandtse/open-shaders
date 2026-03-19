# Community Shaders SKSE API

This document explains how another SKSE plugin can talk to Community Shaders at runtime.

## Audience

This is for **consumer plugins** (mods that want to call into Community Shaders).

## API Version

- Interface revision: `1`
- Build number: returned by `getBuildNumber()`
- Weather-interaction methods were added in build `2`

## What This API Exposes

- Screen Space Shadows toggle (`SSS` in method names)
- Screen Space GI toggle
- Volumetric Lighting Exterior toggle
- Volumetric Lighting Weather-Interaction toggle

All four are runtime toggles.

## Files You Need In The Consumer Mod

Use the interface contract only:

- `include/VRAPI/CSinterface001.h`
- Optionally `src/VRAPI/CSinterface001.cpp` (convenience helper that fetches and caches the interface)

You do **not** need provider internals like `CSpluginapi.*`.

## Handshake Details

- Target plugin name: `CommunityShaders`
- Message type: `0x43534150` (`CSMessage::kMessage_GetInterface`)
- Requested revision: `1` (provider also accepts `0` as "latest")

## Integration Steps

1. Register your plugin's SKSE messaging listener.
2. On `SKSE::MessagingInterface::kPostLoad`, fetch the interface.
3. Store the pointer and check for `nullptr`.
4. Call methods when needed.

## Minimal Consumer Example

```cpp
#include "VRAPI/CSinterface001.h"

namespace
{
    CSPluginAPI::ICSInterface001* g_csApi = nullptr;

    void OnMessage(SKSE::MessagingInterface::Message* msg)
    {
        if (!msg) {
            return;
        }

        if (msg->type == SKSE::MessagingInterface::kPostLoad) {
            g_csApi = CSPluginAPI::GetCSInterface001();
            if (!g_csApi) {
                logger::warn("Community Shaders API unavailable");
            }
        }
    }
}

bool RegisterMessages()
{
    auto* messaging = SKSE::GetMessagingInterface();
    return messaging && messaging->RegisterListener("SKSE", OnMessage);
}

void SetShadowsEnabled(bool enabled)
{
    if (g_csApi) {
        g_csApi->SetSSSEnabled(enabled);  // SSS == Screen Space Shadows
    }
}
```

## Method Contract

`ICSInterface001` exposes:

- `unsigned int getBuildNumber()`
- `bool GetSSSEnabled()`
- `void SetSSSEnabled(bool enabled)`
- `bool GetSSGIEnabled()`
- `void SetSSGIEnabled(bool enabled)`
- `bool GetVolumetricLightingExteriorEnabled()`
- `void SetVolumetricLightingExteriorEnabled(bool enabled)`
- `bool GetVolumetricLightingWeatherInteractionEnabled()`
- `void SetVolumetricLightingWeatherInteractionEnabled(bool enabled)`

## Behavior Notes

- `SSS` means **Screen Space Shadows**, not Subsurface Scattering.
- Setters change runtime state in Community Shaders.
- `SetVolumetricLightingWeatherInteractionEnabled(false)` keeps VL available but stops weather-driven retuning (useful for rain-transition mod logic).
- Check `getBuildNumber() >= 2` before calling the weather-interaction methods.
- If the API pointer is null, Community Shaders is missing, too old, or not ready yet.
- Call from the main/game thread, or queue via SKSE task interface.

## Compatibility Guidance

- Always null-check the API pointer.
- Prefer checking `getBuildNumber()` before relying on behavior.
- Treat missing API as optional integration and continue without hard failure.
