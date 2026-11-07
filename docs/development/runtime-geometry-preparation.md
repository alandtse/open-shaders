
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
