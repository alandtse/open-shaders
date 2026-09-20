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
    correspondence with the center-pixel depth guide. OS motion vectors already
    use the normalized units expected by the upscaler, so Feature 18 receives
    identity motion-vector scales. Reset frames submit zero motion because their
    previous history is invalid.
-   Frame data: diagnostics observe cached camera position, view direction, and
    projection changes. These inferred cuts do not reset production history.
    No undocumented
    jitter, frame-time, or camera-matrix aliases are supplied to Feature 18.
    Before a history reset, prior NR GPU work is retired. Frames retaining
    history keep GPU-only interop ordering.
-   Loading transitions, skipped world frames, enable changes, shader
    invalidation, and resource recreation invalidate history. Ordinary UI menus
    continue evaluating NR when the world rendered that frame; UI composition
    remains later in the frame and is not passed through Feature 18.
    Resolution, format, eye-count changes, or explicit resource recreation retire
    GPU work and recreate the eye resources and NGX instances together. Changing
    only the engine texture pointer does not recreate NR.

The Feature 18 parameter contract and HDR behavior were checked against the
[DLSS5VKLayer contract notes](https://github.com/bmitch87/DLSS5VKLayer/blob/main/extracted_pipeline_notes.md)
and its current implementation. Feature 18 uses a private runtime interface;
the version gate bounds this implementation to its observed ABI.

## Experimental native HDR contract

The native-HDR D3D12 route described below failed image-quality validation with
the 310.8 DVS Production runtime. It must not be treated as an established
Feature 18 contract. It accepts feature creation and evaluation but produces
unstable colored blocks around scene-linear highlights and incorrect localized
relighting when Local Tone or Local Structure is active.

The rejected experiment created every Feature 18 instance with
`IsHDR | DoSharpening | AutoExposure` (`0x61`) in `Feature_Flags` and
`NVSDK_NGX_Parameter_Feature_Flags`, plus `DLSSNR.Upscaling=0u`,
`DLSSNR.Hdr=1u`, `DLSSNR.SDR=0u`, and `DLSSNR.AutoExposure=1u` for the
same-resolution HDR path. `InPreExposure`,
`InExposureScale`, `NVSDK_NGX_Parameter_PreExposure`, and
`NVSDK_NGX_Parameter_ExposureScale` are floats initialized to `1.0f`.
Each parameter object is populated through the runtime's
`NVSDK_NGX_D3D12_PopulateParameters_Impl` before the create contract is written.
The per-eye parameter object retained these creation values across evaluations.
Frame resources and resets are updated in place. Appearance tuning is written before
creation and UI edits recreate the persistent eye handles after the edit is committed.
The experiment always used HDR model input, including
when the final display is SDR; there is no live HDR/SDR model switch or reuse of
an SDR instance. Any future contract switch must retire GPU work and recreate the
eye handles. An HDR creation failure pauses NR with its NGX result in the UI/log;
it does not retry through an SDR approximation.

The contract follows the public
[DLSS5VKLayer creation code](https://github.com/bmitch87/DLSS5VKLayer/blob/main/core/ngx_snippet.cpp#L509-L523)
and [HDR input description](https://github.com/bmitch87/DLSS5VKLayer#hdr-input).
Its Vulkan implementation is reference evidence, not a runtime validation of this
D3D12 integration. The bundled public D3D12 requirements API reports general feature
support rather than the private Vulkan structure's HDR feature flags; the latter's
D3D12 ABI has not been established here. No speculative capability query is made,
and a successful create/evaluate alone does not verify correct HDR output.

Independent public implementations do not establish this exact D3D12 HDR route.
[video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr#neural-rendering)
can omit its normal sRGB encoding, but its D3D12 forwarder does not set the
Feature 18 HDR flags used here and its ordinary image inputs remain bounded.
[DLSSNR-Cost-Scaler](https://github.com/xenmods/DLSSNR-Cost-Scaler)
reports operation with scRGB float16 and HDR10/PQ resources without publishing
the resource-value or D3D12 creation contract needed to reproduce it. NVIDIA
has not published a Feature 18 programming guide, so these projects establish an
observed pre-release contract rather than an official compatibility guarantee.

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

## Validated D3D12 color boundary

The public D3D12 contract evidence summarized by DLSS5VKLayer establishes a bounded route:
same-resolution `R8G8B8A8_UNORM` color and output, with Feature 18 operating on
a display-referred image. ComfyUI-DLSS5-NR and obs-dlss5-nr use RGBA16F carrier
textures but explicitly derive them from 8-bit color and clamp the values to
0–1. None of these paths demonstrates unbounded scene-linear HDR through the
signed D3D12 Feature 18 entry points.

The rejected Open Shaders route sent `max(ToLinear(kMAIN), 0) * exposure` without an upper
bound and forces `IsHDR`, `DLSSNR.Hdr=1`, and `DLSSNR.SDR=0`. Those flags and
the model capability negotiation were taken from DLSS5VKLayer's Vulkan path.
That path queries private Vulkan feature requirements and falls back when the
model refuses HDR; no equivalent D3D12 ABI has been established here. Successful
D3D12 create/evaluate results therefore did not validate the color contract.

This mismatch explains the observed boundary conditions: corruption clusters
around fire, emissives, sky, and other values above display white; Local Tone and
Local Structure amplify it; running NR after tone mapping substantially improves
it; temporal resets, motion vectors, jitter, serialization, DLAA/DLSS selection,
and an output-channel swap do not repair it.

The current pre-upscale integration is a bounded, display-referred NR proxy at the
render resolution. Feature 18 operates on that proxy using the proven SDR D3D12
contract. Its output is not inverse-tone-mapped. Instead, Open Shaders derives a
luminance/detail ratio between the NR result and the original proxy, bounds that
ratio against invalid or near-black pixels, and applies it to the untouched
scene-linear HDR color. This preserves the original hue and HDR highlight energy,
keeps Feature 18 before DLSS for performance, and leaves the existing downstream
tone mapper as the only tone mapper whose output reaches the display.

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
allocator backpressure, history resets, recreation, and teardown. Both eyes must succeed
before either result is copied back. D3D11 pipeline state is restored on
all exit paths.

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
stopping restore the options selected before the sequence. Changes apply live,
reset history once, and do not recreate Feature 18. Loading, frame-gap and
resource resets remain active when inferred camera-cut resets are disabled.

All switches can also be changed manually during the same game session. They are
session-only, default off, and **Restore Diagnostic Defaults** clears them.
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
