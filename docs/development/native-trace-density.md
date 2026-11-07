# Full-resolution native RT trace-density modes

Implemented for the **independent DX12 Native Lighting** and **DX12 Native
Scene shadow-mask producer** on `open-RT`. These are trace-density controls,
not reduced-size textures or reconstructed resolution modes.

## Runtime controls

Two independent persisted settings, both defaulting to **Full**:

- `Native Lighting → Native lighting trace density`: Full, Two-thirds, Half.
  The same option is available through `configure.traceDensityMode` (0–2).
- `DX12 Native Scene → Early DX12 shadow-mask controls → Independent shadow
  trace density`: Full, Two-thirds, Half. The raw mask's legacy DevBench
  `setShadowLayer.traceDensityMode` argument (0–2) controls it independently.
  This is not the Native Composite shadow-strength setting.

The independent shadow contribution, softness, blend, receiver tolerance and
before/after-lighting composition remain in **Native Composite**. Existing
per-light stochastic emitter sampling is completely separate from these
per-pixel trace-density modes.

## Sampling contract

Each output texture retains **100% of the active dynamic render resolution**
(identical to the original Full mode). Every mode dispatches over the same
full-size output grid. The sampling predicate is evaluated inside the HLSL
shader before primary ray traversal:

| Mode | Pixels running RT ray work | Pattern |
| --- | --- | --- |
| Full (0) | 100% | Every full-resolution pixel |
| Two-thirds (1) | ~66.7% | One pixel omitted per complete horizontal triplet; per-group pixel selected by a frame-varying GPU hash |
| Half (2) | 50% | Parity `(x ^ y ^ frame ^ featureSalt) & 1`; A/B alternates every frame |

A fresh hash is evaluated for every frame in Two-thirds mode. It is
not guaranteed that all individual pixels receive rays across a fixed number
of frames. The lighting and shadow producers use different salts, so their
selection patterns and their mode controls are independent. The salts
have opposite parity, making Half's initial A/B checkerboards
complementary rather than accidentally synchronized.

No output texture dimensions, dynamic-resolution viewport, game raster
buffer, prepared-scene resource or RR context size is changed.

## Raw samples and compositor

Omitted samples are deliberately **not** reconstructed:

- Native Lighting zeros its raw diffuse/specular and guides before the density
  gate. A skipped pixel has direct/effective validity 0 and a dedicated
  `RejectionReason` high bit (`0x10000000`). The existing GPU counter 0 tracks selected
  candidates; on GPU completion, the omitted count is derived from the
  full-size pixel count minus that existing counter, with no extra atomic.
- The shadow producer emits `ShadowMask = (0, -1, 0, 0)` for omitted pixels.
  The mask validity is `-1` rather than invalid `0` or traced `1`.
  The mask continues to hold visibility, distance and instance ID for
  pixels that were actually evaluated.
- Native Composite continues to accept only valid current-frame samples.
  It does not manufacture radiance, fill holes, promote invalid shadow
  samples, substitute history or override the raster background at skipped
  pixels. **Sparse RT contributions remain visible against raster.**
- The existing optional spatial **shadow softness** in Native Composite is
  a post-raw effect on already accepted shadow pixels only. Skipped pixels
  remain invalid and do not become evaluated samples.

No RR execution, temporal accumulation, upsampling, history readback,
spatial reconstruction or extra checkerboarding toggle is introduced.

## ABI, ownership and safety

- Native Lighting reuses the previous `LabPadding` HLSL constant-buffer slot
  for `TraceDensityMode`. The C++ constant remains 272 bytes. Enlarging
  the GPU counter buffer is unchanged: skipped counts are derived from the existing
  candidate counter after readback, so there is no extra per-pixel atomic.
- DX12 Native Scene uses the already-existing `DirectTraceMode` shader
  constant slot to transfer the shadow density mode. The C++ root constant
  block remains 36 x 32-bit values. The old `NativeTraceSample` gating
  is retired from the independent shadow path so the stochastic emitter
  toggle cannot accidentally affect trace density.
- All produced resources keep their existing frame, scene, device, dimension,
  producer fence and consumer-retention identities.
- Both independent features can be disabled, configured, or changed without
  requiring the other feature to run.
- The shaders use the common `NativeTraceDensity.hlsli`, included through
  the existing native DXC include roots; Native Lighting's DXIL build
  depends explicitly on that helper, while the shadow DXIL build uses the
  existing HLSLI dependency glob.

## Owner's runtime verification matrix

1. Confirm **Full / Full** matches the earlier rendering and submission
   behavior, including the previously tested shadow defaults.
2. Test Native Lighting at **Two-thirds** with independent shadows still
   **Full**; check that native direct raw pixels are omitted and the shadow
   mask remains unaffected.
3. Test Native Lighting at **Half** while shadow is **Two-thirds**; verify
   alternating direct checkerboard parity and unrelated raw shadow holes.
4. Reverse the modes (**Two-thirds lighting / Half shadows**) and inspect
   frame-to-frame shadow parity.
5. Test both at **Half**. They retain independent feature salts; no
   reconstruction is expected to hide their raw sparse outputs.
6. Disable Native Lighting entirely and verify independent shadow
   tracing/consumption still runs.
7. After changing the mode, verify `shadow-layer submissions`,
   `Native Lighting.skippedPixels`, trace status, fixed frame dimensions,
   GPU completion and absence of device removal/slot retention failures.
8. Repeat for odd render dimensions and dynamic-resolution changes, especially
   width or height not divisible by 2 or 3.

Source consistency checks and deterministic JavaScript sampling simulations
passed. Actual Windows DXC, Visual Studio Release build and Skyrim runtime
checks remain **unverified** and are reserved for the owner.


## Independent native shadow reconstruction extension

The shadow selector additionally offers native-pixel One-third (3), Quarter
(4), One-sixth (5), One-ninth (6) and Four-ninths (7). The prior Full (0),
Two-thirds (1) and Half (2) values are unchanged for persisted settings and
Native Lighting. The new modes rotate a per-tile phase without changing any
resource dimensions. The 1/N modes visit every tile residue in N frames;
Four-ninths traces four of nine tile pixels each frame and covers all nine
within three successive phases. Sampling counts differ only at partial
viewport tiles.

Native Composite has an independent persisted `spatialReconstruction` boolean
(default true). When enabled, it fills only explicitly omitted raw mask pixels
(validity -1) using full-render-size neighboring samples whose ray distances
agree with the current raster-derived receiver distance. It also restricts
blending to one material instance identity; invalid (0) and non-finite
samples never become evidence. No extra reduced-resolution buffers, RT rays,
FSR Ray Regeneration or temporal history are involved in this extension.
The original raw mask remains immutable and available for debugging.
