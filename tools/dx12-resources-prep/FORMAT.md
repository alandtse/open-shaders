# DX12 Resources cell preparation contract

## Main-menu PGPatcher reference index

`DX12ResourcesPrep --pg-references <Data> <previous-index.json> <work-index.json>`
reads the deployed `ParallaxGen_Diff.json` and only the NIF paths explicitly
listed there. The index is independent of CELL/worldspace preparation and never
compiles geometry, loads DDS content, or creates GPU resources. Its output has
`kind: "dx12-pg-references"`, `schemaVersion: 1`, `sourceDiffSha256`, counts,
and `meshes`. Each mesh stores its normalized path, expected PG CRC32,
file identity, verification status, SHA-256, and NIF shape references with raw
shader flags and ordered texture paths. Stable shape lookup is the mesh path
plus NIF block index; a changed NIF hash represents a new revision.

The companion reuses entries whose PG CRC32, file length, and write timestamp
are unchanged. NIF verification uses at most four concurrent workers while
preserving sorted output order. Skyrim publishes the validated result atomically at
`DX12-cache/pg-references/index-v1.json`; the immutable in-memory snapshot is
available through `DX12Resources::GetPgReferenceSnapshot()`. Missing or
unverified outputs remain explicitly unresolved. The index is currently limited
to CRC-tracked patched NIFs: PG-generated duplicate meshes not present in the
diff, plugin-only TXST overrides, and PGLite-only runtime patches are **not**
claimed as covered by this producer version.

`cell-preparation.json` identifies this producer contract with
`formatContract: "dx12-resources-cell"` and `formatVersion: 1`. Its `cell`
field is the original plugin-local CELL identity. The companion writes a
cell-specific workspace containing `cell-layout.json`, `cell-meshes.json`,
`cell-textures.json`, `cell-records.json`, and `cell-preparation.json`.

Geometry and shading files are content-addressed by SHA-256. Geometry uses
the existing RSGM version 1 binary layout; shading uses the existing RSGA
version 1 or 2 layout. Their magic values and extensions are unchanged. The
layout document separately records its renderer, shading, material, and room
schema versions. These binary formats are shared with the legacy renderer;
the new workspace is not automatically consumed by it.

The producer records omitted or unresolved sources and does not claim runtime
coverage or acceleration-structure completion. A successful process exit is
not a validation or publication of the resulting scene.

## Geometry and texture-reference ownership

The companion preserves original NIF triangle topology and source shading attributes
(normals, UVs, tangents). It never samples height or normal textures to subdivide,
displace, or otherwise regenerate triangles. Preparation quality `2` is retained
as a compatibility alias for quality `1`, with no texture-driven geometry bake.
The semantic mesh cache key was changed to reject older displaced products.

Each placed shape's `material` member carries `twoSided`, the ordered source
`textures` paths, raw NIF shader flags, alpha, clamp mode, and the UV transform
where NiflySharp exposes all four finite components. `materialSchemaVersion: 3`
identifies these unevaluated source values; older version-2 records are still
accepted for geometry but cannot establish native material readiness.
The `textureReferences` catalog records canonical paths, not copied DDS payloads.
No evaluated roughness, BRDF or DX11 TruePBR material output is baked into the
companion. NativePBR resolves material interpretation and residency at runtime.
Geometry opacity exclusions remain conservative for safe RT traversal.

## Projected-snow receiver geometry

The companion computes `instances[].projectedSnow` from **original RSGM
triangles and the placed shape-to-cell transform**, without consulting DX11
textures, weather, or material evaluation.
`basis: "world-up-face-projection-v1"` identifies this geometric descriptor:

- `candidate`: whether any opaque, upward-facing triangle contributes area;
- `projectedArea`: sum of upward triangle areas projected onto the cell XY plane;
- `surfaceArea`: total transformed triangle surface area;
- `upwardTriangles` / `totalTriangles`: source triangle counts under the same basis.

If geometry cannot be inspected safely, the descriptor instead has a
`basis` beginning with `unavailable:` and zero values. Absence of the member
in an older package means **not marked**, never assumed snowy. Layout and
preparation reports include `projectedSnowCandidateInstances`; the descriptor
also participates in `layoutDigest` so old preparations are not silently reused.

These are **potential receiving areas, not actual snow coverage**. A positive
candidate does not mean precipitation, accumulation, or whitening is present;
vertical and downward-facing triangles produce zero per-hit suitability.
Native Scene validates the descriptor and transfers one flag into the existing
per-instance NativePBR draw binding (bit 3, no extra GPU buffer). NativePBR
calculates each native ray hit's upward-face incidence and exposes it as
`SurfaceMaterial.projectedSnowCoverageCandidate`, forwarded through
`EvaluatedSourceMaterial`. Actual coverage is a separate native terrain input;
the marker alone does not change surface shading. No DX11 data is admitted
into this coverage path.

At runtime, Native Lighting obtains native snow evidence from Skyrim's
landscape snow-material IDs (`Snow`, `SnowStairs`), with no DX11 resource access
and no dependence on weather or precipitation. Terrain probes are cached per TLAS; a
neighboring 256-unit probe may supply half-strength coverage to tolerate
small placement/landscape boundary mismatches. A temporary session-only
`Cover all eligible snow receivers` control in **NativePBR** forces full coverage
on companion-marked receivers, including interiors; the normal terrain-based
mode remains available for A/B.
The current value is packed into unused bits 16–23 of the existing 24-byte
NativePBR draw binding. NativePBR applies **textureless, ray-hit procedural snow**
directly in its PBR evaluation. Noise samples each hit's native material UV,
seeded by draw identity and triangle, rather than world coordinates. Its patch
frequency depends on the scheduled native ray budget (resolution and trace
density), divided by the configurable rays per patch. Snow coarseness scales
the grain frequency, while a second slider blends that setting toward the
underlying material roughness (smooth surfaces yield finer snow). A smooth
four-corner hash varies neutral-white snow albedo and roughness. Crystal
inclusion is hashed from material-space UV cells and hit identity instead of
screen pixels, so the pattern remains attached to geometry while the view
moves. An optional setting follows the post-parallax native material UV;
otherwise snow retains the flat hit UV. Post-parallax mapping changes snow
only when native PBR parallax is enabled and a material supplies height.
The crystal response is
submitted into the native specular lobe only under unoccluded direct light;
shadowed snow does not emit light. Only actual native hits evaluate the effect; no
per-frame hit-count readback or GPU atomics are required. NativePBR owns snow
enable, noise amount/rays-per-patch, both coarseness controls, snow parallax UV,
sparkle amount/density, roughness, and
the temporary coverage override. Tuning reaches SM6.5 via the existing
direct-lighting constant buffer, with no atlas, snow texture residency,
DX11 snow inputs, or per-scene generation. An optional NativePBR
`Ultraviolet affected snow` setting adds a coverage-weighted, very dim
blue-white emission (default 0.012, capped at 0.05), independent of direct
light presence. This is a stylized fluorescence approximation; there is no
UV-spectrum light detection. An 0.08 upward-incidence margin tolerates minor face-angle
discrepancies. This is surface shading, not geometry displacement or a
persistent accumulation simulation. Interior scenes receive coverage only
when the explicit eligible-receiver test is enabled.

## Plugin-scoped interior catalog (Stage B)

Command: DX12ResourcesPrep --list-cells <Data directory> <SelectedPlugin.esm> <Plugins.txt> <output.json>

This command enumerates selected-plugin interior CELL records from disk, including unloaded interiors and overrides. Its JSON result has kind=dx12-resources-plugin-interiors, schemaVersion=1, selectedPlugin, interiorCount, and interiors. Each entry has plugin (original owner), localId, editorId, displayName and relation (defined or override). Failed scans return nonzero exit codes; no atlas is involved.

## Subcell spatial catalog (incremental streaming groundwork)

The companion additionally writes `subcell-index.json` beside the unchanged
full CELL package. `kind=dx12-native-spatial-subcell-index` and
`schemaVersion=1` identify a **metadata-only** index, not nine complete
prepared packages. No subcell BLAS/TLAS is produced by this stage.

Subcell squares are anchored at XY origin zero and use mathematical floor
division (including negative coordinates). Z remains unpartitioned. The
default size is 4096 Skyrim units; accepted sizes are 2048 through 16384,
inclusive, in steps of 1024. Invoke the companion as before, or append
`[quality 0..2] [subcell size]` as the fifth and sixth positional arguments.

Each entry contains `x`, `y`, `instanceIndices`, `lightIndices` and
a content-local `contentDigest`. Indices refer to the unchanged
`cell-layout.json` arrays. One instance may be indexed by several
overlapping subcells, but must be included **once** when the active TLAS
is assembled; geometry bytes are never duplicated in this catalog.
References and hashes remain shared.

The present `coordinateSpace` is CELL-local. Exterior worldspace scope,
on-demand companion requests, per-subcell geometry packages, cross-subcell
BLAS sharing, and GPU residency are subsequent stages. Consumers MUST NOT
interpret this index as evidence that a subcell is renderable.

## On-demand one-subcell preparation

`DX12ResourcesPrep --subcell <Data> <plugin:local-cell-id> <work-output>
<Plugins.txt> <quality 0..2> <size> <x> <y>` creates a **partial,
anchor-selected** subcell package. Its `cell-layout.json` and
`cell-preparation.json` carry the same `subcell = {size,x,y}` scope.
Only references anchored to that square are considered for expensive PG
source reads and NIF compilation. The output remains an ordinary verified
RSGP/RSSP package, and a companion publication may place it under
`DX12-cache/cells/<plugin>/<id>/subcells/<size>/<x>_<y>/`.

This first on-demand producer is not yet complete spatial-coverage discovery:
large instances originating outside the requested square are not found by
anchor selection alone. Do **not** mark such a subcell as authoritative
geometry coverage, and do not activate it for production DXR until bounds
discovery and GPU residency integration have been validated. This path is
separate from the unchanged full-CELL producer and baseline shader compiler.

## Persistent companion mesh cache

The companion stores source-hash/quality/semantic-compiler-keyed compiled
NIF geometry and shading products under the approved
`DX12-cache/work/cells/.shared-mesh-cache` subtree. Metadata and every
restored binary blob are SHA-256-checked before reuse. A poisoned/stale cache
falls back to normal compilation. The cache never contains texture-expanded geometry, regardless of preparation
quality. The cache survives work-package cleanup and is independent of CPU/GPU
subcell residency; it never pins BLAS or DDS textures in GPU memory.

## Exterior WRLD subcell preparation

`DX12ResourcesPrep --worldspace-subcell <Data> <plugin:local-WRLD-ID> <work-output> <Plugins.txt> <quality> <size> <x> <y>` enumerates reference origins in the requested exterior worldspace across CELL records. Mutagen link-cache parent lookup confirms WRLD membership. Manifests carry `scopeType=worldspace` with the worldspace FormKey in the existing `cell` identity field. The game publishes these independent subcell packages beneath `DX12-cache/worldspaces/<plugin>/<WRLD-ID>/subcells/<size>/<x>_<y>/`; interior CELL packages remain unchanged.

**Current coverage remains provisional:** large objects extending from another tile, landscape, foliage, and dynamic meshes require further bounds-aware discovery. This stage must not report complete exterior RT geometry.
