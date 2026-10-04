# Runtime feature allocation budget

Source-derived texture and buffer payload budget, 2026-09-29. No shader compilation,
shader validation, game inspection, or runtime measurement was performed for this budget.

## Assumptions

Flat rendering, one sample per pixel, one mip unless explicitly specified, successful
initialization, default Wind and FFT sizes. Main and postprocessing allocation dimensions
are both the dimensions in the table; main/main-copy color is RGBA16F, main depth is
32-bit, HDR output is 32-bit. Actual allocation dimensions can differ from display
resolution and between engine/postprocessing targets, especially with upscaling.
MiB means 1,048,576 bytes. Views are not counted as duplicate texture storage.
Driver alignment, shader objects, CPU metadata, small constant buffers and asset textures
are excluded. These are allocated payloads, not a measurement of resident VRAM or the
net difference from vanilla. The rows describe enabled allocations. The new main-switch
lifecycle releases these effect-owned pools while disabled; persistent backend resources
listed separately can remain in use.

## Enabled allocation budget

| Allocation                           | 1920x1080 MiB | 2560x1440 MiB | 3840x2160 MiB |
| ------------------------------------ | ------------: | ------------: | ------------: |
| SSGI main textures                   |        123.90 |        220.28 |        495.62 |
| Postprocessing textures listed below |        295.59 |        503.72 |       1098.36 |
| Skylighting probe volumes only       |        112.00 |        112.00 |        112.00 |
| Grass Collision simulation textures  |         24.00 |         24.00 |         24.00 |
| Wind grass and tree fields           |         50.25 |         50.25 |         50.25 |
| Subsurface Scattering targets        |         31.64 |         56.25 |        126.56 |
| Terrain Blending targets             |         27.69 |         49.22 |        110.74 |
| HDR private color/output/UI targets  |         31.64 |         56.25 |        126.56 |
| Screen-Space Shadows mask            |          1.98 |          3.52 |          7.91 |
| LLF cluster AABBs, indices and grids |          8.72 |         15.72 |         34.86 |
| Effects11 common textures            |        154.52 |        249.88 |        522.35 |
| **Subtotal**                         |    **861.93** |   **1341.08** |   **2709.22** |

The main-switch lifecycle now skips these setup paths while disabled and releases
previous allocations on disabling. This table preserves the full allocation arithmetic;
postprocessing now allocates only active children, so its row is a combined budget,
not the default or disabled-state cost. The SSGI row describes full GI; AO-only
operation releases GI-only textures. Reduced dispatch resolution does not shrink
the remaining textures.

## Arithmetic and source references

Let P = W*H and M(n) = sum(max(1,W>>i)*max(1,H>>i), i=0..n-1).

-   [SSGI](../../src/Features/ScreenSpaceGI.cpp): 52P + 8M(5) bytes. Four RGBA16F GI
    textures (32P), two RG16F chroma textures (8P), four R8 AO/history textures (4P),
    temporary radiance and previous geometry (8P), and radiance/depth/normal mip chains (8M(5)).
    AO-only retains 8P + 4M(5) bytes and releases the GI-only 44P + 4M(5) bytes.
-   [Skylighting](../../src/Features/Skylighting.cpp): 256*256*128\*(8+1+4+1) = 112 MiB.
    The copied precipitation-occlusion depth texture is additional.
-   [Grass Collision](../../src/Features/GrassCollision.cpp): 1024^2*2*(8+4) = 24 MiB.
-   [Wind grass](../../src/Features/Wind/WindGrass.cpp): four RGBA16F textures at each
    default 1024, 512, 256 dimension: 32*(1024^2+512^2+256^2) = 42 MiB.
    [Wind trees](../../src/Features/Wind/WindTrees.cpp): four RG16F spring textures plus
    two RGBA16F and two RG16F three-slice transient arrays at 256,128,128:
    (16+72)*(256^2+128^2+128^2) = 8.25 MiB.
-   [Subsurface Scattering](../../src/Features/SubsurfaceScattering.cpp): two main-format targets, 16P.
-   [Terrain Blending](../../src/Features/TerrainBlending.cpp): main-depth clone + R32F,
    R16 UNORM, R32F = 14P with 32-bit depth. With 16-bit main depth, use 12P.
-   [HDR](../../src/Features/HDRDisplay.cpp): RGBA16F + output-format + RGBA8 = 16P
    for a 32-bit output, 20P for scRGB. Engine render-target format upgrades are additional.
-   [Screen-Space Shadows](../../src/Features/ScreenSpaceShadows.cpp): one R8 mask, P;
    VR additionally allocates a same-sized stereo copy.
-   [LLF](../../src/Features/LightLimitFix.cpp): ceil(W/64)*ceil(H/64)*32 clusters,
    each with 32-byte AABB, 128 four-byte indices and 16-byte grid = 560 bytes per cluster.
    Light buffers, tile-depth ranges, shadows and readback buffers are additional.

## Postprocessing subtotal

[Setup](../../src/Features/PostProcessing.cpp) can allocate the engine-main copy and
linear input, plus active children from six full-sized outputs (ColorGrading, Composite, LUT, Vignette, Camera,
Border), DoF resources, LocalExposure resources, bloom, LensFlare and PhysicalGlare.

At 3840x2160 with the assumptions above:

| Group                                                               |    MiB |
| ------------------------------------------------------------------- | -----: |
| Main copy + linear input                                            | 126.56 |
| Six outputs + 64^3 color LUT + curve textures                       | 381.69 |
| DoF full/half/gather/tile/focus textures                            | 280.13 |
| LocalExposure luminance pyramid, grid, blur pair and base luminance |  37.44 |
| Bloom nine-mip RGBA16F texture                                      |  84.37 |
| LensFlare full/half/quarter targets + default 256 FFT textures      | 100.88 |
| PhysicalGlare output + default 512 FFT textures                     |  87.28 |

DoF: 3 full RGBA16F, 4 half RGBA16F, 3 successively halved gather RGBA16F,
full/half/3 gather R16F, 3 tile RGBA16F and one R32F focus texel.
LensFlare: full + 2 half + quarter RGBA16F, plus 4*256^2*8 FFT bytes.
PhysicalGlare: full RGBA16F, plus 512^2*(10*8+16) FFT bytes.
LocalExposure: ten-mip R16F, full R16F, two W/32 by H/32 R16F textures,
and ceil(W/64)*ceil(H/64)*32 RG32F grid.
MotionBlur textures are allocated later; alternate-resolution pipelines, provider output,
asset LUT/bokeh textures and later FFT caches are also excluded from this subtotal.

## Effects11 common allocation

[TextureManager](../../src/Features/Effects11/TextureManager.cpp) allocates eleven
canvas-sized textures totaling 62 bytes per pixel: two HDR temporaries (16), RGBA32 (4),
RGBA64 (8), RGBA64F (8), R16F (2), R32F (4), RGB32F (4), two SDR temporaries (8),
and the lens target (8). Fixed resources add 31.916 MiB: two 1024-square RGBA16F
bloom targets, seven RGBA16F targets from 1024 down through 16, a three-mip
1024-square R11G11B10 downsample texture, and two single-pixel adaptation textures.

Effects11::SetupResources calls EffectManager::Initialize when the main feature is
enabled; ReleaseResources drops these allocations when it is disabled. Compiled FX
objects are retained, including their internal constant buffers. Shared textures remain
available while the feature is on, even when an individual preset stage is off. These allocations are separate
from the PostProcessing feature above. Additional preset-loaded textures, shader resources,
VR eye-crop targets, the reverse-Z standard-depth copy, and the lazy
volumetric-ray targets are excluded. Volumetric rays add
two half-size R16F targets plus one half-size R32F depth target: 2*W*H bytes for even
allocation dimensions (15.82 MiB at 4K).

## Additional allocations outside the subtotal

-   Dynamic Cubemaps: six capture cubes in the inherited reflection format, three
    R11G11B10 cubes, BC6H scratch and two BC6H cubes. At 256 faces with full mips,
    these total approximately 19.5 MiB with 32-bit capture or 31.5 MiB with 64-bit capture,
    plus the packaged fallback texture.
-   Cloud Shadows: 32 layer cubes + two copies, R8; at 256 faces, 12.75 MiB with one
    mip or approximately 17 MiB with a full chain (inherits reflection mip count).
-   LLF atlas: A*A*bytesPerDepthTexel. At the default 8192 dimension this is 128 MiB
    for 16-bit depth or 256 MiB for 32-bit depth, per atlas. A static-cache atlas, when
    allocated, adds the same amount; engine shadow arrays are separate.
-   Exponential Height Fog, allocated when its volumetric path is used: default
    grid ceil(W/16)*ceil(H/16)*64, four RGBA16F volumes + two R32F depth grids.
    At native 4K this is 63.53 MiB; at 1440p it is 28.23 MiB.
-   Volumetric Shadows: two 512^2 RG16 UNORM textures, each with two mips: 2.5 MiB,
    allocated when the shadow capture path first needs them.
-   Upscaler internals, additional Effects11 resources, terrain-world textures, grass instance capacity,
    water geometry/flowmaps and material/packaged asset textures remain data-dependent.

The subtotal describes hundreds of MiB to multiple GiB of enabled effect resources;
it is not a complete application VRAM total or a measured saving. The new lifecycle
keeps shader support compiled, delays effect allocations until enabled, and releases
them at the next render-thread transition after disabling.

## Allocations intentionally retained

-   LLF strict-light buffer, native shadow data, scheduler and atlases remain required
    for an enabled session. Its main toggle now requires a restart in both directions;
    starting disabled skips those allocations and hooks without changing shader support.
-   Unified Water's geometry/flowmaps and True PBR's converted material support are
    selected at startup. Their restart-required switches skip that setup when off.
-   Horizon Fix integration does not control the separate plugin's geometry/resources,
    even though its integration switch now requires a restart.
-   Upscaling performance mode and the active DX12 swapchain retain backend resources.
-   Compiled shader/FX objects, their internal constants, and small cached layouts/states
    remain. Grass CPU capture bookkeeping remains for rebuilding on enable.
-   Engine caches or an active borrower can retain shared resources independently.
    Submitted GPU work can outlive the application references released at a toggle.

The C++ resource implementation was built and locally deployed on 2026-10-02,
before the 2026-10-04 dev sync. The current source has not been rebuilt.
Shader validation and in-game memory measurements have not been performed. The budget does not establish the resulting resident-VRAM delta.
