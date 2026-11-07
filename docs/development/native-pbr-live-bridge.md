# Experimental verified live True PBR scalar bridge

The `DX12 Native Scene` panel now exposes **Experimental: verified live PBR scalars** (default **off**).
This is a first, deliberately limited live bridge, not a live DX11/DX12 texture interop layer.

The render thread captures a revision-matched, immutable True PBR recipe generation and
a bounded set of reference/shape observations. A worker builds a per-instance matching
plan. Candidates are rejected unless the prepared reference/shape is unique, the live
observation is unique, the surface is static and opaque, the material is an ordinary
mesh recipe without unsupported PBR features, and alpha, clamp mode, UV transform and
base/normal/RMAOS/emission texture paths match exactly.

On an accepted match the worker applies only **roughness scale** and **specular level**
to the existing 128-byte native material record. Texture descriptors, resident DDS
pages, emitter settings, geometry and shader paths remain as prepared by the offline
scene. The material table stays immutable after upload. Different live identities or
revisions cannot share deduplicated records.

Unsupported, ambiguous, dirty, missing, animated, alpha-tested, landscape, or otherwise
incompatible candidates continue through the existing offline atlas material route.
A matching observation is not proof that a live GPU texture contains the same bits as
the offline asset; use the opt-in mode only for evaluating scalar parity.

The material worker accepts a new recipe revision without blocking the main thread.
New captures are throttled to approximately 120 render frames, and completed
materials are only published after the existing upload-fence policy. In-flight
prepared material work retains its existing resource ownership. As the branch does
not yet revalidate source texture bytes or fully verify renderer material identity,
this bridge deliberately does **not** import live textures, change opacity or alter
advanced PBR features.

Diagnostics under `materialBudget` expose `livePbrMatched`, `livePbrAmbiguous`,
`livePbrIncompatible`, `livePbrMissing`, and `livePbrApplied`. A matched candidate may still be
rejected by the ordinary native material compiler; the existing material readiness
and failure counters remain authoritative.

### Not completed by this bridge

- Live texture byte/digest verification and DX12 GPU residency for non-atlas textures.
- Per-landscape-layer PBR texture paths and native landscape shading.
- Advanced True PBR coat, fuzz, displacement, glint, subsurface and projection.
- Full geometry-to-material provenance, including mesh block identity and dynamic forms.
- Native shader support for all material types and in-game quality/performance validation.

No Windows/Skyrim compilation or live runtime test has been performed by this commit.
