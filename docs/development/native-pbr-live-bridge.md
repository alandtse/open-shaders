# Native True PBR material authority and DX12 residency

## Authority and identity

The DX12 Native Scene opt-in control named "Experimental: verified live PBR scalars"
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
