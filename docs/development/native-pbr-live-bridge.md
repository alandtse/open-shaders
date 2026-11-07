# Experimental verified live True PBR scalar bridge

The `DX12 Native Scene` panel now exposes **Experimental: verified live PBR scalars** (default **off**).
This is a first, deliberately limited live bridge, not a live DX11/DX12 texture interop layer.

The render thread captures a revision-matched, immutable True PBR recipe generation and
a bounded set of reference/shape observations. A worker builds a per-instance matching
plan. Candidates are rejected unless the prepared reference/shape is unique, the live
observation is unique, the surface is static and opaque, the material is an ordinary
mesh recipe without unsupported PBR features, and alpha, clamp mode, UV transform and
base/normal/RMAOS/emission texture paths match exactly.

On an accepted match the worker applies **roughness scale** and **specular level**
to the existing 128-byte native material record. If the same instance has verified
finite nonnegative emission metadata, its observed emissive color and multiplier
are applied separately. Texture descriptors, resident DDS
pages, emitter settings, geometry and shader paths remain as prepared by the offline
scene. The material table stays immutable after upload. Different live identities or
revisions cannot share deduplicated records.

A live source match additionally requires corresponding SHA-256-verified DDS
sources to have been successfully published in the native material page registry.
The source index catches normalized-path conflicts and rejects incompatible
observed texture dimensions/mip counts. This verifies **atlas provenance**, not
byte-for-byte identity with the currently bound DX11 GPU texture.

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
`livePbrIncompatible`, `livePbrMissing`, `livePbrApplied`, `livePbrEmissiveApplied`,
and `livePbrSourcesMissing/Ambiguous/Incompatible`. A matched candidate may still be
rejected by the ordinary native material compiler; the existing material readiness
and failure counters remain authoritative.

### Not completed by this bridge

- Live texture byte/digest verification and DX12 GPU residency for non-atlas textures.
- Per-landscape-layer PBR texture paths and native landscape shading.
- Advanced True PBR coat, fuzz, displacement, glint, subsurface and projection.
- Full geometry-to-material provenance, including mesh block identity and dynamic forms.
- Native shader support for all material types and in-game quality/performance validation.

No Windows/Skyrim compilation or live runtime test has been performed by this commit.

### Experimental loose DDS supplement (separate opt-in)

The `Experimental: import missing loose PBR DDS` checkbox is **off by default**.
Enable verified live PBR scalars first; the loose import option only applies to that
already-matched opaque static scene. On a background material worker it selects
up to **8 unique missing DDS paths per scene/revision**, reads at most **32 MiB**
of additional source data, rejects reparse-point files, hashes their bytes with
SHA-256 and validates DDS metadata and DX12 shader-sampling support.

Only **loose files** under `Data/textures` are accepted. BSA/archive resources,
non-DDS sources, complex PBR features and renderer-owned GPU textures are
deliberately not captured. Import uses the **same material upload page cache,
DXGI budget, staging limit, single in-flight upload slot and completion fences**
as the prepared atlas. A file is reopened and digest-checked before GPU upload;
a changed or unavailable file does not become a resident texture.

The per-instance source request must still pass the strict live-material matcher.
Sources missing at a particular load remain on the existing prepared/fallback
path. The planner defers requests past its eight-file cap; it does not loop on
the main render thread trying to fill them. Toggling the option triggers a
fresh asynchronously prepared binding set.

Diagnostics: `looseDdsPlanned`, `looseDdsImported`, `looseDdsRejected`,
`looseDdsDeferred` and `looseDdsBytes`. These are worker load counters;
they are **not** evidence of correct in-game lighting until user validation.

If repeated load attempts exhaust GPU memory, turn off the loose import toggle.
The known-good baseline is the prepared atlas plus the existing native fallback.
