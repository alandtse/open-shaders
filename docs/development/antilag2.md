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

The slider sets actual rendered FPS (20-240, default 60) in gameplay,
menus and loading screens. `Upscaling.antiLag2FPSLimit` stores that value
directly. The note below the slider shows estimated displayed FPS from the
active FG multiplier: 60 rendered FPS means up to 120 displayed FPS with
FSR 2x generation, or 60 displayed FPS without generation. It is an estimate,
not a measurement. Display refresh-rate limiting is an additional ceiling.

Anti-Lag runs before input when available, then the CPU timer enforces only
the remaining frame budget in every game state. Native D3D11 uses the same
timer without the driver API. `timerBackstop` names this CPU pacing safeguard;
it is not a GPU fence wait or a frame-generation switch. DLSS-G retains its
own Reflex pacing.

The existing `setGameplayFPSLimit` devbench command now accepts
`renderedFPS` from 20 to 240. For compatibility, even `displayedFPS` from
40 to 480 still sets a rendered cap of `displayedFPS / 2`. No save occurs.

Menu frame generation follows alandtse/dev's original resource preparation,
UI routing and game-time delta behavior. The rendered FPS limiter remains
active independently of whether menu frame generation runs.

For acceptance, test the cap in gameplay, main/paused menus, dialogue and
loading with menu FG on and off, and verify UI/HDR composition. Check native
D3D11 and driver failure fallback separately. The user launches Skyrim;
provider success alone does not establish generated-frame presentation.
