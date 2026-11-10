# Native True PBR material authority and DX12 residency

## Authority and identity

The DX12 Native Scene opt-in control named "Experimental: runtime True PBR materials"
now uses runtime True PBR recipes to compile complete 128-byte shader records.
Its existing setting name and default OFF state remain unchanged.

Runtime material properties, shader flags, base/normal/RMAOS/emission paths,
UV transforms, sampler and PBR parameters are authoritative. The offline atlas
is retained for static geometry, vertex attributes and reusable resident DDS
pages. Offline material paths and scalars no longer veto unique live matches.

Runtime matching requires exactly one reference-and-shape identity on each
side. Skinned and alpha-property geometry, ambiguous references and geometry
not verified as an opaque traversal candidate remain excluded. This is
deliberately conservative: the capture hook does not yet prove a native NIF
model/block identity, animated UV state or exact runtime GPU texture contents.

## Runtime record compiler

CompileRuntimeMaterial reads the live immutable recipe and the captured
shader-property observation, resolving runtime texture paths through the
existing verified DX12 residency registry. Its material record includes
runtime base, normal, RMAOS and optional emission bindings; UV and clamp;
roughness and specular; appropriate model-normal and vertex-color flags;
and valid instance emission.

Unsupported advanced effects (glint, projected PBR, displacement, feature
textures, coat/fuzz/subsurface), non-opaque state, absent normal basis and
missing resident DDS bindings do not use fabricated shader approximations.
They retain the previous offline material compiler as a fallback. A live
match alone is not proof that a record has native coverage.

Texture source provenance is SHA-256-verified, but this does not prove that
an archive or loose DDS matches the current renderer-owned DX11 GPU bytes.
Direct DX11-to-DX12 GPU texture sharing/copy remains future work and is
limited to descriptor eligibility diagnostics in this branch.

The existing asynchronous material worker prepares and publishes immutable
bindings after its upload completion fences. Runtime matching and compilation
are not performed by the draw shader or synchronously on the main thread.

## Runtime UI counters

The collapsible Native PBR Diagnostics section reports livePbrMatched as
unique identity matches, and livePbrApplied as the number of complete
runtime-derived shader records accepted. livePbrOfflineFallback indicates
matched objects whose material records used the offline fallback.

New reasons: livePbrUnsupported, livePbrUnsafeOpacity,
livePbrMissingBasis, livePbrMissingBindings, plus existing source missing,
ambiguous and incompatible counters. Loose/archive import statistics and
DXGI memory/upload telemetry remain available.

## Bounded texture acquisition (still opt-in)

Loose DDS acquisition and archive-backed Skyrim resource-stream fallback
remain separate switches, disabled by default. The source acquisition plan
is capped at 8 DDS paths and 32 MiB per load; it uses the existing page
caches, DXGI memory checks and upload retirement fences. Archive sources
are considered only if the loose file is absent. No CPU GPU readback or
renderer texture pointer is borrowed.

## Limitations and test requirements

This runtime-authoritative update has not passed Windows compilation or
in-game testing by ChatGPT. Previous successful screenshots validated the
earlier scalar-only route, not this new complete material compiler. Run a
fresh Release build and then validate normals, opacity, UVs, emissive
objects, applied/fallback counters, source acquisition and frame pacing.
The offline fallback remains the recovery path for unsupported geometry
and native shading features.


## Staged retirement of offline-generated material data

The legacy `cell-textures.json` path is now **optional at the loader API
boundary** (`MaterialSource::usePreparedTextureManifest`, default true).
A worker can instead prepare texture pages from already-matched live True PBR
recipes and bounded loose/archive DDS sources without consulting the offline
texture manifest. The default gameplay configuration remains unchanged:
existing atlas texture pages and offline fallback material records still load.

This is an architectural seam, **not yet removal of the offline generator**.
No UI or production feature automatically selects manifest-free mode.
At current coverage most matched materials still lack supported/resident
runtime shading, and removing legacy pages would leave their fallback
records unusable. We must first establish full source residency, native
feature coverage and renderer fallback behavior, then switch the default
and delete obsolete offline material generation. BLAS/TLAS geometry
preparation is independent of this material deprecation.

## Progressive streaming and manifest-free mode

The native material worker can now make repeated, bounded progress within a
single prepared DXR scene:

1. `RuntimeTextureProgress` persists previously attempted canonical source
   paths per scene and live recipe revision. Rejected or unavailable DDS
   requests are not retried indefinitely within the same revision.
2. The initial worker pass may load the legacy atlas as usual; later passes
   inherit the completed resident DX12 pages, logical texture identities
   and SHA-256 provenance of the previous published material generation.
3. Subsequent passes skip `cell-textures.json`, request up to eight further
   missing runtime DDS paths, and create fresh material tables and descriptors
   using the still-valid geometry/TLAS. A completed worker generation replaces
   the previously published binding set only after its upload fence reports
   completion. Previous bindings remain accessible to in-flight consumers.
4. A minimum 500-ms interval separates successful automatic batches.
   No shader calls the archive reader and no synchronous GPU readback was added.

A separate **Experimental: runtime-only material sources (no atlas manifest)**
setting starts this process without loading the offline **texture manifest**.
It remains OFF by default. This mode requires the live True PBR and loose DDS
switches; the archive fallback remains independently optional. Materials
whose native feature implementation or source texture is missing will
remain unsupported by the native shading pass. Existing prepared static
geometry and vertex attributes are still needed to construct TLAS/BLAS;
the project does not yet support runtime-only scene geometry acquisition.

The diagnostics report cumulative `streamBatches`, `streamAttempted`,
`streamSuccessful`, `streamFailed` and per-batch
`looseDdsPreviouslyAttempted`. Other loose/archive counters reflect the
most recently published binding generation, not the whole streaming lifetime.

### Known acquisition and coverage blockers

The Skyrim runtime screenshot from 2026-11-07 showed eight attempted
archive-backed DDS requests with zero imports and
`Archive DDS resource unavailable`. The streaming scheduler prevents
these failed paths from permanently blocking the other requests; it does
**not** fix the archive-reader failure. The next separate task is to verify
the engine's archive stream acquisition API and/or implement a safely
synchronized live DX11 texture copy path.

The 293 active unsupported True PBR recipes (especially displacement,
glint and projection) also still require actual native shader coverage.
Texture residency alone cannot make these recipes natively renderable.

This stage is source-committed but **not** Windows-compiled or Skyrim-runtime
verified. For testing, first leave runtime-only mode disabled and observe
successive stream batches. Then test it separately and verify that no
new frame-time spikes, device failures, missing geometry, or unexpected
native shading artifacts occur. Restore the previous settings if necessary.
