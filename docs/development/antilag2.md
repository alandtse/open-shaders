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

The controls remain in the Frame Generation tab:

-   **Anti-Lag 2** is the master switch, on by default. Off disables the driver
    integration and our limiter; subordinate controls remain visible but disabled.
-   **FPS limiter** is on by default. Off clears both the SDK FPS cap and the
    CPU timer while leaving Anti-Lag latency management enabled.
-   **Apply limiter** selects **Gameplay only** or **Always** (default). Gameplay
    requires available UI state, an unpaused game and no main, loading or dialogue
    menu. The selected scope affects the FPS cap, not Anti-Lag's enable state.
-   **Rendered FPS limit** stores actual rendered FPS (20-240, default 60).
    The note estimates displayed FPS from active FG: 60 rendered FPS means up
    to 120 displayed FPS with FSR 2x FG, or 60 without FG. It reports disabled or
    suspended limiting instead when appropriate; it is not an FPS measurement.

The live settings are `Upscaling.antiLag2Enabled`,
`antiLag2FPSLimiterEnabled`, `antiLag2FPSLimiterGameplayOnly` and
`antiLag2FPSLimit`. Missing keys preserve the defaults above. No restart is
required. The existing display refresh cap and other independent limiters
remain separate ceilings.

Anti-Lag runs before input when enabled and available. The CPU timer supplies
only the remaining frame budget while the limiter applies, including on native
D3D11 without the driver API. Disabling or suspending the cap resets its timer
deadline; the SDK receives zero FPS to disable its explicit limiter. Master-off
also explicitly disables the SDK. No context is destroyed during a live toggle.
`timerBackstop` is CPU pacing, not a GPU fence wait or an FG switch. DLSS-G
retains its own Reflex pacing and bypasses this integration.

Devbench `setAntiLag2` accepts optional boolean `enabled`, `fpsLimiterEnabled`
and `gameplayOnly` fields. `setGameplayFPSLimit` accepts `renderedFPS` from
20 to 240; legacy even `displayedFPS` from 40 to 480 still sets a rendered cap
of `displayedFPS / 2`. Commands apply live without automatically saving.

Menu frame generation follows alandtse/dev's original resource preparation,
UI routing and game-time delta behavior. The rendered FPS limiter remains
active independently of whether menu frame generation runs.

For acceptance, test each toggle and scope in gameplay, main/paused menus,
dialogue and loading. Verify master-off and limiter-off remove only our cap,
then re-enable and confirm pacing resumes without a stale timer deadline.
Check old settings files retain on/on/Always and the saved rendered limit. Check native
D3D11 and driver failure fallback separately. The user launches Skyrim;
provider success alone does not establish generated-frame presentation.
