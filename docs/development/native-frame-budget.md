# Native DXGI frame budget and in-flight slots

The earlier `open-RXRT` native lighting budget consulted the active
`ID3D12Device::GetAdapterLuid()` adapter via `IDXGIAdapter3::QueryVideoMemoryInfo`.
The new `VideoMemoryBudget.h` restores that policy for frame producers:
the OS LOCAL-segment budget and current usage include game, upscaler,
scene and SDK allocations. An adaptive safety reserve of 256 MiB to 1 GiB
(1/16 of the budget, clamped to those limits) is retained.

Frame-pool budget = tracked existing pool allocations + max(0,
LOCAL budget - LOCAL usage - reserve); optional manual caps limit the
result. Reuse never bypasses the producer/consumer fence checks. Failed
budget queries use a conservative 512 MiB fallback unless explicitly capped.

The raster snapshot, early native shadow and Native Lighting stages can
now lazily acquire one of **four** fenced frame slots instead of two.
Native Composite also has four slots and checks incremental allocations
against the DXGI headroom. Shadow binding descriptor table offsets were
expanded to match four independently in-flight masks.

`DX12 Native Scene` now exposes an independent **Native frame-pool**
automatic/manual budget (0 = DXGI auto). Its existing scene/material
budget remains unchanged. Native Lighting defaults to automatic DXGI
frame budgeting and retains a manual ceiling for experiments.
Raster, native shadow and lighting diagnostics expose slot counts,
busy deferrals, OS-local usage and current effective frame budgets.

These changes target alternating-frame native RT loss under deep GPU
pipelines and diagnostic readback retention. Four slots cannot guarantee
an RT submission on every frame if GPU work spans more than four frames
or when headroom is exhausted. The fallback intentionally preserves raster
rather than overwriting resources in use or presenting stale lighting.

Source-level review only: full Visual Studio compilation, actual DXGI
headroom, frame-by-frame GPU fence timing, FPS, and visible Skyrim output
must be verified on the owner's system.
