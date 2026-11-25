# DX12 disabled-path contract

The native DX12 master switch is a boot-latched opt-in. When disabled, upstream SCM retains authority over light selection, engine mutations, caster classification, shadow submission and cache reuse. Do not perform native light metadata extraction, geometry observation, ownership queries, worker startup or offload-only resource submission on that path.

Raster comparison is not master-disabled: it permits geometry preparation but does not grant native shadow ownership. Shadow offloading may use the native extension of the synchronous SCM caster walk. Its activation policy is separate from upstream DX12 upscaling and frame generation.

Revoke native ownership before redraw budgeting when switching away from offloading; invalidate affected raster tiles and schedule recovery. Never invalidate upstream raster caches in a session that has not acquired native ownership.

Validate with clean upstream and integrated builds at identical settings, capped and uncapped, using DragonsreachTorchThirdPerson. Compare SCM invocation counts, inclusive/exclusive timings, cache behavior and rendered shadows; use a separate native on/off A/B. Disabled runs must not start native workers or submit native passes. A small activation branch is permissible; measurable recurring native work is not.

Coverage correctness for enabled offloading is a separate requirement: an emitter match alone does not prove caster coverage. This contract does not establish complete native coverage or guarantee an immediate fallback during every live mode transition.

Raster cache provenance travels with atlas tiles, including parked and migrated records. Native ownership retirement invalidates only native-filtered depth and leaves ordinary upstream cached tiles alone. Render submission does not independently change the provenance established during accumulation. The original redraw budget remains in charge of recovery; live mode transitions and partial native coverage still require visual validation.

Native render hooks opt in through the boot-latched master policy. Native grid and scene resets are conditional on actual session activity; a never-enabled session performs no native reset work. On runtime disable the existing workers may finish cancellation and GPU retirement, but no new native preparation is admitted. Do not synchronously wait for GPU retirement on the render thread.
