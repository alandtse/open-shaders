# AMD Anti-Lag 2

`extern/AntiLag2-SDK/` is a Git submodule of the official
[AMD Anti-Lag 2 SDK](https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK),
pinned to `390aa4a8c8655d0ae6e90079db2c85e103a96da3` (`v2.0.0a`).
Initialize it with `git submodule update --init extern/AntiLag2-SDK`, or
clone Open Shaders with `--recurse-submodules`. CMake reports the initialization
command if the header is missing. The dependency is header-only and MIT licensed;
the upstream DX11 header and sample are present but are not compiled or packaged.

## Upstream audit and local adaptation

Compared on 2026-10-24 against the official repository's HEAD and `v2.0.0a`,
both at the pinned commit above. The previous vendored license matches upstream.
Its README differs only by trailing whitespace. The DX12 header has one
functional change, plus a final newline: local `Update` returns state-setting
errors and updates `m_enabled`/`m_maxFPS` only after a successful driver call.
Upstream caches those fields before calling the driver and ignores the returned
state-setting error. A rejected toggle or FPS change can therefore appear applied
and suppress retries, even if the subsequent delay call succeeds.

The submodule stays unmodified. `src/Features/Upscaling/antilag2sdk.h` retains
the checked state transition in project-owned code, then calls upstream `Update`
for its delay and HRESULT normalization. This adapter relies on the pinned SDK's
`APIData_v1`, interface and context layout; review it and run the Anti-Lag tests
when updating the submodule. FSR continues to receive the original SDK context,
so its private-data ABI is unchanged.

## Conditional session integration

Anti-Lag 2 is automatic when the existing startup path selects an available
FSR Frame Generation proxy and the existing SDK initialization returns `S_OK`.
No separate hardware probe, device or vendor-detection system is introduced.

`DX12SwapChain::CreateSwapChain` runs the SDK check immediately after obtaining
the existing presentation device, before creating the FSR swap chain. Generic
DX12 device creation and the direct DLSS-G swap-chain path do not call Anti-Lag.
A failed check releases any returned interface and retains no context.

After FSR interop resources have been created, a successful check permits
`Hooks::InstallBeforeInputPoll()` to attach the optional engine-entry detour.
The general `Hooks::Install()` does not attach it. Hook failure releases the
context and selects the ordinary proxy. Ineligible sessions therefore do not
intercept input or invoke the added feature callbacks.

Eligible sessions receive `AntiLag2SwapChainProxy`, which uses
`PresentImpl<true>`. Ordinary FSR and DLSS-G sessions use the existing proxy
and `PresentImpl<false>`; `if constexpr` removes the Anti-Lag private-data and
end-marker calls from that presentation path. There are no per-frame
availability checks or no-op Anti-Lag notification callbacks on that path.

The installed input callback always requests Anti-Lag latency reduction with
zero FPS limit. There is no independent enable setting, checkbox or command.
The temporary testing toggle, SDK limiter, status query and per-call diagnostic
logging have been removed. Old saved Anti-Lag settings have no runtime effect
and are not serialized. There is no CPU fallback timer.

Frame Generation > Current session shows `Anti-Lag 2: Active` or
`Anti-Lag 2: Unavailable` directly below `Method: AMD FSR`, only while FG is
selected and the session uses FSR. Active requires an installed context and
an accepted enabled driver state. Other backend views do not show this line.
The same state is available as `antiLag2Active` in FSR-session devbench diagnostics.

Eligibility is established for the selected session backend. Editing the
restart-gated FG/backend selection does not install or remove this integration
mid-session. Temporary menu/loading suspension is not an eligibility input.

The FSR context handshake keeps the upstream context pointer and enabled-state
layout. The rendering-end marker remains after command submission and before
swap-chain Present. Shutdown first drains pending FSR presents, publishes the
disabled handshake, then releases the interface. If draining fails, both the
context allocation and interface remain alive for process cleanup, avoiding
a dangling context pointer in a queued presentation. Ineligible teardown
makes no SDK or FSR-drain calls.

## Validation boundaries

The existing display-refresh limiter, frame-generation pacing, VSync and Reflex
behavior are preserved. See [the dev comparison](antilag2-dev-audit.md).
The upstream SDK submodule and checked-state adapter are unchanged by the
conditional-installation change; the FPS limit is fixed at zero.

Targeted tests exercise production initialization, interop selection, update,
optional-hook installation and teardown with mocks, plus the real upstream
SDK interface for checked state changes and zero FPS limits. Source checks
cover FSR-only initialization, removal of unconditional input installation,
compile-time presentation selection and notification ordering.

A Release build checks integration compilation. Runtime acceptance remains
user-controlled, including supported FSR-FG, unsupported FSR-FG, FG-off and
DLSS-G paths. No Skyrim launch or deployment is performed by this change.
Initialization success alone does not establish measured latency improvement.

## Production logging

Initialization reports its HRESULT and availability once at FSR startup.
Update, private-data and end-marker failures are reported once per category;
shutdown-drain and hook-installation failures remain visible. Successful
per-frame calls, skipped markers and testing-session status are not logged.
Conditional installation and notification behavior are unchanged.
