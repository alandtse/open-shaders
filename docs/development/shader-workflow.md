# Shader Development Workflow

## Quick Reference

```bash
# Fast shader-only deployment (recommended for dev iteration)
cmake --build build/ALL --target COPY_SHADERS

# Full deployment (DLL + tests + shaders)
cmake --build build/ALL --target DEPLOY_ALL

# Prove an HLSL refactor changed no behavior (compares compiled DXBC vs a git ref)
pwsh tools/verify-shader-refactor.ps1 package/Shaders/Foo.hlsl   # or tools/verify-shader-refactor.sh
```

## Verifying refactors

`tools/verify-shader-refactor.ps1` (bash wrapper: `tools/verify-shader-refactor.sh`)
compiles a shader from a base git ref and from the working tree across the
`VR` × `HDR_OUTPUT` permutations, then compares the compiled bytecode. The base
ref's whole include tree is materialized (via `git archive`), so the base compiles
against base-ref `.hlsli` headers and the working tree against working headers — a
refactor that also edits a shared header is compared correctly, not masked:

-   **IDENTICAL** SHA-256 of the `.cso` ⇒ the refactor is a provable no-op (fxc emits
    no timestamps without `/Zi`, so identical source ⇒ identical bytes).
-   **DIFFERS** ⇒ it dumps and diffs the `/Fc` assembly so a legitimate-but-non-identical
    change can be reviewed.

Exit codes: `0` all identical, `2` some differ, `1` compile error. Defaults to comparing
the working tree against `merge-base(HEAD, origin/dev)`; pass `-BaseRef <ref>` to override.
Requires `fxc.exe` from the Windows SDK. The permutation sweep is strong evidence, not the
full `shader-validation.yaml` matrix — pass `-Permutations` for exotic define combos.

## Fast iteration, without paying a full shader recompile

Three separate mechanisms interact here; picking the wrong one is what makes
iteration feel slow.

### Pick the narrowest build for what you changed

| What changed               | Command                                                                     | Cost                                                       |
| -------------------------- | --------------------------------------------------------------------------- | ---------------------------------------------------------- |
| C++ only, no in-game test  | `cmake --build --preset Dev-Fast`                                           | fastest; never deploys                                     |
| C++, needs an in-game test | build the `CommunityShaders` target under a `*-WITH-AUTO-DEPLOYMENT` preset | DLL+PDB only, via POST_BUILD; does not touch shaders/tests |
| Shader only                | `cmake --build <dir> --target COPY_SHADERS`                                 | content-diffed, no DLL/tests                               |
| Everything (pre-push)      | `cmake --build <dir> --target DEPLOY_ALL`                                   | DLL + shaders + tests                                      |

There is no separate `DEPLOY_DLL` target: building just the `CommunityShaders`
target under an auto-deploy preset already deploys only the DLL/PDB via its own
`POST_BUILD` step, without pulling in `PREPARE_AIO`/`COPY_SHADERS`.

### Why a branch switch used to recompile every shader in-game

The runtime disk cache (`src/ShaderCache.cpp`) checks a manifest-recorded XXH3
content digest of a shader's source (and its transitive `#include` closure)
against the compiled cache blob, falling back to an mtime comparison only when
no digest is on record yet (a cache built before the manifest existed). This is
already correct in principle, since an untouched file's content never changes
its digest and never re-triggers, regardless of mtime. Historically (before the
digest existed) the check was mtime-only, and the problem was one hop upstream: deploying the
staged AIO shader tree into the game's `Data/Shaders` folder used `robocopy`'s
timestamp/size comparison, which can't tell "same content, different mtime"
apart from "actually changed." `CMakeLists.txt` compensated for this
(`#2251`) by wiping the entire staged AIO tree (Shaders included) whenever
`git rev-parse --short HEAD` changed, forcing every file to a fresh mtime so a
legitimately-changed file always won that comparison against a stale
manually-installed one. The side effect: switching branches (or committing)
gave every unchanged shader a fresh mtime too, so the game recompiled
everything on next launch regardless of whether that shader actually differed
between the two branches.

**Fixed**: the AIO-staging -> game-folder hop for `Shaders/` now goes through
`cmake/SyncShaderDeploy.cmake`, a real per-file content comparison (the same
`cmake -E copy_if_different` mechanism already used for the repo-source ->
AIO-staging hop), instead of `robocopy`. An unchanged shader now keeps its
existing mtime end to end, so the runtime cache and the in-game FileWatcher
(below) correctly see nothing to do. The git-HEAD wipe still runs for
non-shader AIO content (textures, configs, `package/`), since that hop is
still `robocopy`-based and still needs it; Shaders/ is excluded from the wipe.

Windows has no native `rsync`-equivalent bulk content-diff tool: `robocopy`
only compares name/size/timestamp, never content. CMake's own
`copy_if_different` (byte comparison per file) is the practical native
substitute, and this repo already used it for one hop; `SyncShaderDeploy.cmake`
just applies the same mechanism to the other one, at the cost of reading file
content instead of just stat-ing it (cheap for a shader tree's file sizes).

### The two in-game toggles

Independent of the build-side fix above, two menu settings control the
runtime cache's own staleness check:

-   **Skip Unchanged Shaders** (`SettingsTabRenderer`, default on): the
    mtime fallback described above, used only when no manifest digest is on
    record yet for a given blob.
-   **File Watcher** (`AdvancedSettingsRenderer`, default off): watches
    `Data\Shaders` for real filesystem write events (`efsw`) instead of
    re-stat-ing on demand. It tracks a shader's real mtime _at the moment a
    write event fires_, so it only reacts to files that were actually
    (re)written, so a content-identical deploy that the sync script correctly
    skips produces no write event and no false recompile, while a genuinely changed
    shader produces exactly one event and one targeted recompile. This is why
    the content-based deploy hop above and FileWatcher compose correctly with
    no FileWatcher-side change needed: FileWatcher was already only as good as
    the write events it's fed, and those are now accurate.

### Routine version bumps no longer force a full recompile

`ValidateDiskCache` used to wipe the entire disk cache (`DeleteDiskCache()`) on
_any_ `PluginVersion` mismatch in `Data/ShaderCache/Info.ini`, i.e. every single
release, whether or not that release actually touched a shader. Since the
manifest digest is now authoritative for individual blob staleness, a
`PluginVersion` bump with every feature's enabled/version state unchanged keeps
the existing cache instead (logged as `Plugin version changed with no
feature-state changes; keeping disk cache`); each shader still gets
individually re-validated against its content digest on the next request, so a
release that genuinely changed a shader still recompiles exactly that shader,
not the whole cache. `EnabledFlip`/`FeatureVersion` mismatches keep their prior
partial-invalidation behavior unchanged.

One consequence: a shader file removed or renamed by an upstream sync no longer
gets swept away by an incidental full wipe on the next version bump. A
`PruneOrphanedShaderCacheEntries()` pass runs whenever `ValidateDiskCache`
decides to keep the cache across a version mismatch, removing manifest entries
(and their blob files) whose source `.hlsl` no longer exists under
`Data/Shaders`.

### Branch-swap A/B testing without paying any recompile tax

Even with the fix above, a branch switch that _does_ change a shader still
correctly triggers exactly one recompile for that shader, which is expected, not a
bug. To A/B two branches with zero recompile risk at all:

-   **Prefer a runtime toggle over a branch switch** when comparing a
    feature flag, not a code change: flip it via `openshaders.feature set` /
    devbench with the game closed, or in-menu. Zero files touched.
-   **Use a separate build directory (or worktree) per branch** when you
    genuinely need two different branches built: each directory's own
    `git-head.stamp` never changes as long as you don't switch branches
    _inside_ it, so the non-shader wipe never triggers between A/B runs.
    Building from a `.claude/worktrees/`-style path hits Windows `MAX_PATH` on
    FidelityFX's generated permutation headers; `subst` a drive letter to the
    worktree root first.

## Overview

Two deployment targets for different workflows:

-   **`COPY_SHADERS`** - Fast shader-only deployment (seconds)
-   **`DEPLOY_ALL`** - Full build + tests + deployment (minutes)

### Requirements

-   Must have `AUTO_PLUGIN_DEPLOYMENT=ON` in your CMake preset
-   Must have `CommunityShadersOutputDir` environment variable set to your Skyrim directory

### Usage

#### Manual

```bash
# Fast iteration: Only copy changed shaders to game directory
cmake --build build/ALL --target COPY_SHADERS

# Or in Visual Studio: Right-click "COPY_SHADERS" target -> Build

# Full deployment (same as running cmake --build with no target):
cmake --build build/ALL --target DEPLOY_ALL
```

#### Automatic (VSCode)

You can configure VSCode to automatically deploy shaders when you save `.hlsl` or `.hlsli` files using the [RunOnSave](https://marketplace.visualstudio.com/items?itemName=emeraldwalk.RunOnSave) extension.

**See [VSCode Setup](../development/vscode-setup.md) for complete configuration instructions.**

### Prerequisites

1. Run `cmake --preset ALL-WITH-AUTO-DEPLOYMENT` at least once to create build directory
2. Set `CommunityShadersOutputDir` environment variable to your Skyrim `Data` directory
3. Ensure `AUTO_PLUGIN_DEPLOYMENT=ON` in your CMake preset

### What COPY_SHADERS does now

1. ✅ Transforms shaders from source layout → game layout (via AIO staging)
2. ✅ Copies only changed shader files (incremental robocopy)
3. ✅ Deploys to `$CommunityShadersOutputDir/Shaders`
4. ❌ Does NOT build the DLL
5. ❌ Does NOT run shader tests
6. ❌ Does NOT deploy non-shader files

### Target Comparison

| Target            | Builds DLL | Runs Tests | Copies Shaders | Use Case              |
| ----------------- | ---------- | ---------- | -------------- | --------------------- |
| `COPY_SHADERS`    | ❌         | ❌         | ✅             | Fast shader iteration |
| `DEPLOY_ALL`      | ✅         | ✅         | ✅             | Full deployment       |
| `prepare_shaders` | ❌         | ✅         | ✅ (AIO only)  | CI validation         |

## Incremental shader validation

Validating the full shader suite recompiles ~3,300 variants per config, which is
slow for a one-line change. Incremental validation compiles **only the
entry-point shaders that transitively `#include` a changed file**, derived from
an `#include` dependency graph. A leaf shader used by one entry point validates
in seconds; a shared `Common/*.hlsli` fans out to every shader that includes it.

### Local

```bash
# Compile only shaders affected by your working-tree changes (vs HEAD)
cmake --build ./build/ALL --target validate_changed

# Override the config (defaults to Flatrim / shader-validation.yaml)
cmake -DVALIDATE_CHANGED_CONFIG=.github/configs/shader-validation-vr.yaml -S . -B build/ALL
cmake --build ./build/ALL --target validate_changed
```

The target depends on `prepare_shaders`, so the AIO shader tree is assembled
first. Under the hood it runs `tools/validate_changed_shaders.py`, which maps
your changed `package/Shaders/**` and `features/**/Shaders/**` paths into the
AIO layout and passes them to `hlslkit-compile --changed-files`.

### CI

The PR shader-validation job feeds the PR's changed-file list (from
`tj-actions/changed-files`) into the same wrapper. Validation is **only**
narrowed when provably safe — any of these forces a full run:

-   a change to a validation config (`.github/configs/**`), CMake, or a submodule
    (these can redefine the entry-point/define set itself);
-   a changed shader path outside the known shader roots;
-   push/release builds (no PR change set), which always validate in full.

`hlslkit` applies the same safety net independently: any changed path it can't
find in the shader tree falls back to full validation. Preprocessor guards are
ignored when scanning `#include`s, so the affected set is always a conservative
superset — over-validating costs time, never correctness.

## Manual Shader Validation Commands

To validate shaders locally using `hlslkit` (external dependency):

```bash
# Install hlslkit
pip install git+https://github.com/alandtse/hlslkit.git

# Prepare shaders for validation (builds shader directory structure)
cmake --build ./build/ALL --target prepare_shaders

# Full shader suite validation
hlslkit-compile --shader-dir build/ALL/aio/Shaders --output-dir build/ShaderCache --config .github/configs/shader-validation.yaml --max-warnings 0 --suppress-warnings X1519

# VR-specific validation
hlslkit-compile --shader-dir build/ALL/aio/Shaders --output-dir build/ShaderCache --config .github/configs/shader-validation-vr.yaml --max-warnings 0 --suppress-warnings X1519

# Targeted testing for faster development
# Test specific base shader
hlslkit-compile --shader-dir build/ALL/aio/Shaders/Lighting.hlsl --output-dir build/ShaderCache --config .github/configs/shader-validation.yaml

# Test specific compute shader
hlslkit-compile --shader-dir build/ALL/aio/Shaders/DeferredCompositeCS.hlsl --output-dir build/ShaderCache --config .github/configs/shader-validation.yaml

# Test specific feature directory
hlslkit-compile --shader-dir build/ALL/aio/Shaders/ScreenSpaceGI/ --output-dir build/ShaderCache --config .github/configs/shader-validation.yaml

# Test feature-specific compute shader
hlslkit-compile --shader-dir build/ALL/aio/Shaders/LightLimitFix/ClusterBuildingCS.hlsl --output-dir build/ShaderCache --config .github/configs/shader-validation.yaml

# Generate shader defines from game log (requires CommunityShaders.log from game)
hlslkit-generate-defines --log CommunityShaders.log

# Scan for buffer conflicts across features
hlslkit-buffer-scan --features-dir features/
```

### Regenerating shader-validation.yaml / shader-validation-vr.yaml

Clear the disk cache, set Log Level to Debug/Trace, launch, and wait for the
boot-time compile queue to finish -- **main menu only, no save needed.** A
2026-08-19 bounded debate confirmed this: the offline `hlslkit-compile` pass
validates every entry-point/define combination declared by the selected
config during a full validation run; ordinary PRs use incremental validation
for only the affected entry points and their permutations (see
`tools/validate_changed_shaders.py`). Either way, structural correctness is
re-checked deterministically regardless of how the config was populated --
so a live capture's only job is seeding that structure and a warnings
baseline, not exhaustive runtime enumeration. Visiting real gameplay to widen
coverage doesn't actually close the gap below (see caveat).

A cold-compile burst (clearing the disk cache) used to be a reliable
reproducer for a UAF in `PostProcessFeature::CompileComputeShadersAsync()`'s
completion callback (fixed 2026-08-19, see PR #500 -- the callback captured
`this` by raw pointer with only a generation counter guarding it, not
lifetime). Verified fixed via a genuinely cold full VR capture
(3343 variants) completing cleanly post-fix where the same scenario had
crashed twice before.

**Known gap:** `hlslkit-generate` only recognizes the `ShaderCache`
`ShaderClass:Type:descriptor` log format. Shaders compiled ad hoc outside that
system (e.g. Effects11's `CopyPS`/`ColorCorrectionCS`/`RaymarchVolumetricRaysPS`,
each compiled via a raw `D3DCompile`/`Util::CompileShader` call in
`EffectManager.cpp`/`Effects11.cpp`) never appear in a generated config no
matter how the capture is driven -- this is a parser limitation in `hlslkit`
itself, not something a longer or more thorough play session fixes. Closing it
needs either a `hlslkit` change to recognize these log lines, or a manually
maintained config entry for each such shader.

**Second known gap, confirmed 2026-08-19:** even within the `ShaderClass`
system, a handful of shaders only compile when live scene/weather state
happens to need them, not just when their feature is loaded -- e.g.
`ISVolumetricLightingBlurHCS`/`BlurVCS`/`GenerateCS`/`RaymarchCS` (require a
real exterior/lit-interior scene with Dynamic Resolution active) and VR's
`ISFullScreenVR` (a native engine full-screen image-space effect, not yet
root-caused which runtime state gates it) never fired during a static
main-menu boot on either platform and silently drop out of a from-scratch
regen. **Diff the new config's `file:` list against the previous version's
before committing a regen**:

```bash
for config in .github/configs/shader-validation.yaml \
              .github/configs/shader-validation-vr.yaml; do
  echo "== $config =="
  diff -u \
    <(git show origin/dev:"$config" | grep -oP '(?<=- file: )\S+' | sort -u) \
    <(grep -oP '(?<=- file: )\S+' "$config" | sort -u) || true
done
```

to catch these -- but do NOT manually splice the missing entries back into
the generated YAML (anchor/reference IDs are regen-specific and hand-editing
them has caused real breakage before). Treat a diff match against this known
list as an accepted, documented gap and commit the clean regen as-is; only
chase it with a real gameplay capture if a _new_, unexplained file drops out.

## Custom CMake Targets

In addition to `COPY_SHADERS` and `DEPLOY_ALL`, the project provides several other specialized build and utility targets:

```bash
# Prepare AIO package structure (automatic with AIO_ZIP_TO_DIST or AUTO_PLUGIN_DEPLOYMENT)
cmake --build ./build/ALL --target PREPARE_AIO

# Prepare shaders only (useful for CI shader validation)
cmake --build ./build/ALL --target prepare_shaders

# Create AIO zip package (when AIO_ZIP_TO_DIST=ON)
cmake --build ./build/ALL --target AIO_ZIP_PACKAGE

# Format all C++ and HLSL code (requires clang-format)
cmake --build ./build/ALL --target FORMAT_CODE

# Generate shader validation configs from game logs (requires PowerShell)
cmake --build ./build/ALL --target generate_shader_configs
```
