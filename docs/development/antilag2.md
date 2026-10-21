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

The Frame Generation tab exposes the gameplay limit as displayed FPS with
FSR's fixed 2x multiplier (40-480, default 120). Settings store rendered
FPS in `Upscaling.antiLag2FPSLimit` (20-240, default 60). The existing display
refresh-rate cap remains an additional ceiling. Menus and loading use
60 rendered FPS. On the FSR DX12 path Anti-Lag runs first; scene frames use
the residual pre-input timer, and unsupported or failing driver
APIs fall back to that timer. NVIDIA DLSS-G retains its own Reflex pacing.

Devbench command under Upscaling: `setGameplayFPSLimit` accepts even
`displayedFPS` from 40 to 480. The generic feature get/set interface exposes
`antiLag2FPSLimit` in rendered FPS. Commands change live settings without
saving automatically.

For runtime acceptance, test the gameplay FPS control at multiple values
with FSR frame generation enabled and disabled. Verify the displayed FPS
cap in both modes: rendered FPS is half the cap with FSR's fixed 2x
multiplier, and equals the cap without frame generation.
Check an unavailable/failed Anti-Lag path as well as a supported AMD
driver. An SDK success result or a timer wait alone does not establish
latency reduction or displayed-frame behavior.
