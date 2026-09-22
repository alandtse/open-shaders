# Native Neural Rendering MVP

Upscaling owns `NeuralRendering`, which evaluates NGX Feature 18 once per rendered eye
inside `Upscaling::Main_PostProcessing`, before DLSS/FSR upscaling, VR submit capture,
and frame-generation input capture. It processes the active render-resolution regions
of `kMAIN` and writes them back into the existing pipeline. NR is not evaluated at
the larger display resolution when render scaling is active.

The implementation owns its render-resolution integration directly and does not
depend on the VR submit-upscaling implementation from PR #625.

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
Runtime validation requires the supported NVIDIA hardware/runtime and
manual testing in both VR and SE/AE: native and scaled rendering, eye
independence, toggling, loading, abrupt camera changes, and resolution
recreation. A C++ build alone does not establish image quality, runtime
compatibility, or GPU correctness. No shader tests or validation are
required by this document.

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
`CommunityShaders.log`.

The overlay shows the last 120 engine frames, including missing hooks, and is
controlled by **Show NR Diagnostics** and the global overlay setting.
`COPY QUEUED` means the CPU submitted the output copy. These observations do not
validate GPU pixels or detect later writes to the same texture. The tests narrow
suspects; a visual difference is evidence to investigate, not proof of a fix.
