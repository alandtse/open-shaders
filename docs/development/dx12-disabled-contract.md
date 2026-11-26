# DX12 disabled-path contract

The native DX12 master switch is a boot-latched opt-in. When disabled, upstream SCM retains authority over light selection, engine mutations, caster classification, shadow submission and cache reuse. Do not perform native light metadata extraction, geometry observation, ownership queries, worker startup or offload-only resource submission on that path.

Raster comparison is not master-disabled: it permits geometry preparation but does not grant native shadow ownership. Shadow offloading may use the native extension of the synchronous SCM caster walk. Its activation policy is separate from upstream DX12 upscaling and frame generation.

Revoke native ownership before redraw budgeting when switching away from offloading; invalidate affected raster tiles and schedule recovery. Never invalidate upstream raster caches in a session that has not acquired native ownership.

Validate with clean upstream and integrated builds at identical settings, capped and uncapped, using DragonsreachTorchThirdPerson. Compare SCM invocation counts, inclusive/exclusive timings, cache behavior and rendered shadows; use a separate native on/off A/B. Disabled runs must not start native workers or submit native passes. A small activation branch is permissible; measurable recurring native work is not.

Coverage correctness for enabled offloading is a separate requirement: an emitter match alone does not prove caster coverage. This contract does not establish complete native coverage or guarantee an immediate fallback during every live mode transition.

Raster cache provenance travels with atlas tiles, including parked and migrated records. Native ownership retirement invalidates only native-filtered depth and leaves ordinary upstream cached tiles alone. Render submission does not independently change the provenance established during accumulation. The original redraw budget remains in charge of recovery; live mode transitions and partial native coverage still require visual validation.

Native render hooks opt in through the boot-latched master policy. Native grid and scene resets are conditional on actual session activity; a never-enabled session performs no native reset work. On runtime disable the existing workers may finish cancellation and GPU retirement, but no new native preparation is admitted. Do not synchronously wait for GPU retirement on the render thread.

Previously native-filtered lights are checked before redraw budgeting, including when their emitter match or scene scope is lost. Native capture packets carry the originating world scope through publication; a disabled session rejects worker publications through an atomic gate. This does not replace the future per-caster coverage contract.

## Acceleration CPU scheduling and GPU retirement

The native acceleration coordinator submits short serialized CPU tasks through the existing shader compilation threadpool. It owns neither a persistent thread nor a second pool. A lazily created runtime task gate shares the existing dispatch budget, admits at most one runtime job globally, gives render-demanded shader tasks priority, and survives shader-cache generation clears. The gate closes on terminal shutdown. If compilation fills all available slots, acceleration admission is retried by active frame callbacks rather than blocking a shader or render thread.

GPU command submission remains on the native queue. Fence events request CPU continuations through the same gate; active frames retry denied admission and provide a non-blocking readiness fallback if event registration fails. Reset and disable invalidate publication without waiting for submitted GPU work. Producer and consumer resources retire through one-shot OS fence waits, including after a session closes. A failed queue-tail signal retains untracked GPU work until device removal; if no safe completion registration can be made, the exceptional owner is quarantined until process exit.

The companion process, its worker, and its IPC protocol are outside this change.

### Validation

Run the C++ unit tests with the DX12 runtime-task and fence tags. The pure policy and gate tests do not require a live graphics device. Windows runtime validation must additionally cover:

- Cold boot with native DX12 OFF: no runtime gate initialization, native CPU task, fence registration or native rendering pass. Compare clean upstream and integrated runs using the existing Dragonsreach procedure.
- DX12 ON with a warm cache, then background shader compilation: check one admitted acceleration task at a time, demanded shader priority, TLAS publication latency and baseline rendering.
- Rapid grid changes, loading screens, DX12 disable and re-enable, and terminal shutdown with work pending: no stale publication, blocked render thread or prematurely released GPU resources.
- Failed producer signal, device removal and event registration failure: verify recovery or safe retention without detached polling threads.

Use Tracy zones `CS::RuntimePoolTask`, `CS::RuntimePoolQueueWaitUs`, `DX12::Acceleration::Task`, `CollectInputs`, `BuildBLAS`, `BuildTLAS`, `Submit` and `Retire` (the latter zones are prefixed with `DX12::Acceleration::`). Compare CPU active time, P99 frametime, thread count and TLAS availability against the previous implementation.
