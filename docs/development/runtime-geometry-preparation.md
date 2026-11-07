
# Runtime DX12 geometry preparation — experimental preview

This stage begins retiring external geometry preparation without changing the previously validated atlas-backed render path. It adds **opt-in geometry capture, native buffer conversion, background upload and a separate BLAS/TLAS preview**, not a new production TLAS.

## Capture at the Skyrim render hook

Enable **DX12 Native Scene → Experimental: capture runtime static geometry** (default off). The existing BSLightingShaderProperty_GetRenderPasses hook observes reference, shape, world transform and bounds, then copies eligible static BSTriShape CPU vertex and 16-bit index arrays. It does not retain RE::BSGeometry pointers, raw engine memory, or D3D11/COM objects for worker use.

Limits: 256 KiB per mesh, 512 KiB total copied per render frame, 64 MiB total cached CPU bytes and 8192 scene entries. Malformed vertices/indices, non-finite transforms, missing identity, skinned/dynamic/alpha and GPU-only meshes are excluded.

The render observation supports PBR and non-PBR geometry that passes through the existing True PBR render hook, but is not guaranteed to cover every Skyrim draw call. Reference/shape identity is not yet sufficient to prove a unique source NIF block.

## Runtime native preparation

RuntimeGeometryEncoding converts copied bytes into the native positions/32-bit-indices format and optional normal/UV/tangent attribute format. No generated .rsgm/.rsga files are needed to encode eligible runtime meshes. The compressed-normal and UV decoder remains experimental and requires visual validation.

RuntimeGeometryPreparation processes up to four owned meshes per asynchronous worker batch, uploads to independent D3D12 upload-heap resources and publishes immutable prepared generations. A *separate* acceleration worker builds preview BLAS/TLAS with the existing completion fences. Preview geometry allocation is limited to the smaller of 64 MiB or one quarter of the main scene budget.

**No preview geometry is passed to production Native Lighting or the shadow producer.** The existing atlas-backed scene and runtime material path are unchanged.

UI and diagnostics report captured mesh count/bytes, prepared instances/bytes, batch/attempt/failure counts and separate acceleration-preview status.

## Main-menu and loading-screen boundary

Main-menu preparation is possible for a reusable **runtime-generated persistent geometry cache**, but Skyrim does not know the destination cell before a save is selected. Persistent cache writes, content provenance verification and main-menu GPU prewarming are not yet implemented. This stage performs runtime preparation of observed geometry, not cold-start menu prewarming.

The existing transition policy still interrupts scene publication during loading. A future handoff must preserve in-flight work and publish a complete, coherent geometry+material+light generation when entering the cell.

## Validation and outstanding work

These commits have no Windows Release compilation, CI or Skyrim runtime verification. Build first; if successful, enable capture in a static interior and inspect the preview counters while checking that the existing active renderer is unchanged.

Next work: engine geometry model/NIF block identity, raw CPU buffer fidelity, safe GPU-only source copy, persistent runtime cache, main-menu prewarm, loading-screen handoff, dynamic/skinned support, runtime light acquisition, and atomic cutover away from offline prepared geometry.


## Persistent runtime geometry cache and main-menu prewarm

A second, separately opt-in setting, **Experimental: persist and menu-prewarm
runtime geometry**, is now available below the static capture toggle.
It is **off by default** and requires live static geometry capture.
It has no effect on the existing production TLAS or runtime material source
configuration.

When enabled, the CPU-only geometry cache worker saves up to four validated
runtime-observed mesh packets per 250 ms while gameplay continues. Files use
the versioned **RGCP v1** byte format and a content-addressed SHA-256 name
with a distinct ".rgc" extension in:

\`Data/SKSE/Plugins/CommunityShaders/NativePBRCache/geometry\`

The 256-MiB disk limit uses the existing hardened native cache writer:
temporary files, hash checks, atomic publication, conservative eviction,
and rejection of file reparse points. No disk access occurs inside
the geometry render hook.

At a main-menu state with no current cell, if EarlyPrepass is being
called, the background worker scans at most 256 cached files, verifies
each SHA-256 digest, decodes the packet and pre-encodes the CPU geometry
buffer. Prewarmed data is capped at 64 MiB and is **not** promoted to
an active scene or BLAS/TLAS at the menu. Cache writes from the last
gameplay session can be picked up on a subsequent menu visit.

After entering a scene, the normal render hook must **recapture the live
geometry first**. A prewarmed payload is reused by the async geometry
preparation worker **only if its whole saved packet** (reference,
shape, transform, bounds, vertex/index layout and exact bytes) hashes
to the same SHA-256 digest as the newly observed packet. Thus no
reference/shape alias or stale geometry file grants publication rights.
A matching prewarm can avoid the CPU geometry conversion work; it does
not bypass the existing DX12 upload, BLAS/TLAS build, fence or scene
identity requirements.

The UI exposes cache save/rejection counts, main-menu verified/accepted
files and bytes, worker activity and cache-assisted geometry encodes.
The \`runtimeGeometry.diskCache\` diagnostics section exposes these
same counters.

### Boundaries

This is a **CPU geometry cache**, not full main-menu GPU prebuilding of a
destination scene. The actual cell is unknown at a cold main menu, and
unverified saved instance geometry cannot safely populate a TLAS.
GPU-only geometry, animated/skinned objects, materials requiring
unsupported native features, and source NIF model/block identity are
still unresolved. The legacy prepared geometry path remains the
production fallback.

The code is committed but a fresh Windows Release build and Skyrim
runtime test are still required. First test the existing geometry
capture, then enable the cache option, verify its saved count,
return to the main menu and load the same cell. A nonzero verified
prewarm count and subsequent cache-assisted encode count demonstrate
reuse. The BLAS/TLAS preview remains independent and should
still eventually report Ready, as observed before this cache work.
