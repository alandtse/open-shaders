# DX12 disabled-path contract

The native DX12 master switch is a boot-latched opt-in. When disabled, upstream SCM retains authority over light selection, engine mutations, caster classification, shadow submission and cache reuse. Do not perform native light metadata extraction, geometry observation, ownership queries, worker startup or offload-only resource submission on that path.

Raster comparison preserves upstream raster rendering and admits no new native preparation, acceleration or shadow dispatch. Shadow offloading may use the native extension of the synchronous SCM caster walk. Its activation policy is separate from upstream DX12 upscaling and frame generation.

Revoke native ownership before redraw budgeting when switching away from offloading; invalidate affected raster tiles and schedule recovery. Never invalidate upstream raster caches in a session that has not acquired native ownership.

Validate with clean upstream and integrated builds at identical settings, capped and uncapped, using DragonsreachTorchThirdPerson. Compare SCM invocation counts, inclusive/exclusive timings, cache behavior and rendered shadows; use a separate native on/off A/B. Disabled runs must not start native workers or submit native passes. A small activation branch is permissible; measurable recurring native work is not.

Coverage correctness for enabled offloading is a separate requirement: an emitter match alone does not prove caster coverage. This contract does not establish complete native coverage or guarantee an immediate fallback during every live mode transition.

Raster cache provenance travels with atlas tiles, including parked and migrated records. Native ownership retirement invalidates only native-filtered depth and leaves ordinary upstream cached tiles alone. Render submission does not independently change the provenance established during accumulation. The original redraw budget remains in charge of recovery; live mode transitions and partial native coverage still require visual validation.

Native render hooks opt in through the boot-latched master policy. Native grid and scene resets are conditional on actual session activity; a never-enabled session performs no native reset work. On runtime disable the existing workers may finish cancellation and GPU retirement, but no new native preparation is admitted. Do not synchronously wait for GPU retirement on the render thread.

Previously native-filtered lights are checked before redraw budgeting, including when their emitter match or scene scope is lost. Native capture packets carry the originating world scope through publication; a disabled session rejects worker publications through an atomic gate. This does not replace the future per-caster coverage contract.

## Acceleration CPU scheduling and GPU retirement

The native acceleration coordinator submits short serialized CPU tasks through the existing shader compilation threadpool. It owns neither a persistent thread nor a second pool. A lazily created runtime task gate shares the existing dispatch budget, admits at most one runtime job globally, gives render-demanded shader tasks priority, and survives shader-cache generation clears. The gate closes on terminal shutdown. If compilation fills all available slots, acceleration admission is retried by active frame callbacks rather than blocking a shader or render thread.

GPU command submission remains on the native queue. Fence events request CPU continuations through the same gate; active frames retry denied admission and provide a non-blocking readiness fallback if event registration fails. Reset and disable invalidate publication without waiting for submitted GPU work. Producer and consumer resources retire through one-shot OS fence waits, including after a session closes. A failed queue-tail signal retains untracked GPU work until device removal; if no safe completion registration can be made, the exceptional owner is quarantined until process exit.

Geometry and light preparation uses a single lazy, in-process ScenePrepWorker.
The render-side capture obtains engine references and copies bounded rigid
mesh slices; the worker decodes owned data. References without loaded 3D
stay in raster: loading models off an engine thread crashes. Publications are immutable and carry
partial-coverage diagnostics. No external preparation application, IPC,
plugin indexing or persistent scene cache is used.

Main-menu preparation is idle. Loading preparation requires the actual
attached player cell and subcell to agree with the queued destination;
otherwise preparation begins on arrival. Loading tasks use elevated
thread priority only while their generation is admitted. Gameplay tasks
run below normal priority. Cancellation invalidates queued and completed
work and revokes loading priority without waiting on the render thread.

Profile capture and worker Tracy zones separately. Validate model database
thread safety and loading contention in Skyrim; attached mesh snapshots
avoid asynchronous reads of renderer-owned buffers.

### Validation

Run the C++ unit tests with the DX12 runtime-task and fence tags. The pure policy and gate tests do not require a live graphics device. Windows runtime validation must additionally cover:

- Cold boot with native DX12 OFF: no runtime gate initialization, native CPU task, fence registration or native rendering pass. Compare clean upstream and integrated runs using the existing Dragonsreach procedure.
- DX12 ON with a warm cache, then background shader compilation: check one admitted acceleration task at a time, demanded shader priority, TLAS publication latency and baseline rendering.
- Rapid grid changes, loading screens, DX12 disable and re-enable, and terminal shutdown with work pending: no stale publication, blocked render thread or prematurely released GPU resources.
- Failed producer signal, device removal and event registration failure: verify recovery or safe retention without detached polling threads.

Use Tracy zones `CS::RuntimePoolTask`, `CS::RuntimePoolQueueWaitUs`, `DX12::Acceleration::Task`, `CollectInputs`, `BuildBLAS`, `BuildTLAS`, `Submit` and `Retire` (the latter zones are prefixed with `DX12::Acceleration::`). Compare CPU active time, P99 frametime, thread count and TLAS availability against the previous implementation.

## Full RT light publications

Full RT snapshots engine light membership on the render thread at a configurable
frame interval (default 8). Stationary metadata is acquired once per observed
identity, with membership and visibility refreshed at subsequent observations.
The render thread passes value records and immutable resource-grid snapshots
to a short native light task through the existing runtime gate.

The worker collects prepared emitters only when tile publications change,
filters exact placed-reference duplicates, applies the existing unification
and additive-emission policy, and packs the unchanged two-float4 shader ABI.
Only changed inventories receive a new revision and a default-heap GPU buffer.
Rendering reuses the previous completed same-visit publication during an
update; an initial visit has no native shadow submission until the first
upload completes. Each shadowmask slot retains its publication until the
tracing fence retires. The upload owner separately retains copy resources
through the upload fence. Cell, scene, mode and runtime resets detach stale
worker state; no task holds engine pointers or the feature itself.

The observation interval bounds normal membership-update latency, not worker
or GPU completion time. No stability filtering or permanent light bake is
performed. Offloading keeps the existing synchronous matched-light path.

Profile the zones `DX12::Lights::Observe`, `DX12::Lights::Task`,
`DX12::Lights::Prepared`, `DX12::Lights::Consolidate` and
`DX12::Lights::Upload`. In a stationary cell, verify that revision and
upload counts stop increasing once light membership settles. Exercise
light removal, setting changes, loading screens, same-world cell changes,
mode switches and shader-pool contention. Compare the diagnostic overlay
and shadow masks against the prior Full RT path, and repeat the baseline
offloading/disabled-path checks.

### Native preparation coverage and reuse

Prepared geometry and render-observed geometry remain distinct producers.
The latter admits stable rigid draws; it cannot certify never-observed
casters. The published TLAS reports both source counts, deduplication,
triangles and AS allocation widths, not a percentage of complete coverage.
Preparation rejects controllers, unsupported base/geometry types, skinning,
alpha-dependent shapes and unavailable CPU data. Native vertex descriptor
accessors match the render observation path; the other admission rules
are intentionally unchanged. Unknown bounds remain scope-level evidence,
not tile-local omissions. Whole-cell consolidation requires known spatial
bounds independently of extracted shape counts.

When a complete, bounded whole-interior enumeration cannot consolidate,
reuse its immutable accepted shape/light records for the same center and
size for at most two seconds. Partition exact shape bounds on the worker;
never retain engine pointers or claim rejected geometry is covered. Scope
rejections remain separate. Discard on cancellation, center/size/scope
change, loading handover or exhausted requests. Budget-truncated results
fall back to normal capture. Capture timings exclude reused requests.

Runtime validation: in the same interior and camera route, record grid
source counts and the published TLAS revision with FG on and off. Compare
shadows from an occluder initially outside the camera frustum against a
raster reference and then turn toward it and back. Verify prepared casters
are present without prior observation; observed-only casters may appear
only after render-pass observation. Repeat with an interior larger than
the consolidation footprint, an A-to-B-to-A transition and exterior
streaming. Compare Tracy capture/worker/acceleration timings and P99 to
the pre-change build. No complete off-screen coverage is implied by a
valid publication or a functioning shadow mask.

## Corrective lifecycle and baseline validation

A terminal preparation worker failure rejects admission and resolves any pending
tile as failed; recreation is bounded and the terminal status is published.
Scene-transition cancellation resets the recovery budget. Thread priority and
the active/predictive scheduling order are unchanged.

Shared CPU fence waits reject device removal and untracked values as successful
completion. Native DLSS uses nonblocking completion checks during resource and
feature changes. Pending resets are serviced on the rendering thread; shared
textures and command contexts still in flight at destruction use shared
asynchronous retirement. Failed recovery signals preserve untracked ownership
rather than assuming completion.

The SCM baseline classifier compiles without native observation or suppression
branches. The shared engine cull hook still selects the already-resolved caster
mode once per invocation; compare its exclusive timing in Tracy before claiming
performance equivalence.

Additional Windows validation:

- Inject worker startup/terminal failures and exhaust the recovery budget, then
  reset the scene and verify preparation resumes without stranded pending tiles.
- Inject device removal before a fence wait and during event completion. Inject
  both submission and rescue signal failures on Native DLSS.
- Switch DLSS quality, preset, resolution and upscaler while work is pending;
  verify DX11 fallback and safe render-thread SDK resource release.
- Compare DX12 OFF, raster comparison, offloading and both Full RT profiles
  against the audited revision using Dragonsreach at 4K and 1080p, FG on/off.
  Verify dynamic raster ownership and outdoor disable transitions independently.
