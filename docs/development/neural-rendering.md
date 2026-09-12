# Native Neural Rendering MVP

Upscaling owns `NeuralRendering`, which adds one NGX Feature 18 evaluation per rendered eye to
`Upscaling::Main_PostProcessing`, after OS pre-upscale effects and before
frame-generation input capture, ordinary upscaling, or VR submit input
capture. The output replaces the same active regions of `kMAIN`.

The implementation is layered on PR #625 at
`659913c8da5e4b018febc1576a2abe1ca23f9eb7`.

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

The panel exposes **Intensity**, **Local Tone**, and **Local Structure** (0–2,
default 1.7), plus **Skin Structure** (-1–2, default -1 for the runtime default).
These persist in `Upscaling.neuralRenderingTuning` and take effect on the next
NR evaluation, with temporal history reset. Invalid/nonfinite config values are
bounded or restored to defaults. **Restore NR Defaults** resets these four controls;
**Reset NR History** invalidates both eye histories without changing tuning.

Both VR and SE/AE use this hook. In VR there are two persistent Feature 18
instances; in flatrim only eye zero is evaluated. NR input and output
dimensions always match. At native render scale this is full eye
resolution. With render scaling enabled, NR runs over the complete
pre-upscale eye at the engine's current render dimensions; DLSS/FSR and
the existing post chain follow normally. There is no separate reduced
resolution NR setting or post-upscale route.

## Resource and temporal contract

-   Color: the active `kMAIN` region, preserving RGBA16 float or RGBA8 UNORM.
-   Depth: the existing upscaling encoder reads the engine depth SRV,
    including VR's R24 depth view, and writes non-inverted device depth to
    R32 float. Depth is not linearized.
-   Motion: the same encoder's DLSS dilation writes RG16 float. OS motion
    vectors are unjittered previous-minus-current eye UV displacement, so
    Feature 18 receives the eye width and height as pixel conversion scales.
-   Frame data: OS jitter, delta time, and cached unjittered eye view and
    projection matrices are supplied through the existing NGX parameter
    names. Camera cuts are detected from cached position, direction, and
    projection changes.
-   Loading transitions, skipped world frames, menus, enable changes,
    shader invalidation, and resource recreation invalidate history.
    Resolution or source texture changes retire GPU work and recreate the
    eye resources and NGX instances together.

The runtime-specific parameter contract and caller-path compatibility
behavior were checked against
[OpenNR at 29d9218](https://github.com/olekspa/OpenNR/tree/29d9218f17daf323cf67996d40b0210a7ea6e6b8/src/Features/Upscaling/NeuralRendering).
Feature 18's interface is undocumented. The reference establishes its
`DLSSNR.*` resource, extent, motion-scale, reset, and appearance parameters.
The supplemental standard NGX jitter/time/matrix keys are supplied, but
their consumption by Feature 18 is not established by that reference.
The version gate bounds this implementation to its observed ABI.

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
allocator backpressure, recreation, and teardown. Both eyes must succeed
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
