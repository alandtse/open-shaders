# Native Neural Rendering

Upscaling owns `NeuralRendering`, which evaluates NGX Feature 18 once per rendered eye
inside `Upscaling::Main_PostProcessing`, before DLSS/FSR upscaling, VR submit capture,
and frame-generation input capture. It processes the active render-resolution regions
of `kMAIN` and writes them back into the existing pipeline. NR is not evaluated at
the larger display resolution when render scaling is active.

The implementation owns its render-resolution integration directly and does not
depend on the VR submit-upscaling implementation.

## Enable

Enable Neural Rendering in the Upscaling panel. Its
`Upscaling.neuralRenderingEnabled` setting defaults to `false`.
There is no separate NR feature registration, feature flag, or INI.
The package does not redistribute NVIDIA runtime binaries. Supply
`nvngx_dlssnr.dll` **310.8.x** under
`Data/Shaders/Upscaling/Streamline/`. NR uses the same
`Streamline::pluginDir` that the existing DLL version check scans.
Initialization or evaluation errors pause NR and appear in the Upscaling panel and
the existing CommunityShaders log. After correcting the error, use **Retry NR**,
toggle NR off/on, or clear the shader cache to retry on the next rendered world
frame. Resource setup also permits a retry. Failed GPU work is retired before
releasing the failed runtime. There is no per-frame automatic retry.

The Enable switch is disabled while the runtime on disk is one the pass would
refuse, and its tooltip names the reason and the fix, so a missing file or a
build outside the validated set is never offered only to latch a failure. The
verdict is re-read when the file changes, so installing a runtime mid-session
unblocks the switch without a restart; an already-enabled feature is never locked
out. The same verdict is drawn under the Streamline DLL table in Backend
Diagnostics, next to the version that table already lists, and DevBench's
`neuralRenderingStatus` reports it as `runtimeAvailability`, `runtimeVersion`,
`runtimeDetail` and `runtimeLoadable`.

Developer mode loads a build the pass would otherwise refuse, so a runtime under
test can be evaluated before it is pinned. It changes nothing about what is
trusted: the version and digest gates still classify the file, the panel shows
`runtime_developer_load` wherever the refusal would be explained, `Runtime::Initialize`
logs the refusal it loaded through, and `RuntimeAvailability::AllowsLoad` is the
one place the exception lives. A missing file is never loadable, and an unverified
build's output is not evidence that a build is correct, only that it ran.

The controls persist under `Upscaling.neuralRenderingTuning`:

| UI control               | Config key               | Range                       | OS default | Evaluation parameter            |
| ------------------------ | ------------------------ | --------------------------- | ---------- | ------------------------------- |
| Style                    | `style`                  | Style 0 / Style 1 / Style 2 | 0          | `DLSSNR.Style`                  |
| Intensity                | `intensity`              | 0–2                         | 1.0        | `DLSSNR.Intensity`              |
| Local Tone Strength      | `localToneStrength`      | 0–2                         | 1.0        | `DLSSNR.LocalToneStrength`      |
| Local Structure Strength | `localStructureStrength` | 0–2                         | 1.0        | `DLSSNR.LocalStructureStrength` |
| Skin Structure Strength  | `skinStructureStrength`  | -1–2 (-1 displays Auto)     | -1         | `DLSSNR.SkinStructureStrength`  |
| Use Auto Mask            | `useAutoMask`            | Off / On                    | On         | `DLSSNR.UseAutoMask`            |

All six values are written in `NR::Runtime::Evaluate()`. Committing a tuning edit
recreates the Feature 18 handles because the runtime can latch appearance tuning
during creation. Existing config values are preserved when present. Missing or
invalid values use the defaults above and are bounded to the documented ranges.
**Restore NR Defaults** resets all six controls; **Reset NR History** invalidates
both eye histories without changing tuning.

Developer mode also exposes **Use resolution-scaled NR motion**. It is a
session-only A/B switch and defaults on. The enabled path supplies the NR input
dimensions as `DLSSNR.MVecScaleX/Y`; the disabled path supplies identity
scales (`1.0, 1.0`). Changing the switch invalidates NR history once.

The public [private-contract findings](https://github.com/kibblerz/DLSS5-Reshade-AIO/blob/main/lab/PRIVATE-CONTRACT-FINDINGS.md)
establish Style 0/1/2 and distinguish the runtime callback's double/fixed-point
encodings from the NGX parameter object used here. OS keeps float strength
setters and unsigned Style/boolean setters on that existing NGX route, as used
by the [evaluation contract](https://github.com/bmitch87/DLSS5VKLayer/blob/main/extracted_pipeline_notes.md).
UI ranges follow the [Cost Scaler configuration](https://github.com/xenmods/DLSSNR-Cost-Scaler/blob/main/nvngx_dlssnr.ini);
its strength default of 1.0 does not override existing OS defaults.

Both VR and SE/AE use this hook. In VR there are two persistent Feature 18
instances; in flatrim only eye zero is evaluated. Color and depth/motion guides
use the current render-eye dimensions. Changing dimensions, format, eye count,
or explicitly recreating resources recreates the NR instances.

### Tracked-actor crop

**Limit to Tracked Actor** (`Upscaling.neuralRenderingTuning.regionOfInterest`,
default off) restricts evaluation to a crop around the most prominent actor the
camera can see and leaves the rest of the frame at pre-NR content. It is not an
NGX parameter: it sets the Color/Output subrects and gates the composite mask in
`ColorTransferCS.hlsl`, so the periphery never samples the undefined region
outside the evaluated subrect.

The main thread scores every loaded actor within `kMaxActorDistance` by its
on-screen coverage, falling off with distance from the frame centre by a
Gaussian (`kCentralitySigma`), and adds a sticky bonus (`kIncumbentScoreBonus`)
to the actor the crop already follows, so two similarly sized actors do not
alternate and reset NR's history. A candidate covering less than
`kMinVisibleAreaFraction` of an eye is dropped, and a candidate no eye sees is
not a candidate at all. The player is a candidate only in third person on
non-VR runtimes; in first person and in VR it sits at the camera, and a crop
around it would be the near plane. The candidates are sorted by score and projected per eye
in that order, so the line-of-sight rays only run on the best few; the winner's
per-eye pixel crop goes through the stabiliser before publication. The crop
maths, the stabiliser and the reset rules are shared in `Util::Region`
(`src/Utils/Region.h`); this actor source's own values (tracking distance,
minimum visible fraction, centre falloff, incumbent bonus, padding, stabiliser
thresholds, reset policy and history tolerance) are named constants in
`src/Features/Upscaling/NeuralRendering/ActorRegion.h`. A second region source,
such as a gaze-driven one, adds a sibling namespace with its own
values rather than a new mechanism. With the toggle off, or with no actor
tracked, the crop is the whole frame and NR evaluates exactly as it does
without it.

The crop controls below only act through a tracked crop, so they follow that
toggle: **Show Region Overlay**, **Crop Fit** and **Track Multiple Characters**
are greyed out while it is off, and `NR::Tuning::Sanitize()` clears all three
(`regionFit` back to Padded) whenever `regionOfInterest` is off, so a value
loaded from a config file cannot read as active while the pass ignores it.

The bound the crop projects is the actor's authored local-space box
(`GetBoundMin`/`GetBoundMax`) carried into world space by its own root transform,
not the engine's world-bound sphere: a sphere's cube is about its radius on every
side whatever the actor's shape, so a standing humanoid got a box as wide as it
is tall. An unusable authored box (a non-finite component, or an inverted axis)
falls back to the sphere-cube.

That box is the rest pose, so a swung limb or a ragdoll leaves it. For the actors
that reach the crop (not for the per-frame ranking of every actor) the bound also
includes the box around the skeleton's joint positions, grown by `kJointMargin` for
limb thickness, so the crop follows the pose. A dead or ragdolled actor keeps an upright
root while its bones lie flat, so its bound is the joint box alone. Camera nodes and any node further than
the body's own reach from its rest-pose box are ignored, and the walk visits at most
512 nodes. `Util::GetActorBoundPoints` in `src/Utils/ActorUtils.h` builds the points
and `Util::IsActorVisibleFromEye` does the line-of-sight test, so the engine access
lives in one utility and the NR code only projects points.

**Show Region Overlay** (`Upscaling.neuralRenderingTuning.regionOverlay`,
default off) draws the crop for debugging: a green outline in the game frame,
through the composite kernel of `ColorTransferCS.hlsl`, and the same per-eye
rectangles over an NR-resolution preview at the bottom of the Neural Rendering
tab. Inside the green outline it draws the tracked actor's own projected box as
a second, thinner yellow outline, so the margin the crop adds around the
character is visible; that box is the raw pick and is not stabilised, so a box
that flaps is shown flapping. It is meaningful only with **Limit to Tracked
Actor** on; it draws nothing while no actor is tracked, and an outline along the
frame edge when a tracked actor's crop fills the whole frame. The per-eye crop is
also readable without the overlay from `openshaders.feature diagnostics`
(`neuralRegionActive` and `neuralRegion`), which reports each eye's rect in
pixels.

In developer mode only, a **Crop Fit** control selects how the crop is fit to the
projected bound: **Padded** is the normal padded crop, and **Tight** evaluates the
grid-aligned bounds alone with no margin, for checking what the crop covers on its
own. Both keep the 64 px alignment and the stabiliser, since NGX must not be handed
an arbitrary subrect.

Also in developer mode, **Track Multiple Characters** (`regionGroup`) grows the crop
to cover the next most prominent actors, in score order, while the union stays under a
share of the eye (`GroupAreaCap`: `kMaxGroupAreaFraction` by default, or the
calibrated cost knee once a calibration has run), with at most `kMaxGroupActors`
members and `kMaxGroupCandidatesTested` line-of-sight tests per frame. An actor that
one eye cannot see does not stop the other eye's crop from growing. The yellow box is
then the union of the members' boxes, and the tracked actor that carries the
incumbent bonus stays the highest-scoring one.

**Calibrate Crop Cost** (developer mode; DevBench action `calibrateNeuralCrop`)
forces centred crops of shrinking area (`CropCalibration::kFractions`, the full
frame first) for about half a minute, times `Upscaling::NREvaluate` from the
profiler over four passes, and reports the fastest low-percentile GPU time per step
(NR's timing flips between a fast and a slow state, so a median would be arbitrary),
`stabilityRatio` between passes, the cheapest step and the
largest crop within `kKneeTolerance` of it under `neuralCalibration` in the
diagnostics. It refuses to run with frame generation on, since frame generation
paces the frame and skews the GPU zones.

Inside the crop the neural result eases in with a smoothstep ramp, so the different
shading NR gives the crop does not end at a hard seam. The band on each side spans the
margin between the tracked actor's box and the crop edge, kept between 16 and 96 px
(`RegionFeather` in `package/Shaders/Common/RegionFeather.hlsli`), so the weight reaches
1 by the actor's outline and the whole margin is used for the transition. With no actor
box, as in the calibration sweep, the band is 32 px, and an edge on the frame edge has no
band. The crop's minimum padding (`kPadding`) is what leaves that margin.

Both drawings come from reusable helpers: `RegionOverlay::OutlineOnly` in
`package/Shaders/Common/RegionOverlay.hlsli` composites a region outline over a
pass's own colour without changing anything else in the frame, and
`Util::RegionOverlay::Draw` (`src/Utils/RegionOverlay.h`) outlines pixel rects over
an already-drawn ImGui image, so another feature can show a rectangular region
with neither a new HLSL helper nor a new rect type.

The published crop is always stabilised, which is what keeps a crop that jitters
by a grid step every frame from resetting NR's temporal history continuously.
`Util::Region::RegionStabilizer` holds the crop for `kStabilizerPolicy.holdFrames`
after the candidate goes inactive, keeps it unchanged while the candidate stays
inside it, grows it only by union, and shrinks it only once a whole
`kStabilizerPolicy.shrinkWindowFrames` window's envelope is at most
`kStabilizerPolicy.shrinkAreaFraction` of the held area. A crop change larger
than one alignment step, or an appearance or disappearance, adds `RegionChanged`
to the frame's reset reasons; the camera, frame-gap and resource resets are
unaffected.

`openshaders.feature diagnostics` reports the crop and its cost:
`neuralRegionActive`, `neuralRegion` (one `{x,y,width,height}` rect per eye, in
the pixels of `neuralRenderSize`'s `{width,height,eyes}`), `neuralActorBounds`
(the tracked actor's own projected box, unpadded and unstabilised, in the same
per-eye shape), `neuralRegionSource` (which sources chose the crop:
`none`, `actor`, `fovea` or `both`), `neuralCalibration` (`state`, `fractions`, `stepMs`, `stabilityRatio`, `floorMs` and
`kneeFraction` of the last crop cost sweep), `neuralFrames`
(frames NR has applied), `neuralResets` (a cumulative count per reset reason,
keyed `request`, `first`, `gap`, `position`, `direction`, `projection`,
`creation`, `region`) and `neuralResetDrainMs` (cumulative CPU time blocked in
the history-reset drain). The counters are monotonic since NR started, so a
caller differences two reads: a `region` count that keeps rising against a
rising `neuralFrames` means the crop keeps moving, and a large
`neuralResetDrainMs` per frame means those resets are costing real CPU time.

### Fovea crop

**Follow Foveation** (`Upscaling.neuralRenderingTuning.regionFollowFoveation`,
default on) is the second region source. On VR, while FoveatedRender is active
(DLSS or FSR selected and the region narrower than Full Eye), NR evaluates only
the foveated region, so the periphery keeps its pre-NR content, which the
foveated route replaces with its cheap stretched view anyway. It does not act
through the tracked actor: the foveation region alone is a valid crop, so the
control stays set and usable with **Limit to Tracked Actor** off, and
`NR::Tuning::Sanitize()` does not clear it. With a tracked crop the two are
intersected per eye (`Util::Region::ClipRegion`), so the clip never leaves NR
evaluating outside the region the upscaler sharpens.

The per-eye UVs come from `FoveatedRender::GetClipUV`, which reports nothing
while foveation is inactive or the user is dragging the region; skipping the
clip for those frames costs at most the resets of leaving and re-entering it,
not one per drag frame. `NR::FoveaClip::BuildClip` (`NeuralRendering/
FoveaClip.h`) turns the two UVs into a per-eye pixel clip, padded by the
shader's 32 px default feather band and aligned to the usual 64 px grid, so the
composite's fade to pre-NR content lands in the stretched periphery instead of
the sharp region; it is inactive unless both eyes resolve.
`NR::FoveaClip::ClipSubject` narrows the yellow actor box the same way, dropping
an eye whose box falls outside the clip, so the overlay and the feather subject
never extend past the evaluated crop and an out-of-region actor leaves the
feather band at its 32 px default. The clip is skipped while the crop cost
calibration forces its own centred crops and in the main and loading menus, the
same gate the foveated route uses. `neuralRegion` and `neuralActorBounds`
describe the tracked-actor source alone; `neuralRegionSource` reports whether
the fovea clip also narrowed the crop the last frame evaluated.

## Resource and temporal contract

-   Color: a bounded display-referred proxy in an RGBA16 float carrier at
    render-eye resolution. The original scene-linear `kMAIN` RGB and alpha are
    retained separately for luminance-ratio writeback.
-   Depth: the existing upscaling encoder reads the engine depth SRV,
    including VR's R24 depth view, and writes non-inverted device depth to
    R32 float. Depth is not linearized.
-   Motion: the same encoder's undilated path writes RG16 float, preserving
    correspondence with the center-pixel depth guide. The default Feature 18
    contract converts normalized eye-UV displacement to NR input pixels using
    the input width and height independently as motion-vector scales. The
    developer A/B switch can instead send identity scales. Reset frames submit
    zero motion because their previous history is invalid.
-   Frame data: diagnostics observe cached camera position, view direction, and
    projection changes. These inferred cuts do not reset production history.
    Production evaluation does not supply camera aliases to Feature 18. When
    the developer **Feed historical camera parameters** option is enabled, the
    current jitter, frame time, world-to-view matrix, and view-to-clip matrix
    are supplied for the diagnostic comparison. Before a history reset, prior
    NR GPU work is retired. Frames retaining history keep GPU-only interop
    ordering.
-   Loading transitions, skipped world frames, enable changes, shader
    invalidation, and resource recreation invalidate history. Ordinary UI menus
    continue evaluating NR when the world rendered that frame; UI composition
    remains later in the frame and is not passed through Feature 18.
    Resolution, format, eye-count changes, or explicit resource recreation retire
    GPU work and recreate the eye resources and NGX instances together. Changing
    only the engine texture pointer does not recreate NR.

The runtime receives independent depth and motion subrects and X/Y motion scales
from the producer. It clamps each subrect against its own D3D12 allocation before
writing the Feature 18 parameters; an empty region fails evaluation. This follows
the resource-metadata handling in [OptiScaler DLSSNR PR #42](https://github.com/Dagherbou/OptiScaler_DLSSNR/pull/42).
That integration uses the game's SR `MVLowRes` flag to select render- versus
output-resolution motion. OS owns its guide encoder instead: it always extracts
the active render-resolution region, including the eye offset, into a separate
texture starting at `(0, 0)`. Its NR metadata therefore describes that extracted
texture, not the original packed engine allocation or the subsequent SR output.
No SR creation flags are copied into the Feature 18 creation flags.

For the current pre-SR path, NR input, depth, and motion extents are equal, so
making this metadata explicit does not change the normal values sent to NGX.
The OptiScaler fix alone does not establish the cause of OS's reported flicker.

The Feature 18 parameter contract and HDR behavior were checked against the
[DLSS5VKLayer contract notes](https://github.com/bmitch87/DLSS5VKLayer/blob/main/extracted_pipeline_notes.md)
and its current implementation. Feature 18 uses a private runtime interface;
the version gate bounds this implementation to its observed ABI.

## Current Feature 18 contract

The runtime creates one persistent Feature 18 instance per rendered eye using
the observed 310.8.x private ABI. It creates a same-resolution pass with
`DLSSNR.Upscaling=0u`, `DLSSNR.Scale=1.0f`,
`DLSSNR.ScalingRatio=1.0f`, and `DLSSNR.Hint.Render.Preset=0u`.
The creation flags are `IsHDR | DoSharpening | AutoExposure` (`0x61`) in both
`Feature_Flags` and `NVSDK_NGX_Parameter_Feature_Flags`. The private selectors
are `DLSSNR.AutoExposure=1u`, `DLSSNR.Hdr=1u`, and `DLSSNR.SDR=0u`.
`InPreExposure`, `InExposureScale`, `NVSDK_NGX_Parameter_PreExposure`, and
`NVSDK_NGX_Parameter_ExposureScale` are initialized to `1.0f`. Parameters are
populated through `NVSDK_NGX_D3D12_PopulateParameters_Impl` before creation.
The creation contract also supplies `DLSSNR.ControlMask=nullptr`,
`DLSSNR.UseAutoMask`, and `DLSSNR.UICorrection=1u`.

The HDR-selected model contract does not mean that unbounded scene-linear
values are sent to Feature 18. The input is the bounded display-referred proxy
described below. The original scene-linear frame remains separate and is used
for reconstruction after model evaluation. This D3D12 private ABI is observed
rather than officially documented; successful creation and evaluation do not by
themselves establish image quality or temporal correctness.

Feature resources and frame parameters are updated in place after creation.
Appearance tuning is written before creation, and committed UI tuning changes
recreate the persistent eye handles. A contract change must likewise retire
queued GPU work before releasing the handles and recreate both eye instances
when it changes creation-latched parameters.

The runtime discovers the private float setter slot from the populated parameter
object before writing Feature 18 floats. Unsigned and resource parameters use
the corresponding private ABI slots rather than the public SDK overloads.

The public [DLSS5VKLayer creation code](https://github.com/bmitch87/DLSS5VKLayer/blob/main/core/ngx_snippet.cpp#L509-L523)
and [HDR input description](https://github.com/bmitch87/DLSS5VKLayer#hdr-input)
are reference evidence for the selector names and values. They do not establish
the complete D3D12 private ABI. Independent projects such as
[video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr#neural-rendering)
and [DLSSNR-Cost-Scaler](https://github.com/xenmods/DLSSNR-Cost-Scaler) likewise
provide observed pre-release behavior rather than an official Feature 18 guide.

`ColorTransferCS.hlsl` now keeps the scene-linear frame in `Original` and builds
a separate bounded proxy for Feature 18. It applies the current exposure, a
luminance-preserving soft knee above display white, a hue-preserving peak bound,
and the existing linear-to-sRGB conversion. Feature 18 therefore sees an opaque
display-referred image in the 0–1 range.

When Post Processing is active, the host derives the scene white point from its
latest GPU adaptation value, exposure compensation, adaptation limits, and enabled
color-grading exposure multiplier. The input is divided by that white point and
writeback multiplies by the exact saved value. This matches the float-HDR reference
contract without a tone curve, ceiling, or CPU readback. The scalar is one when
that exposure pipeline is unavailable. Feature 18 still owns its internal temporal
auto-exposure state.

NR output is decoded only into the same proxy space. A luminance ratio with a
near-black floor and two-sided highlight bounds is applied to the untouched
scene-linear RGB. The model cannot replace the frame's hue or alpha, and its
bounded proxy is never inverse-tone-mapped. Both eyes must evaluate successfully
before either is written back. NR remains before the existing HDR DLSS pass and
the actual tone mapper stays downstream. Per-eye history/reset behavior is unchanged.

Runtime validation still requires the supported hardware and in-game comparison in
SE/AE and VR. A C++ build and deployment do not establish native HDR model support,
image quality, or temporal stability in this D3D12 path.

## Applying Neural Rendering by material

The **Apply Neural Rendering by Material** setting, backed by
`neuralRenderingTuning.materialStrength`, the six strengths (`strengthSkin`,
`strengthHair`, `strengthEyes`, `strengthFoliage`, `strengthLandscape` and
`strengthOther`, each 0 to 1 where 1 applies NR fully and 0 bypasses it) and `strengthEdgeSoftness` (0 to 4 pixels), binds
Feature 18's `DLSSNR.UIAlpha` lane. That lane is a graded protection value, not a
binary mask: alpha 0 applies NR fully, alpha 1 restores the pixel from
`DLSSNR.Backbuffer`, and a value between the two blends. `strengthEdgeSoftness`
averages the per-pixel value over a `(2r+1)`-square box, clamped at the eye bounds,
so a material boundary is soft rather than hard. With the setting off, nothing is
bound and the shipped null-mask path is emitted exactly as before.

`CategoryAlphaCS.hlsl` rebuilds one `R8_UNORM` alpha per eye every frame, sized to
the Feature 18 **output** extent, from the `Masks2` SRV at the same per-eye pixel
offset `ColorTransferCS.hlsl` uses (`id.xy + EyeOffsetX`). `DLSSNR.Backbuffer` is
the NR input texture itself (`eye.color`, the R16G16B16A16_FLOAT proxy domain the
model works in, bound as Color too); a raw scene copy is the wrong colour domain and
darkens the frame. `DLSSNR.ControlMask` stays null and `UseAutoMask` keeps its
tuning value, because binding a control mask and `UIAlpha` together can collapse the
network contribution. The eight `DLSSNR.UIAlphaSubrect*` and
`DLSSNR.BackbufferSubrect*` keys are written equal to the output subrect at both
creation and evaluation, so an active crop does not move the alpha: the alpha is
indexed by the same absolute eye pixel as the NR input, and only the subrect the
runtime reads it through changes.

This is a quality control, not a performance one: `NREvaluate` still runs over the
whole frame at a fixed cost, so masking a material does not save GPU time.

Feature 18 latches the `UIAlpha` binding at creation, so switching `materialStrength`
on or off recreates the persistent eye features at the single per-frame detection
point in `DrawBeforeUpscaling`, which also covers a devbench change; a strength or
softness change only rewrites the per-frame alpha, with no rebuild and no history
reset. If the deferred material lane is absent or the alpha shader or texture cannot
be created, the pass logs once and falls back to the shipped unprotected path for the
session; an NGX or SEH fault with the alpha bound turns the setting off for the
session without changing the saved value. Both are reported by
`materialStrengthAvailable` on the `neuralRenderingStatus` query.

Test it through devbench: `openshaders.feature set shortName=Upscaling
settings={"neuralRenderingTuning":{"materialStrength":true,"strengthSkin":0,...}}`,
then read `materialStrengthAvailable`, `materialStrengthActive`,
`materialStrengthValues` (None, Skin, Hair, Eyes, Foliage, Landscape) and
`materialEdgeSoftness` from `neuralRenderingStatus`, and capture a frame with
`captureNeuralRendering`.

### Show material map

**Show Material Map** (`Upscaling.neuralRenderingTuning.showMaterialMap`, default off) is a
debug view that draws the deferred material lane instead of the composited image, so the
pixels each material covers can be checked on a real character. It adds no pass: the
existing composite kernel of `ColorTransferCS.hlsl` decodes the same `Masks2` SRV
`CategoryAlphaCS.hlsl` does and reads a few more cbuffer values, so the setting costs no GPU
time.

Every pixel whose decoded category is enabled in `materialMapFilter` is drawn; a pixel of a
disabled category keeps the normal image. The filter is a bitmask with one bit per
`NeuralRenderingCategory` id (bit 0 None, shown in the panel as **Everything Else**, then bit
1 Skin, 2 Hair, 3 Eyes, 4 Foliage and 5 Landscape), all six on by default.
`NR::Tuning::Sanitize()` keeps only the low six bits, so a hand-edited config cannot select a
category that does not exist.

`materialMapMode` picks what a drawn pixel shows. **Category colours** blends the pixel's
fixed `DebugColor` over the composited image at 0.65 opacity; these are the colours the
developer **Debug view** combo's Category entry draws, and the panel shows them as legend
swatches beside the filter. **Strength** replaces the pixel with a grayscale ramp of the
strength **Apply Neural Rendering by Material** gives it (0 black, 1 white), so a material
the classification mislabels reads as the wrong strength. The strength view samples the
pixel's own label and skips the `strengthEdgeSoftness` box average, which approximates the
alpha `CategoryAlphaCS.hlsl` builds; with **Apply Neural Rendering by Material** off nothing
is protected, so it shows 1.0 everywhere.

DevBench sets the three keys under `settings.neuralRenderingTuning` and reads
`showMaterialMap` back from `neuralRenderingStatus`.

## Feature 18 output channel-order test

Feature 18 runtime builds do not consistently expose the channels of an
`R16G16B16A16_FLOAT` output as RGBA. This behavior is independently handled by
[ComfyUI-DLSS5-NR](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR/blob/main/nodes.py),
which documents wrong or blue colors and swaps returned slots `[2, 1, 0]` for
BGRA-like builds, and
[obs-dlss5-nr](https://github.com/Saganaki22/obs-dlss5-nr/blob/master/src/bridge/nr_bridge.cpp),
which packs the input as RGBA16F and compares RGBA and BGRA interpretations of
the returned output.

The Skyrim test of the 310.8 DVS Production runtime, SHA-256
`8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC206`,
disproved BGRA output for this build. Changing
`NeuralOutput[id.xy].rgb` to `.bgr` produced an immediate full-frame blue cast,
including the original scene rather than only NR-generated highlights. This is
the expected signature of swapping an already-correct RGBA result. Open Shaders
must retain `.rgb` for this runtime. Its earlier localized blue lighting and
colored highlight blocks have a different cause.

The test also establishes that the input must remain RGBA. Runtime-specific
channel selection may be useful for other DLL builds, but it must not be inferred
from the 310.8 version number alone.

Because the channel order belongs to the build and not the version number,
`Runtime::Initialize` accepts only the builds listed in
`NR::kValidatedRuntimeSha256`: the 310.8 DVS Production runtime above and the
310.8.0.0 test build, SHA-256
`E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`, whose
in-game SE/VR captures showed no channel swap. Any other 310.8.x build is refused
with a latched failure instead of being rendered through an unverified channel map,
unless developer mode forces it through for evaluation as described under
[Enable](#enable).

The existing scene-white normalization is separate and remains valid. Open
Shaders' exposed-linear value `1.0` represents the current scene paper white,
matching the documented float-HDR convention. Applying another paper-white
multiplier would double-scale the image and would not fix red/blue inversion.

## Color boundary and reconstruction

Public D3D12 implementations consistently bound the image presented to Feature
18 to a display-referred range. Open Shaders follows that resource-value rule
while retaining an RGBA16F carrier: `ColorTransferCS.hlsl` derives a bounded
proxy from the scene, and Feature 18 receives that proxy at render-eye
resolution. The HDR selectors in the private creation contract describe the
model route; they do not authorize passing unbounded scene-linear values to the
model.

The implementation avoids the earlier unbounded form
`max(ToLinear(kMAIN), 0) * exposure`. Exposure and the current scene white point
are applied while building the proxy, then a soft knee and peak bound keep the
model input in display-referred range. The original scene-linear RGB and alpha
remain untouched for reconstruction. This separation is required because the
public D3D12 evidence does not establish an unbounded scene-linear Feature 18
contract.

Feature 18 output is decoded in the same proxy space. Open Shaders derives a
luminance/detail ratio between the NR result and the original proxy, bounds that
ratio around invalid or near-black pixels, and applies it to the untouched
scene-linear HDR color. The model cannot replace the frame's hue or alpha, its
output is never inverse-tone-mapped, and the existing downstream tone mapper
remains the only tone mapper whose result reaches the display. This keeps NR
before DLSS for performance while preserving the scene's HDR highlight energy.

## Ownership and synchronization

The host tone filter separates the current NR/input log-luminance difference
into low and high spatial bands. Equal band strengths use the unfiltered
difference directly. Unequal strengths, or a band diagnostic view, prepare a
per-eye `R32G32_FLOAT` texture containing input log luminance and the difference
once per pixel. The 5-by-5 filter reads that texture instead of decoding both
proxy images for each neighbor. This scratch texture carries no frame history;
Feature 18 still evaluates once per eye.

`D3D12Interop` obtains the renderer adapter via `IDXGIDevice::GetAdapter`,
creates a D3D12 direct queue, and opens D3D11-created NT shared textures.
It reuses OS `Texture2D`, `ConstantBuffer`, `LazyShader`, resource naming,
and the existing `EncodeTexturesCS` shader.
The encoder's mask outputs at `u0` and `u1` have full-eye R8 UNORM scratch
UAVs, reused sequentially across eyes; motion and depth occupy `u2` and `u3`.

The shared fence orders D3D11 input writes, D3D12 evaluation, and D3D11
output copies. D3D11 flush submits the input signal without waiting on
the CPU. Shared resources transition from COMMON to shader reads/UAV and
back to COMMON before D3D11 consumes them. Three command allocators are
reused only after their completion values retire. CPU waits occur for
allocator backpressure, history resets, recreation, and teardown. Changing
the historical-camera diagnostic option also drains the queue before releasing
the existing Feature 18 handles. Both eyes must succeed before either result
is copied back. D3D11 pipeline state is restored on all exit paths.

`Runtime` caches the NR and NGX core exports at initialization. Handles,
parameter objects, loaded modules, and COM resources have RAII owners.
The NR-only IAT hook is installed once, uses a thread-local scope around
NGX calls, and restores the original import when the runtime is destroyed.
It does not patch Skyrim or the existing Streamline imports.
Its synthetic `nvngx.dll` caller-path string is solely a compatibility identity;
it is not a dependency and no physical `nvngx.dll` is required in the runtime
directory. Initialization commits ownership only after parameter allocation
completes. Any earlier failure releases allocated parameters, shuts down a
successfully initialized NGX session, restores the IAT, and unloads the modules.

## Verification boundary

Compile the universal `CommunityShaders` target with shader tests disabled.
Then compile the NR shaders for the working-tree diff:
`cmake --build <build-dir> --config Release --target validate_changed` (see
[Shader Development Workflow](shader-workflow.md#incremental-shader-validation)
for the target's prerequisites and config override; the target exists only
when the build is configured with `AUTO_PLUGIN_DEPLOYMENT` or `AIO_ZIP_TO_DIST`,
for example the `ALL-WITH-AUTO-DEPLOYMENT` preset). That step reaches
`ColorTransferCS.hlsl`, which the C++ build does not compile.

Runtime validation still requires the supported NVIDIA hardware/runtime and
manual testing in both VR and SE/AE: native and scaled rendering, eye
independence, toggling, loading, abrupt camera changes, and resolution
recreation. HLSL validation establishes only that the shaders compile: it does
not establish image quality, runtime compatibility, or GPU correctness, all of
which require the hardware run.

## Flicker diagnostics

Enable developer mode, then click **Run All NR Tests** in Upscaling's NR section,
close the menu, and reproduce the problem. The overlay labels eight tests of 600
world frames each:

1. Baseline.
2. Apply inferred camera-cut resets.
3. Apply inferred direction/projection resets while ignoring position.
4. Force a reset every frame.
5. Zero NR motion vectors (most useful while stationary).
6. Zero NR jitter parameters (the renderer's jitter remains active).
7. Serialize GPU execution using an explicit retirement wait; expect lower FPS.
8. Bypass NR output writeback while continuing NGX evaluation.

Repeat the same standing-still, turning, and walking pattern in each phase.
The sequence pauses when the OS menu is open, the world is inactive, NR is off,
or the game is paused. **Stop Tests and Restore** ends it early. Completion and
stopping restore the options selected before the sequence. Most diagnostic
changes apply live and reset history once. Changing **Feed historical camera
parameters** also recreates Feature 18 after retiring queued work. Loading,
frame-gap and resource resets remain active when inferred camera-cut resets are
disabled.

All diagnostic switches can also be changed manually during the same game session.
They are session-only, default off, and **Restore Diagnostic Defaults** clears
them. Toggling **Feed historical camera parameters** retires queued GPU work and
recreates the Feature 18 handles because it changes the frame-data contract.
**Copy Trace Path** copies the location of the suite's timestamped text file in
the Windows temporary directory. A game restart does not overwrite it. An
incomplete sequence still saves completed phases and periodically flushes its
current phase.

Each trace records options, scheduling, NGX results, detected and applied reset
bits, current/previous camera coordinates, engine previous camera coordinates,
inverse-view translation, position distance, direction dot product, projection
change, NR jitter and frame time per eye. The cut thresholds are distance >256,
direction dot <0.5 and projection change >0.1. Reset bits are 1=requested,
2=first frame, 4=frame gap, 8=camera position, 16=camera direction, 32=projection,
and 64=feature creation. Periodic summaries also use `[NRDiag/v2]` in
`OpenShaders.log`.

The overlay shows the last 120 engine frames, including missing hooks, and is
controlled by **Show NR Diagnostics** and the global overlay setting.
`COPY QUEUED` means the CPU submitted the output copy. These observations do not
validate GPU pixels or detect later writes to the same texture. The tests narrow
suspects; a visual difference is evidence to investigate, not proof of a fix.
