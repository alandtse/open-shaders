# Anti-Lag comparison with alandtse/dev

Compared on 2026-10-24 against freshly fetched
[alandtse/dev](https://github.com/alandtse/skyrim-community-shaders/commit/79906cc59a97abdc8931b3c0217f45d5e9151254),
commit `79906cc59a97abdc8931b3c0217f45d5e9151254`
(`fix(cache): reuse precompiled release shaders (#998)`, 2026-10-23).
The comparison includes the working tree and the earlier uncommitted SDK migration.

## Differences before removal

All tracked differences were confined to the Anti-Lag integration:

| Area                                                   | Difference from dev                                                                 |
| ------------------------------------------------------ | ----------------------------------------------------------------------------------- |
| `.gitmodules`, `CMakeLists.txt`, `extern/AntiLag2-SDK` | Official pinned AMD SDK submodule, include path and initialization diagnostic.      |
| `src/Feature.h`, `src/Hooks.cpp`                       | Generic feature callback before engine input polling.                               |
| `Upscaling.cpp`, `Upscaling.h`                         | Anti-Lag settings/UI/commands, rendered-FPS cap, scope and native CPU fallback.     |
| `DX12SwapChain.cpp`, `.h`                              | SDK lifecycle/update, residual CPU timer, FSR handshake and end marker.             |
| `antilag2inputpacing.h`                                | Added CPU waitable-timer/sleep limiter, absent from dev.                            |
| `antilag2sdk.h`                                        | Checked SDK state adapter from the earlier migration, untracked before this task.   |
| `RemoteControl/ToolSpecs/FeatureTool.h`                | Added settings and command descriptions/schema.                                     |
| Docs and tests                                         | Anti-Lag documentation/regressions and shared test helper SDK include-path support. |

## Removed

-   CPU fallback/backstop timer, header, member and native D3D11 call path.
-   Configurable SDK FPS cap: the adapter now accepts only enable state and always supplies `maxFPS = 0`.
-   Limiter settings/defaults/serialization/clamping, gameplay scope, FPS slider and limiter status UI.
-   `setGameplayFPSLimit` and limiter parameters in `setAntiLag2`, with related tool descriptions and tests updated.

Anti-Lag latency reduction, SDK submodule/checked-state adapter,
FSR context handshake, rendering-end marker and shutdown drain remain.
Old limiter keys have no runtime effect and are omitted when settings are serialized.
Dev's VSync, display-refresh cap, Reflex and frame-generation pacing were not edited.

## Preservation checks

Source comparisons against the baseline passed:

-   `Upscaling::FrameLimiter()` and `LoadSettings()` match dev exactly.
-   Frame Generation UI retains dev's controls, with a read-only Anti-Lag Active/Unavailable line added below the current FSR method only; the independent Anti-Lag checkbox is removed.
-   `DX12SwapChain::PresentImpl<false>()` matches the dev presentation body after compile-time removal of the Anti-Lag-only blocks; eligible sessions select the specialization containing those notifications.
-   The existing `BSInputDeviceManager_PollInputDevices` Reflex thunk matches dev exactly.
-   `Streamline.cpp`, `Streamline.h` and `FidelityFX.cpp` match dev in full.

The SDK's remaining pre-input delay provides Anti-Lag latency reduction;
its optional explicit FPS limiter is disabled.

## Conditional installation

The SDK check now runs only during FSR swap-chain startup. Its successful
result permits optional pre-input hook installation and selection of the
Anti-Lag presentation proxy. Failed detection or hook attachment leaves the
ordinary proxy active with no context retained. Generic DX12/DLSS-G creation
and the main hook installer do not invoke or install Anti-Lag.

The independent `antiLag2Enabled` setting and `setAntiLag2` command are removed.
Session backend selection determines eligibility; menus and loading do not
change it. See [the current integration](antilag2.md) for lifetime details.

## Validation

Five targeted Anti-Lag tests pass, including Windows C++ coverage using the
real SDK interface to assert every state update carries zero FPS and rejected
toggles remain retryable. Conditional startup, hook failure, context cleanup,
proxy selection and failed-drain retention are covered by compiled production-method fixtures. Compilation and runtime outcomes are reported separately.
No Skyrim process is launched by this task.

## Testing overlay removed

The temporary test toggle, SDK FPS limiter, commands/query and per-call success
logging have been removed. Automatic conditional installation and the fixed
zero FPS cap are restored. Initialization and one-time failure diagnostics remain.
The existing dev limiters, Reflex and ordinary presentation path are unchanged.
