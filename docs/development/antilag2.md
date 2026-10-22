# AMD Anti-Lag 2

The DX12 SDK header, license and integration guide in
`extern/AntiLag2-SDK/` are vendored from the upstream
[AMD Anti-Lag 2 SDK](https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK/releases/tag/v2.0.0a).
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

Runtime acceptance requires VR plus one flat edition (SE or AE); report
which editions actually ran. Skyrim launches remain user-controlled.

The Frame Generation tab exposes a displayed FPS limit (40-480, default 120) that applies during gameplay, menus and loading screens. Settings
retain `Upscaling.antiLag2FPSLimit` as half the displayed cap (20-240,
default 60) for configuration compatibility. The rendered cap is half the
displayed cap only while FSR frame generation is active, using its most recently
reported state; otherwise it equals the displayed cap. Frame-generation
transitions can therefore take one input poll to change pacing mode.
The existing display refresh-rate cap remains an additional ceiling.

On the FSR DX12 path Anti-Lag runs first, followed by the residual pre-input
timer on every frame, even if the driver reports success. Native D3D11 uses
the same timer without the driver API. NVIDIA DLSS-G retains its own Reflex
pacing. The existing `setGameplayFPSLimit` devbench command name is retained
for compatibility; it now sets the all-state displayed cap, accepts even
`displayedFPS` from 40 to 480, and does not save automatically.

For runtime acceptance, vary the cap during gameplay, the main menu, paused
menus, dialogue and loading screens, with FSR frame generation enabled and
disabled. Verify the displayed cap when menu frame generation is disabled,
and test native D3D11 and an unavailable/failed Anti-Lag driver path. An SDK
success result or timer wait alone does not establish latency reduction or
displayed-frame behavior. Skyrim launches remain user-controlled.
