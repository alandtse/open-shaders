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
-   SkyrimVR launchable with RenderDoc injected (devbench MCP when available, or launch+inject
    from the RenderDoc GUI). The original VR TAA artifact is visible at the **main menu**, so the
    menu is a valid capture surface.
-   `fxc.exe` (Windows SDK) — same compiler the offline verifier uses.

## Steps

### 1. Compile A' (current) and B (candidate) to DXBC — match the build's permutation

Use the SAME defines as the running build (SkyrimVR ⇒ `VR`; add `HDR_OUTPUT` only if the user
runs the HDR path) and `/I package/Shaders` so includes resolve. Feeding DXBC sidesteps
RenderDoc's HLSL include/define handling.

```powershell
$fxc = (Get-Command fxc.exe).Source
$inc = "package/Shaders"; $sh = "package/Shaders/ISTemporalAA.hlsl"
# Baseline A' = the shipping shader at this permutation (validates our define set)
& $fxc /nologo /T ps_5_0 /E main /D PSHADER=1 /D VR=1 /I $inc $sh /Fo "$env:TEMP\taa_A.dxbc"
# Candidate B = the Standard-B working tree (or a ref checked out first)
& $fxc /nologo /T ps_5_0 /E main /D PSHADER=1 /D VR=1 /I $inc $sh /Fo "$env:TEMP\taa_B.dxbc"
```

### 2. Capture a frame

Launch SkyrimVR to the main menu with RenderDoc attached and capture one frame. Confirm with
`Instance list` (the `renderdoc` MCP) that an instance shows `capture_loaded: true`.

### 3. Load the harness and run the A/B (via the `renderdoc` MCP `Eval`)

The embedded interpreter persists across `Eval` calls, so load the module once:

```python
exec(open(r"f:/Worktrees/.../tools/taa-renderdoc-ab.py").read())
taa_candidates()          # find the ISTemporalAA draw: 2 outputs (Color+Feedback), >=5 SRVs
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
