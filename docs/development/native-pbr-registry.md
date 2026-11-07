# Native PBR registry — stage 1

The live True PBR material is the authoritative source of material metadata.
`NativePBR::Registry` is a thread-safe, CPU-only boundary for copied
metadata. Its exported snapshots contain no Skyrim pointers, COM resources,
D3D11 textures or GPU handles, and are safe to hand to an asynchronous native
material-preparation worker.

A registry identity belongs to one live material instance. The render thread
publishes on first observed PBR draw and after mutation; material destruction
removes the identity so reused addresses cannot inherit stale metadata.
Each modified material receives a new revision. The registry does not
implicitly upload, mirror, share or bind textures.

Coverage in the first integration is limited to standard PBR mesh metadata,
standard material texture paths and landscape scalar/feature metadata.
Landscape texture paths and resource residency require later explicit
handling; consumers must respect `texturePathsComplete`.
Packed RMAOS and optional layers remain metadata, not DX12 shader inputs.

The existing raster material shaders, native light/shadow producers, composite,
FSR and scene atlas remain unchanged. A later bridge stage may map these
snapshots to material tables and texture residency under a GPU budget.
