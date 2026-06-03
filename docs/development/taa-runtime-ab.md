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

### 2. Capture a frame

Launch SkyrimSE or SkyrimVR to the main menu with RenderDoc attached and capture one frame.
Confirm with `Instance list` (the `renderdoc` MCP) that an instance shows `capture_loaded: true`.

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

## Caveats

-   One frame proves equivalence on that frame; loop over a few captured frames (different menu
    animation phases) for confidence.
-   The `HDR_OUTPUT` define must match the user's actual HDR setting, or `baseline_vs_live` will
    flag it.
-   This is a regression check against a known-good baseline, not a formal proof; pair it with
    the offline verifier (which formally covers every part that stays bytecode-identical).
