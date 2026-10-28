# AMD Anti-Lag 2

Anti-Lag 2 automatically accompanies FSR Frame Generation when AMD's driver
interface is available. It reduces latency by delaying CPU work immediately
before Skyrim samples input. There is no independent enable switch or FPS cap.

## Availability

The existing FSR startup path initializes Anti-Lag on its presentation device.
Only `S_OK` enables the integration. Unsupported hardware or drivers retain the
ordinary FSR path without Anti-Lag input hooks or presentation notifications.
Native D3D11 and DLSS-G do not initialize Anti-Lag; DLSS-G retains Reflex pacing.

Frame Generation and backend selection require a restart. Temporary suspension
of generation in menus or loading screens does not change session eligibility.
Under **Frame Generation > Current session**, the FSR method displays
**Anti-Lag 2: Active** or **Unavailable**. Active requires an installed context
and an accepted enabled driver state. Other method views omit the status.
FSR-session devbench diagnostics expose the same state as `antiLag2Active`.

## Integration

The header-only [AMD SDK](https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK)
is pinned as `extern/AntiLag2-SDK` at `v2.0.0a`. Initialize it with
`git submodule update --init extern/AntiLag2-SDK` before configuring a build.

`DX12SwapChain` owns initialization, optional input-hook installation and
presentation. The project adapter in `Upscaling/antilag2sdk.h` checks driver
state changes before caching them so rejected changes remain retryable.
The SDK FPS limit stays zero; existing display-refresh and Reflex limits remain.

Eligible sessions use a separate presentation specialization. It publishes the
SDK context through FSR's swap-chain private-data handshake and marks rendering
completion after command submission, before Present. Ordinary sessions compile
out these notifications. Shutdown drains pending presents before clearing the
handshake and releasing the interface. A failed drain retains the context and
interface until process exit to protect pending presentation references.

## Validation

Catch2 tests in `tests/cpp/test_antilag2.cpp` run through CI's `CppUtilTests`.
They cover SDK state errors and retries, zero FPS limits, initialization and
hook failure cleanup, and shutdown lifetime.

Runtime checks require supported FSR-FG, unsupported FSR-FG, FG-off and DLSS-G
sessions. Initialization logs its HRESULT and availability; runtime failure
messages are limited to the first occurrence per category. Successful calls
produce no per-frame logging. Initialization and API success establish interface
operation, not measured latency reduction or generated-frame presentation.
