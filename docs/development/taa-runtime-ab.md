# TAA runtime A/B check (Tier-3)

A same-frame, GPU-level equivalence check for `ISTemporalAA.hlsl` refactors whose compiled
bytecode legitimately diverges (the **Standard B** restructure of the bracket/flicker/blend
core). For refactors that stay bytecode-identical, use `tools/verify-shader-refactor.ps1`
instead — it is a hard offline proof and needs no game.

## Why same-frame swap (not two-launch screenshots)

TAA is temporal: its output depends on accumulated history. Two separate game launches never
align frame-for-frame (animated menu, HMD pose, timing/RNG), so a screenshot A/B only yields a
noisy tolerance diff. Instead we capture **one** real frame in RenderDoc and replace just the
TAA pixel shader on that captured frame. The inputs (history `t1`, velocity `t2`, depth `t3`,
mask `t4`, alpha `t5`, cbuffer `b2`) are frozen, so A (shipping) and B (candidate) run on
byte-identical inputs — a near-zero output diff means equivalent behavior on a real frame.

This is the runtime analog of the offline verifier, tolerant of the bytecode divergence that
defines Standard B.

## Prerequisites

-   RenderDoc, reachable via the `renderdoc` MCP (`Eval`, `Get-Texture`, `Instance`).
-   SkyrimSE or SkyrimVR launchable with RenderDoc injected (`rd.ExecuteAndInject(skse64_loader.exe,
…, hookIntoChildren=True)`, or launch from the RenderDoc GUI). The **main menu** already runs
    the TAA pass, so it is a valid capture surface on either edition.
-   The build must be in **TAA upscaling mode** (DLSS/FSR replace `ISTemporalAA`) and ideally with
    **HDR and frame-generation off** — otherwise Open Shaders presents through a DX12 interop
    swapchain and RenderDoc captures only the D3D12 present (Copy/Present, no draws), not the
    D3D11 TAA pass.
-   `fxc.exe` (Windows SDK) — same compiler the offline verifier uses.

## Steps

### 1. Compile A' (current) and B (candidate) to DXBC — match the build's permutation

Use the SAME defines as the running build (SkyrimSE ⇒ no `VR`; SkyrimVR ⇒ `VR`; add `HDR_OUTPUT`
only if HDR is on) and `/I package/Shaders` so includes resolve. Feeding DXBC sidesteps
RenderDoc's HLSL include/define handling.

**A′ and B must come from different sources** — A′ from the **deployed/shipping** shader (so its
diff vs the live RT establishes the noise floor), B from your **candidate**. Pull A′ from a git
ref and B from the working tree so they can't accidentally be the same file:

```powershell
$fxc = (Get-Command fxc.exe).Source
$inc = "package/Shaders"; $sh = "package/Shaders/ISTemporalAA.hlsl"
$defs = @("/D","PSHADER=1")            # add "/D","VR=1" and/or "/D","HDR_OUTPUT=1" to match the build
# Baseline A' = the DEPLOYED shader (extract the shipping ref to a temp file; UTF-8, not PS UTF-16)
[IO.File]::WriteAllLines("$env:TEMP\taa_A.hlsl", (git show origin/dev:$sh))
& $fxc /nologo /T ps_5_0 /E main @defs /I $inc "$env:TEMP\taa_A.hlsl" /Fo "$env:TEMP\taa_A.dxbc"
# Candidate B = the Standard-B working tree
& $fxc /nologo /T ps_5_0 /E main @defs /I $inc $sh /Fo "$env:TEMP\taa_B.dxbc"
```

**Baseline = the refactor's BASE, not blindly `origin/dev`.** If your refactor is _stacked_ on a
fix/feature not yet on `dev` (e.g. a VR fix the restructure sits on top of), compiling A′ from
`dev` makes `baseline_vs_live` measure _that fix's effect_, not the compiler noise floor — you'll
see a huge "floor" (e.g. ~0.22 instead of ~0.001) and a meaningless `EQUIVALENT`. Use the
refactor's **parent commit** (`git show <refactor-base>:$sh`) so A′ and B differ only by the
restructure.

**Confirm `git branch` before compiling B.** A wrong-branch compile silently builds the deployed
shader as the "candidate" and reports a meaningless `EQUIVALENT`.

### 2. Capture a frame — MOTION matters

Confirm with `Instance list` (the `renderdoc` MCP) that an instance shows `capture_loaded: true`
after capturing.

-   The **main menu** runs the TAA pass, but its history rectification is near-passthrough on a
    static image. **A menu frame does NOT validate any change that flows through the motion-
    dependent path** (reject / history reproject / clip-to-AABB) — it reads a false `EQUIVALENT`.
    Use a menu frame only for changes you already know are motion-independent. _(This is how a
    gate-inversion bug once slipped through: byte-for-byte `EQUIVALENT` on the menu, ~4× off under
    motion — caught only by re-running on an in-game motion frame and bisecting the commits.)_
-   For anything touching the blend/reject core, capture an **in-game frame with real motion**.
    Via `dev-bench` + `renderdoc`: load a light **interior** save, then `record replay` a movement
    recording (continuous teleport along a path = sustained motion vectors) and fire
    `TargetControl.TriggerCapture(1)` ~3 s into the replay from a **concurrent** `Eval` (the replay
    call blocks for its whole duration, so the trigger must run in parallel). `camera setPov vanity`
    gives orbiting motion on SE; in VR the HMD drives the camera, so **player translation is the
    reliable VR motion source**.
-   **Capture size is the gate.** SE in-game ≈ 1.5 GB; VR interior ≈ 0.5–5.5 GB (loads fine); VR
    **exterior ≈ 14 GB and wedges/crashes RenderDoc** — stay indoors (e.g. Dragonsreach). A
    corrupt/oversized `.rdc` needs a full RenderDoc relaunch, not an MCP reconnect.
-   `TargetControl` sometimes returns a **stale** `NewCapture` path — verify the new `.rdc` by
    timestamp/size on disk, don't trust the returned string.
-   On a huge frame, find the TAA pass by scanning drawcalls **in reverse** (post-process is near
    the end) — a full-frame `SetFrameEvent` sweep times out the eval worker.

### 3. Load the harness and run the A/B (via the `renderdoc` MCP `Eval`)

The embedded interpreter persists across `Eval` calls, so load the module once:

```python
exec(open(r"f:/Worktrees/.../tools/taa-renderdoc-ab.py").read())
taa_candidates()          # find ISTemporalAA by its 6-SRV fingerprint (t0..t5: current/history/velocity/depth/mask/alpha)
```

Pick the `eventId` of the TAA draw from the candidate list, then:

```python
ab(<eventId>,
   candidate_dxbc=r"%TEMP%/taa_B.dxbc",
   baseline_dxbc=r"%TEMP%/taa_A.dxbc")
```

### 4. Interpret

**Always pass `baseline_dxbc`** — the verdict is _relative to it_. The game compiles the live
shader with slightly different optimization than offline fxc, so even an identical shader leaves
a small **noise floor** (`baseline_vs_live.mean_abs`, e.g. ~2e-4 on a 10-bit RT). `ab()` reports:

-   `noise_floor_mean` — the baseline residue.
-   `verdict`: `EQUIVALENT` if `candidate_vs_live.mean_abs ≤ 3× the floor`, else `DIFFERS`.

Validated on the SE main menu (TAA on, HDR/frame-gen off): the pre-rename `dev` shader read
`EQUIVALENT` (mean = floor), a deliberately ×0.5 shader read `DIFFERS` (mean ≈ 83× floor, 61%
of samples). If `baseline_vs_live.mean_abs` is _large_ (not a tiny floor), the permutation is
wrong (e.g. `HDR_OUTPUT` mismatch) — fix step 1 before trusting the candidate.

### 5. (Optional) visual evidence

Use the `Get-Texture` MCP tool on the output RT before vs after replacement (and over the
`x≈0.5` eye-seam region where the original VR bug showed) with `diff_amplify` for a diff image
in the report.

## Caveats & lessons learned

-   **Reach for the offline verifier first.** Far more refactoring stays bytecode-identical than
    you'd expect — even destructuring deeply-aliased decompile scratch (where one float4 component
    means different things at different points) compiles **byte-identically**, because the compiler
    already SSA's it. Go one small step at a time and run `verify-shader-refactor.ps1` (or a per-
    permutation `fxc` byte-compare) after each. Byte-identity is a _stronger_ proof than this
    runtime A/B and needs no game/RenderDoc. Reserve this harness for the genuine op-reordering
    core (the bracket/flicker/blend restructure) where fxc legitimately emits different code.
-   One frame proves equivalence on that frame; loop over a few captured frames for confidence.
-   The `HDR_OUTPUT` define must match the user's actual HDR setting, or `baseline_vs_live` flags it.
-   **The HDR permutation can't be captured at all** — with HDR on, Open Shaders presents through a
    DX12 interop swapchain and the D3D11 TAA pass isn't in the frame. Validate the `HDR_OUTPUT`
    path by byte-identity + static analysis instead (the SDR path's runtime A/B covers the shared
    math; HDR-only blocks are usually value-identical renames).
-   **MCP `Eval` quirks (vary by server/version):** (1) if `ab`/`taa_candidates` are undefined on a
    later call, your server uses a **fresh namespace per `Eval`** — `exec` the harness _and_ call it
    in the **same** `Eval` (`g = dict(globals()); exec(src, g); g["ab"](...)`). (2) Output can lag
    **one call behind** (you get the _previous_ call's stdout); poll again to drain it.
-   This is a regression check against a known-good baseline, not a formal proof; pair it with the
    offline verifier (which formally covers every part that stays bytecode-identical).
