# In-game release validation (via devbench)

A runbook for validating a release build **in-game** before promoting/publishing,
using the [devbench](https://www.nexusmods.com/skyrimspecialedition/mods/181326)
MCP test bench. Run it per edition (SE and VR) on any release candidate.

The single most important automated check is the **shader-cache gate**: a build
that compiles all shaders in-game with zero failures and loads every feature is
the baseline bar for a release.

## Prerequisites

-   **devbench** installed in the game, and the OS build's **Remote Control** feature
    (the devbench bridge) present — it registers the `openshaders.*` tools.
-   Build environment: the worktree `subst`'d to a short drive (e.g. `X:`) to dodge
    `MAX_PATH` (FidelityFX permutation headers blow past 260 chars from deep worktree
    paths).

## 1. Build + deploy

From the short drive, build the auto-deploy preset (copies DLL+PDB + shaders to every
`CommunityShadersOutputDir`, i.e. the SE and VR `Data` folders):

```bat
subst X: <worktree>            :: if not already mapped
X:
BuildRelease.bat ALL-WITH-AUTO-DEPLOYMENT
```

A clean build shows only benign FidelityFX `MSB8028` "intermediate directory shared"
warnings.

## 2. Launch + compile shaders

Launch the edition under test:

-   **SE**: `skse64_loader.exe` (from the SE install dir).
-   **VR**: `sksevr_loader.exe` / Steam.

The first launch after a shader deploy does a **full recompile** (~6–10 min on SE; longer
on VR with a full mod list). devbench's server comes up early, but `inspect` marshals to
the main thread, so it returns **HTTP 504 while shaders are still compiling** — that is
expected. Poll until a clean `inspect` response, e.g.:

```bash
# devbench port is in Data/SKSE/Plugins/devbench/runtime.json (default 8920;
# auto-iterates if another instance already holds the port — e.g. SE + VR at once).
until curl -s -m10 http://127.0.0.1:8920/api/tool/inspect -XPOST \
  -H 'Content-Type: application/json' -d '{"kind":"state"}' | grep -q '"plugin"'; do sleep 30; done
```

## 3. devbench MCP validation

All calls are `POST http://127.0.0.1:<port>/api/tool/<name>`.

> **Two `inspect` tools — not interchangeable.** `inspect` is devbench's own state
> tool and reports `playerLoaded`/`vr` — use it for the compile/load polling above.
> `openshaders.inspect` is the OS bridge's tool for CS `state`/`shadercache` (the
> `failedTasks`/`currentFailedCount` fields) — use it for the shader gate.
> `openshaders.inspect {"kind":"state"}` does **not** report `playerLoaded`, so don't
> use it for load polling.

| Step            | Call                                                             | Pass criteria                                                              |
| --------------- | ---------------------------------------------------------------- | -------------------------------------------------------------------------- |
| Alive           | `inspect {"kind":"state"}`                                       | returns `plugin`, correct `vr` flag, no error                              |
| **Shader gate** | `openshaders.inspect {"kind":"shadercache"}`                     | **`failedTasks == 0` and `currentFailedCount == 0`**, `compiling == false` |
| Features        | `openshaders.feature {"action":"list"}`                          | every entry `loaded == true`                                               |
| Menu (QA)       | `openshaders.menu {"op":"open","section":"Advanced"}`            | menu opens and navigates to requested section                              |
| In-game         | `game {"action":"loadLast"}` → poll `inspect` for `playerLoaded` | reaches `playerLoaded`, no CTD                                             |
| Runtime shaders | `openshaders.inspect {"kind":"shadercache"}` (after load)        | `failedTasks == 0` for the runtime/on-demand batch                         |
| Evidence        | `openshaders.capture {"kind":"screenshot"}`                      | writes `Screenshots/CS_*.bmp`                                              |

If you are driving the base devbench `menu` tool directly, the Community Shaders menu
toggle call is `menu {"action":"invoke","name":"CommunityShaders"}`.

Then scan the log for genuine errors (see §4 for the path). Distinguish release-relevant
failures from **known-benign** lines that appear every run:

-   `[StreamlineSDK] … presentCommon() was not observed` / `slSetTag … deprecated` /
    `Thread id over 65536` — DLSS/Streamline (Upscaling) internal notices.
-   `… has more than one height maps!` — terrain shadows, informational.
-   `[WeatherEditor] Failed to inspect widget settings path '…'` — optional override dirs
    that simply don't exist.

A **critical** error is a shader compile failure (`failedTasks > 0`), a feature failing to
load, a CTD, or a new `[E]`/`[W]` traceable to the change under test.

## 4. Refresh the shader-validation CI data (fast-follow)

The CI shader-validation matrix (`.github/configs/shader-validation{,-vr}.yaml`) is
generated from the game log, so refresh it whenever shader permutations change.

-   **Requires Debug logging** (CS menu → Advanced → Log Level). The log uses single-letter
    levels — the define dumps are `[D] Compiling <shader> with <DEFINES> …` lines (thousands
    of them on a full compile).
-   Log path: `<MyDocuments>/My Games/<Skyrim edition>/SKSE/CommunityShaders.log` (note
    `MyDocuments` may be redirected, e.g. to `E:\Documents`).

```bash
# Both editions from auto-discovered logs (run each edition first):
cmake --build ./build/ALL --target generate_shader_configs

# Or one edition from a specific log:
pwsh .github/configs/generate-shader-configs.ps1 \
  -LogFile "<…>/CommunityShaders.log" -OutputName shader-validation.yaml      # SE
  # -OutputName shader-validation-vr.yaml for VR
```

Diff the regenerated YAML and open a `ci:` PR. Capture/copy the log first — CS overwrites
it on the next launch.

## 5. Repeat per edition

Run §2–§4 for **both SE and VR** — the same change can behave differently per runtime, and
each edition produces its own shader-validation config.

## Future automation (see also the devbench README)

-   **Shader-cache gate as a one-command check** — wrap §3's `openshaders.inspect`
    shadercache assertion in a devbench `scenario` so "do all shaders compile in-game" is a
    single repeatable call.
-   **Record/replay + screenshot diff** — record a fixed trajectory through water/snow/terrain
    cells, replay deterministically, `openshaders.capture` at set points, and diff screenshots
    for visual regression of effects like water ghosting / snow banding.
-   **`autoRun` smoke test** — ship a devbench scenario that runs unattended on first load and
    reports pass/fail via the EventBus, so any playtest self-validates.
