# Skyrim VR Save/Load Hang Assessment

Date: 2026-04-26
Branch/context: `cs-1.4.11-PL-VR`

## Symptom Summary

The reproducible failure is not a normal crash. Skyrim VR starts loading a save, the HMD leaves the in-game load screen, SteamVR shows the room with "waiting for SkyrimVR", and the game never recovers until force quit.

The machine also becomes input-laggy/funky while this is happening. Keyboard and mouse behavior returning after Skyrim VR is killed or restarted is consistent with the game or GPU driver being stuck in a high-impact render/device/compositor state, not just a harmless worker-thread stall.

Important observed pattern:

- New characters can save and load repeatedly from different positions.
- Existing test saves can become unloadable after development churn: swapping CS builds, changing shaders/features, and repeatedly loading older saves.
- The failure is often shown as `3/6`, `6/10`, or similar shader compile progress, but not always.
- Interiors are generally safer than exteriors.
- Some failing cases were narrowed to the engine present caller `SkyrimVR.exe+0xDBBE19`, but bypassing that mirror-present caller did not solve the issue.

## What We Tried And Learned

### Shader cache is probably not the root cause

The initial suspicion was a shader-cache or compile-progress deadlock during save load.

The old watchdog commit `27a99b0b` was meant to detect no shader compile progress during `kDataLoaded` and switch to background compilation after 90 seconds. That would help if the main thread was stuck waiting for the compile queue during data loading. It does not address stalls after data loading, after rendering begins, or inside VR/upscaling/compositor code.

The later post-load shader diagnostics showed a key result: in at least one failing or near-failing run, the post-load shader tasks completed successfully. The log showed shader tasks being queued, read from disk, created as D3D shaders, and completed. The final useful line was later in the upscaling path, after VR intermediate texture creation.

Conclusion: the visible `3/6` or `6/10` counter can be a symptom or stale UI state. It may be where the user sees progress stop, but it is not sufficient evidence that the shader compiler is the actual deadlock.

### The mirror DXGI Present caller is not sufficient

We found `swapchain.present.return_to_engine caller=SkyrimVR.exe+0xDBBE19`. That showed the engine was returning from DXGI Present at a repeatable Skyrim VR address.

A debug guard was added to bypass a small number of mirror presents from that caller after save load. It did not resolve the hang.

Conclusion: `SkyrimVR.exe+0xDBBE19` is likely a visible frame boundary or mirror-present path, not the root cause. It can still be useful context, but fixing that one caller is not enough.

### Current strongest signal points after save-load reset into VR/upscaling/resource handling

In the best diagnostic run, the shader-cache work completed, and the last useful line was:

`[Upscaling] Created VR intermediate textures: per-eye in ... out ...`

The next code region is the VR upscaling per-eye preparation path:

- `PreparePerEyeInputs`
- `EnsureVRIntermediateTextures`
- multiple `CopySubresourceRegion` calls for per-eye color/depth/motion/reactive/transparency buffers
- first-use setup of `ClearHMDMaskCS`
- constant buffer creation
- compute dispatches to clear hidden-area mask data
- then DLSS/FSR/upscaler evaluation and output copy

The HMD falling back to SteamVR's waiting room means Skyrim VR is not submitting frames to the compositor. That aligns with a stall in the render path after loading, not with a pure cache validation issue.

## Most Likely Issue List

### 1. Post-load VR upscaling resource/state deadlock or GPU stall

Likelihood: high.

Why it fits:

- Last strong log point is immediately before more VR upscaling work.
- The failure presents as SteamVR waiting forever, meaning the app stopped submitting frames.
- Exteriors are worse, likely because they trigger more render target changes, shader permutations, terrain/lighting resources, and post-processing pressure.
- New characters work because their loaded world/render state is cleaner and less likely to hit the problematic transition.
- Development churn can leave old saves loading into render states that no longer match current CS feature/resource assumptions.

Possible fixes:

- On `kPostLoadGame` and `kNewGame`, hard-reset VR upscaling runtime state:
  - release per-eye intermediate textures,
  - release/recreate DLSS/FSR resources,
  - clear cached frame tokens/options,
  - clear Reflex/PCL marker state,
  - reset foveated/upscaling per-eye resource dimensions.
- For the first few frames after save load, bypass expensive VR upscaling work and keep submitting simple frames. Re-enable upscaling only after stable render dimensions and at least several successful presents/submits.
- Avoid first-use direct shader compilation during the first post-load render frame. `ClearHMDMaskCS` should be compiled earlier, cached, or handled through the normal shader cache path.
- Add narrow logs around `PreparePerEyeInputs`, the per-eye copies, `ClearHMDMaskCS` compile/create, dispatch, and unbind. This should be normal file logging and flushed, not a separate risky trace system.

### 2. Stale dev cache or shader/resource identity mismatch

Likelihood: high-medium.

Why it fits:

- The issue often appears after switching builds, shader sources, or feature code during development.
- Some saves worked before, then stopped after repeated CS version/shader changes.
- New character saves keep working, suggesting the save content triggers a specific combination of runtime resources rather than every load being broken.
- Disk shader cache validation may not cover every relevant input: dev build identity, direct-compiled helper shaders, Streamline/FidelityFX DLL version, VR/upscaling mode, and hidden feature state.

Possible fixes:

- Strengthen cache namespace/invalidation for dev builds:
  - include plugin version,
  - shader source tree stamp,
  - feature versions,
  - relevant VR/upscaling settings,
  - Streamline/FidelityFX runtime versions,
  - optionally git commit/build id for local development builds.
- Put helper shaders such as `ClearHMDMaskCS` under the same invalidation discipline as other shaders.
- Use temp-file then rename for disk cache writes so force quits cannot leave partial cache files that look valid.
- Consider a development-only cache namespace, so switching branches/builds cannot reuse stale cache artifacts from another build.

### 3. Shader compile progress is a secondary symptom, not always the lock

Likelihood: medium.

Why it fits:

- The UI can stop at `3/6` or `6/10`, but logs have shown all post-load shader tasks can complete.
- If the render loop stalls shortly after queuing or completing tasks, the visible counter may not refresh even though the compile queue is not the real blocker.
- Some failures happen without a shader-stuck signature.

Possible fixes:

- Keep the post-load shader diagnostic lines for now, but do not keep expanding shader-cache logging blindly.
- Add a real compile heartbeat that records active task key, queue depth, completed/failed counts, and last progress timestamp.
- If no compile progress occurs while the main thread is waiting, fail open by switching to background compilation rather than blocking load.
- Do not force a full 3600-shader rebuild unless specifically testing cache invalidation. It hides the smaller post-load signal.

### 4. SteamVR/OpenVR/Streamline/Reflex interaction after load

Likelihood: medium.

Why it fits:

- The visible failure is SteamVR waiting for Skyrim VR.
- Previous tracing suggested the stall was not directly inside OpenVR submit, pose wait, DLSS evaluation, or DXGI Present for that specific run.
- However, Streamline/Reflex/PCL and VR compositor timing can still contribute if markers/sleeps/resource transitions are called at the wrong time during save-load recovery.

Possible fixes:

- Disable Reflex sleep and PCL marker optimization for a short post-load window.
- Re-enable them only after successful frame submission resumes.
- Log Streamline `UpdateReflex`, PCL marker, DLSS evaluate, and present boundaries only during post-load recovery.
- Provide a debug INI switch to disable Reflex/PCL during save-load without disabling DLSS entirely.

### 5. Another SKSE mod or engine hook conflict exposed by CS timing

Likelihood: medium-low, but still possible.

Why it fits:

- The caller address is inside Skyrim VR, but other SKSE plugins may hook nearby render/update functions.
- The issue develops during active mod/plugin development, where load order, binaries, and render hooks may change.
- CS may be the trigger because it changes timing/resource pressure, even if the final deadlock involves another hook.

Possible fixes:

- If CS-side mitigations do not change behavior, run a mod-bisect using the same bad save:
  - keep CS and required dependencies,
  - disable other render/input/VR/compositor SKSE plugins first,
  - then reintroduce in groups.
- Add caller/module logging around the next render/update boundary only if needed.
- Use external tools such as GPUView/PresentMon/ETW only after CS logs identify the last in-process boundary.

## Recommended Fix Direction

The most practical fix direction is no longer "make shader cache safer" by itself. The better target is:

1. Treat save-load in VR as a hostile render transition.
2. After `kPostLoadGame`, force a clean VR/upscaling runtime reset.
3. Submit simple/stable frames for a short grace period.
4. Recreate DLSS/FSR/upscaling resources lazily after dimensions stabilize.
5. Disable Reflex/PCL marker/sleep behavior during that same grace period.
6. Move direct helper shader compilation out of the first post-load frame.

This turns the failure mode from "hang while SteamVR waits forever" into either:

- a few frames without upscaling, then normal recovery, or
- a logged and recoverable upscaling failure where CS falls back to the original image path.

## Logging Strategy From Here

Do not add broad minimized trace systems again. The logging must be plain, file-backed, and placed only at the next unknown boundary.

The next useful markers are:

- post-load reset enter/exit,
- VR/upscaling post-load grace state armed,
- `PreparePerEyeInputs.enter/exit`,
- per-eye copy begin/end,
- `ClearHMDMaskCS` compile/create begin/end,
- mask dispatch begin/end,
- DLSS/FSR region upscale begin/end,
- output copy begin/end,
- first successful post-load frame/present/submit after recovery.

If the game hangs, the last flushed marker is the boundary to fix. If no marker appears, the failure is before that instrumentation and should be moved one boundary earlier.

## Testing Guidance

- Do not clear the 3600-shader cache unless the test is specifically cache invalidation.
- Use the known bad save with the current cache.
- If the HMD drops to SteamVR waiting room and no new log lines appear for 60-90 seconds, force quit. Waiting longer has not produced useful recovery evidence so far.
- Preserve the log immediately after force quit.
- Compare against a new-character save from the same build to confirm whether the mitigation changes only the bad-save path or affects all loads.

## Current Bottom Line

The most likely root is a post-save-load VR render/upscaling resource transition problem, amplified by development cache/state churn. Shader compilation is still involved in the visible timing, but it is probably not the primary deadlock in the strongest logs we have.

The highest-value fix is a robust post-load VR/upscaling reset plus a short safe rendering grace period, with helper shader compilation moved out of the first post-load frame. Shader-cache hardening remains useful, but continuing to chase only the shader compile counter is unlikely to solve the SteamVR waiting-room hang.
