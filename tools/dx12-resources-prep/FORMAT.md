# DX12 Resources cell preparation contract

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

## Plugin-scoped interior catalog (Stage B)

Command: DX12ResourcesPrep --list-cells <Data directory> <SelectedPlugin.esm> <Plugins.txt> <output.json>

This command enumerates selected-plugin interior CELL records from disk, including unloaded interiors and overrides. Its JSON result has kind=dx12-resources-plugin-interiors, schemaVersion=1, selectedPlugin, interiorCount, and interiors. Each entry has plugin (original owner), localId, editorId, displayName and relation (defined or override). Failed scans return nonzero exit codes; no atlas is involved.
