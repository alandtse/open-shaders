# Shader decomposition & compile-time plan

Execution plan for restructuring the monolithic base shaders. Written to be executed
by lower-capability agents: every step is mechanical, gated by a tool that returns
a binary verdict, and has an explicit stop-and-escalate condition. No step asks the
executing agent to judge whether a bytecode difference "looks okay."

## Honest framing — what each goal can and cannot buy

The request couples three goals: **maintainability**, **byte-identical**, and
**cuts compile time**. They do not all come from the same transforms:

| Transform                                                                            | Maintainability | Byte-identical                                  | Compile time                                           | Lower-model safe                    |
| ------------------------------------------------------------------------------------ | --------------- | ----------------------------------------------- | ------------------------------------------------------ | ----------------------------------- |
| Cut-paste decomposition into `.hlsli` sections                                       | ✅ large        | ✅ guaranteed (same preprocessed text)          | ≈0 (parse cost is noise; fxc cost is optimization)     | ✅                                  |
| Tightening `#if` guards so permutation-dead code isn't parsed/optimized              | ➖ mild         | ✅ when the code was dead (fxc DCE'd it anyway) | ✅ modest — must be measured, not assumed              | ✅ with the gate                    |
| Restructuring live code (register-lifetime narrowing, loop-ization, MTLand×TRUE_PBR) | ➖              | ❌ bytecode **will** differ                     | ✅ this is where the 180s permutations actually shrink | ❌ — escalate (issue #149, Phase 3) |

So: Phases 1–2 below are the lower-model program — they deliver maintainability
plus a _measured, probably modest_ compile-time win, with a hard byte-identical
gate. The big compile-time win (MTLand×TRUE_PBR and friends) is **out of scope
for the subagent fleet** and stays in #149 as senior/high-capability work, because
it cannot be byte-identical and verification shifts to asm review + runtime A/B
(`tools/taa-renderdoc-ab.py`, SE+VR in-game). Any plan claiming all three goals
from one mechanical pass would be lying about one of them.

## Measured cost surface (what to attack)

The runtime emits per-permutation `[ShaderTiming]` lines (compile ms, define
set) to `CommunityShaders.log`. A full recompile measured 2026-06-11 (SE and VR
logs) gives the real baseline:

| Shader:Class       | SE perms |        SE total | VR perms |         VR total |
| ------------------ | -------: | --------------: | -------: | ---------------: |
| **Lighting:Pixel** |      552 | **9550s (98%)** |      774 | **11891s (98%)** |
| Water:Pixel        |      348 |             88s |      352 |              65s |
| Utility:Vertex     |      340 |             57s |      372 |              65s |
| Utility:Pixel      |      165 |             27s |      183 |              27s |
| Effect:Pixel       |      914 |             21s |      930 |              18s |
| everything else    |          |            <20s |          |             <20s |

**Compile time is a Lighting:Pixel monoculture.** Entry counts mislead — Effect
has 2.5× Lighting's permutations but 0.2% of its cost. Therefore: compile-time
claims (Phase 2) target **Lighting only**; decomposition of Water/Effect/
Utility/RunGrass is justified as maintainability only.

Within Lighting:Pixel (SE): median 12.1s, max 251s.

-   **LANDSCAPE/MULTI_TEXTURE: 16 perms × ~191s mean = 3063s = 32%** of the cost.
    The slowest perm (251s) has **no TRUE_PBR** — the scheduler folklore
    ("MTLand×TRUE_PBR ≥180s") over-credits PBR; the multi-texture landscape path
    combined with the full feature stack (WETNESS/WATER/VOLUMETRIC_SHADOWS/SSS/…
    common defines present in every perm) is the cost. TRUE_PBR's marginal mean
    is +10.8s (26.5s with vs 15.7s without); ANISO_LIGHTING +6.5s; GLINT 49.4s
    mean over 24 perms.
-   **The long tail dominates: 351 perms at 10–60s = 5670s = 59%.** Landscape
    fixes alone cap out at ~32%; general Lighting slimming (cutting what each
    permutation parses/optimizes) attacks the 59%.

Caveat: log timings reflect that machine's flags (developer-mode logs may add
`D3DCOMPILE_DEBUG`); treat them as the targeting map, and re-baseline with the
Phase-0 bench at release parity before claiming any `perf:` number.

## Verification tiers (cheap → expensive)

-   **Tier 0 — preprocessed-text compare**: `fxc /P` both revisions, strip `#line`
    directives, byte-compare. Identical preprocessed text ⇒ identical bytecode at
    every optimization level (fxc embeds no file/line info without `/Zi`). This is
    the gate for pure cut-paste moves and runs in milliseconds per permutation, so
    it can sweep the **full** entry list of a shader cheaply.
-   **Tier 1 — DXBC SHA-256 compare**: `tools/verify-shader-refactor.ps1`.
    ⚠️ Its **default sweep (VR × HDR_OUTPUT) is far too weak** for the big four —
    it compiles a near-empty permutation. Executing agents must pass an explicit
    `-Permutations` list generated from the validation YAML (Phase 0 tool).
-   **Tier 2 — asm diff**: exists in the verify script, but at lower-model
    capability the rule is absolute: **DIFFERS ⇒ revert the step and escalate.
    Never eyeball-approve an asm diff.**

Timing measurements (not correctness) must use **release-parity flags**
(`/O3`, no `D3DCOMPILE_DEBUG`/`SKIP_OPTIMIZATION`) — the validation config's
debug flags make compiles artificially fast and would understate every number.

### Verification cadence: batch + bisect (never per-micro-step)

Verification cost must scale with the number of _failures_, not the number of
changes. The Phase-1 retrospective: gating every section twice per stage, plus
post-hook re-runs, burned ~an hour per shader on redundant sweeps. The protocol:

1. Author a whole batch of changes (each its own commit; no gating while
   authoring beyond compiling in your head).
2. One gate run over the full list with `-FailFast`. IDENTICAL ⇒ done — N
   changes verified for the price of one run.
3. On DIFFERS the script prints the failing permutation as a **bisect probe**:
   `git bisect run` re-testing only that one permutation
   (`-Permutations "<failing>"`) finds the culprit commit in log₂(N) steps of
   seconds each. Drop or fix it, then one final full-list run as the PR proof.
4. Don't re-gate after clang-format hook reformatting — Tier 0 is
   whitespace-immune by construction (fxc `/P` retokenizes); spot-check one
   permutation if paranoid.

The verify script is parallel (`-Jobs`, default cores−2) and caches the
base-ref side per SHA (`build/verify-cache/`). Measured on Lighting: full
1276-perm Tier-0 ≈ 21s cold / 10s warm (was 5–10 min); stratified Tier-1 ≈
64s cold / 40s warm (was 10+ min).

## Phase 0 — Tooling + triage baseline (one capable agent, sequential)

Deliverables, each small and testable:

1. **Permutation-list generator** (`tools/gen-verify-perms.py` or `.ps1`): reads
   `.github/configs/shader-validation.yaml` (and the VR variant), emits per-shader
   `-Permutations` lists for the verify script — `common_defines` minus the
   `D3DCOMPILE_*` pseudo-defines, plus each entry's defines. Two outputs per
   shader: `full` (every entry) and `stratified` (~20 entries: every define
   symbol covered at least once + all scheduler-named hot combos + a fixed-seed
   sample). Fixed seed so re-runs are reproducible.
2. **Tier-0 mode for the verify script** (`-PreprocessOnly`): `fxc /P`, strip
   `^#line.*$`, compare. Exit semantics unchanged (0 identical / 2 differs).
3. **Compile-time bench** (`tools/shader-compile-bench.ps1`): for a shader and a
   permutation list, compile at release parity, record wall-time per entry, emit
   CSV + a ranked table. Given the measured monoculture, the `full` baseline run
   only needs **Lighting** (others optional). This is the baseline all later
   "compile time improved" claims diff against — without it, no `perf:` claims.
4. **ShaderTiming log parser** (can live in the bench script as a mode): parse
   `[ShaderTiming]` lines from a `CommunityShaders.log` into the same CSV shape,
   so in-game recompiles double as measurements.
5. Post the baseline table to the tracking issue.

## Phase 1 — Byte-identical decomposition (parallel subagents, per work unit)

Goal: each monolith becomes an orchestrating top file plus section `.hlsli`
files, e.g. `Lighting.hlsl` → `LightingVS.hlsli`, `LightingLandscape.hlsli`,
`LightingPBR.hlsli`, … (exact split decided per shader by reading the existing
`#ifdef VSHADER` / `#ifdef PSHADER` / technique-block structure).

**Work-unit contract** (what each subagent gets and must do):

1. Input: shader file, an explicit line range constituting one section, the
   target `.hlsli` name, the shader's `stratified` and `full` permutation lists.
2. Transform: **pure cut-paste**. Move the lines verbatim into the new file;
   leave `#include "NewSection.hlsli"` at the original site. No reordering, no
   renaming, no whitespace/comment edits, no "while I'm here" fixes. One section
   per commit.
3. Gate: batch + bisect (see "Verification cadence" above) — commit each
   section, gate the whole batch once with `-FailFast` at the end. On failure,
   bisect with the printed probe permutation, drop the culprit commit, report.
   Do not retry with variations.
4. End of shader (all sections done): one Tier-1 run over the `stratified` list
   as a belt-and-braces check (catches include-resolution surprises that /P
   could mask), plus `hlslkit-compile` on that shader for warning parity.
5. Forbidden: editing `Common/*.hlsli` shared by other shaders (separate,
   sequential work units if ever needed); touching `featuresIndices` /
   descriptor logic; editing more than the assigned range.
6. Forbidden, git: any history surgery beyond creating your own section
   commits — no `reset`, no `rebase`, no amending or dropping commits you did
   not create in this work unit. Pre-commit hook handling: if the hook
   REJECTED your `git commit` (no new commit exists — check `git log -1`
   authorship/title first), stage the hook's reformatting, re-run the gate,
   and run a fresh `git commit` — `--amend` here rewrites someone else's
   HEAD. Only amend when `git log -1` shows the commit you just created. If
   the branch contains commits or working-tree changes you don't recognize,
   leave them untouched — they belong to the orchestrator. If that state
   blocks you, STOP and report.

Parallelization: shaders are independent (Lighting ∥ Water ∥ Effect ∥ Utility ∥
RunGrass); **sections within one shader are sequential** (each move shifts line
numbers). One subagent per shader, sections in order, is the right shape.

Expected outcome stated honestly: maintainability only. Compile time unchanged
(±noise) — say so in the PR rather than implying a perf win. Type: `refactor:`.

## Phase 2 — Byte-identical permutation slimming (parallel, gated)

After decomposition, wrap each section include in the preprocessor condition
under which its code can contribute at all (most sections already sit under
technique/flag `#if`s — this phase tightens the leaks: declarations, helpers,
and feature blocks that are referenced by nothing in a given permutation).

-   Gate per change: **Tier 1 over the full list** (Tier 0 cannot pass here —
    preprocessed text legitimately shrinks; bytecode must not change). DIFFERS on
    any entry ⇒ revert + escalate; the code wasn't dead.
-   Measure: re-run the Phase-0 bench on the shader; report delta vs baseline.
    If the win is <5% — report that honestly and stop slimming that shader; do
    not chase noise.
-   This is the only lower-model phase that can legitimately claim compile-time
    improvement, and only with the bench numbers attached (`perf:` rules in
    CLAUDE.md apply; normalize to what users feel: boot/recompile wall time).

## Phase 3 — Hot-permutation restructuring (escalation only — NOT fleet work)

MTLand×TRUE_PBR lifetime-narrowing, accumulate-and-discard layer blending, any
`Texture2DArray`/loop-ization (issue #149 Levels A/B). Bytecode **will** differ;
verification is Tier-2 asm review + runtime A/B on SE and VR landscape scenes +
senior review. A lower-capability agent reaching a DIFFERS verdict in Phases 1–2
hands the case here; it never self-approves.

## Sequencing & risk notes

-   Phase 0 → Phase 1 strictly; Phase 2 needs both (the bench for measurement,
    the decomposition for clean guard points).
-   Land per shader, smallest first (`RunGrass` or `Utility`) to shake out the
    tooling before `Lighting`.
-   Every PR: `refactor(shaders): …` (Phase 1) or `perf(shaders): …` with bench
    numbers (Phase 2); note in the description that DXBC was verified identical
    and over which permutation list.
-   Cache interaction: byte-identical refactors still bump the source `.hlsl`
    mtime ⇒ users recompile once per landed PR. Batch sections per shader into
    one PR to avoid repeated mass recompiles (see issue #148 for the cache half).
-   VR: the same gate covers VR — the generated lists include the VR validation
    config's entries; no separate VR pass needed in Phases 1–2.
