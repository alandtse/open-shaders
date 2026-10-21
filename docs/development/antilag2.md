# AMD Anti-Lag 2

The DX12 SDK header, license and integration guide in
`extern/AntiLag2-SDK/` are copied unchanged from the local RedstoneRT
checkout at `D:/pcmaster/open-shaders-FSR-REDSTONE/extern/AntiLag2-SDK/`.
The header identifies AMD's 2024 Anti-Lag 2.0 SDK and carries its MIT
license. This is a vendored, header-only dependency; no SDK binaries or
sample applications are required by the integration.

The integration uses the persistent frame-generation DX12 device and calls
Anti-Lag 2 immediately before engine input polling. The FSR swap chain
receives the SDK's private-data handshake, and the end-of-frame marker
belongs after main rendering submission and before swap-chain Present.
Shutdown drains pending FSR presents before releasing the SDK interface;
if that drain fails, process cleanup retains ownership of the interface.
See the vendored README for AMD's API contract and latency monitor.

This change is a source-only handoff. Compilation, driver initialization,
FPS enforcement, menu/loading-screen frame generation and input latency
remain unverified until the user builds and runs the game. Runtime
acceptance requires VR plus one flat edition (SE or AE); report which
editions actually ran. Skyrim launches remain user-controlled.

The Frame Generation tab exposes the gameplay limit as displayed FPS with
FSR's fixed 2x multiplier (40-480, default 120). Settings store rendered
FPS in `Upscaling.antiLag2FPSLimit` (20-240, default 60). The existing display
refresh-rate cap remains an additional ceiling. Menus and loading use
60 rendered FPS. On the FSR DX12 path Anti-Lag runs first; scene frames use
the RedstoneRT residual pre-input timer, and unsupported or failing driver
APIs fall back to that timer. NVIDIA DLSS-G retains its own Reflex pacing.

`Upscaling.frameGenerationAllowInMenus` is a live, persisted toggle, off
by default. Neutral guides cover frames without a world pass; UI routing
is decided before UI rendering, and steady-clock elapsed time keeps FG
timing valid while game time is paused. FSR REDSTONE is a companion tab
alongside Upscaling and Frame Generation, containing only RR/RC controls.

Devbench commands under Upscaling: `setGameplayFPSLimit` accepts even
`displayedFPS` from 40 to 480; `setFrameGenerationInMenus` accepts boolean
`enabled`. The generic feature get/set interface exposes both settings.
Commands change live settings without saving automatically.

For runtime acceptance, test the gameplay FPS control at multiple values
with FSR frame generation enabled, compare rendered FPS against the
requested cap and displayed FPS against FSR's fixed 2x multiplier, then
toggle the option to retain frame generation in menus and loading screens.
Check an unavailable/failed Anti-Lag path as well as a supported AMD
driver. An SDK success result or a timer wait alone does not establish
latency reduction or displayed-frame behavior.
