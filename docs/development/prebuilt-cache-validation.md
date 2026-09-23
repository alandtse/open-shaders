# Prebuilt shader cache regression test

The release-cache gate checks the default AIO profile for SE and VR. It runs in
the shared release build and the rolling `dev` build, before uploading cache
artifacts. Publishing newly built dev/release artifacts requires this gate to
succeed. To run it against `dev` without publishing a release, dispatch
**CI: Prebuilt Shader Cache** with `ref=dev`. The workflow produces the
`ShaderCache-SE` and `ShaderCache-VR` artifacts; it does not upload to Nexus.

The manual validator grants its shared build `cache-mode: read`: selected refs
can restore CI dependency/build caches but cannot save them. This token-level
restriction is separate from `contents: read` and does not block uploading the
shader-cache test artifacts. Digest checks do not authenticate their producer.

Reused compiler artifacts carry `CompileConfig.yaml` and `CompileInputs.json`,
recorded immediately after compilation. Finalization rejects changes to source
digests, permutations, runtime, or bytecode before writing the release manifest.
These sidecars are removed from the distributable cache.

The offline gate checks every captured permutation, source/include and
permutation digest, feature version/enabled state, manifest entry, blob path,
and DXBC container/stage. It rejects the original 2.13.0 manifest formula and
the stale Sky profile even if that profile has an internally consistent manifest.
It shares the builder's Python interpretation of runtime keys, so a passing
offline check does not prove that the C++ runtime still agrees. Use the live
check below before publishing.

Run the focused regression tests on Windows with hlslkit installed:

```powershell
py -3.10 -m unittest discover -s tests -p 'test_*shader_cache.py' -v
```

To check an extracted cache against its matching checkout and packaged shaders:

```powershell
py -3.10 tools/verify_shader_cache.py artifact --runtime SE `
  --cache build/ShaderCache --shaders build/ALL/aio/Shaders
```

Use `--source-root` for a different checkout. Do not validate a release cache
against unrelated `dev` sources. The gate is read-only and never regenerates a
manifest to make an old cache appear valid.

## Fresh-session reuse check

Install the matching plugin, shaders, and fresh cache in a clean test profile.
Keep Developer Mode off and custom shader defines empty. Use the same feature
profile as the cache. For MO2, use the mod directory containing the deployed
files and ensure it wins all shader/cache conflicts; disable other cache mods
and remove conflicting cache files from Overwrite in that test profile.

Before launching the game, snapshot the installed files. `--reference-cache`
must point to the freshly built/extracted artifact, so an already warmed game
cache cannot silently become the baseline:

```powershell
$testMod = "$env:LOCALAPPDATA\ModOrganizer\Skyrim Special Edition\mods\Open Shaders"
$testLog = "$env:USERPROFILE\Documents\My Games\Skyrim Special Edition\SKSE\CommunityShaders.log"
py -3.10 tools/verify_shader_cache.py snapshot --runtime SE --data "$testMod" `
  --reference-cache build/ShaderCache --log "$testLog" --out build/cache-before-SE.json
```

Launch that runtime, load the test save, and leave the game running. Once the
shader queue has drained, run:

```powershell
py -3.10 tools/verify_shader_cache.py session --snapshot build/cache-before-SE.json `
  --endpoint http://127.0.0.1:8920
```

Repeat with VR's installation, log, cache artifact, and live endpoint (normally
8921). The script uses devbench 1.5+ inspect extensions and only reads state; it
does not launch the game, clear caches, toggle features, or save settings.

A pass requires a fresh matching log, an advancing live runtime with a loaded
player, a nonempty completed queue with **all tasks served from disk**, zero
digest misses/failures, and no feature mismatches. The plugin, shader sources,
all manifest entries, blob hashes, and blob modification times must remain
unchanged. A rewrite to identical bytecode still fails. Compile/invalidation
messages fail even if the compile completed with zero errors. The unrelated
Effects11 missing `enbeffect.fx` message does not fail this cache test.
Live counters and the log are checked again after inspecting files; if the
workload changes during inspection, wait for it to finish and rerun the check.

## Features disabled at boot

Default-disabled Alpha/Beta features are discovered from feature INIs, and
the suite includes a synthetic future feature to test automatic discovery.
Companion requirements use the cache builder's declared default profile.
Live profile mismatches are checked for every feature, without a feature-name
allowlist. A future companion requirement absent from the build metadata must
fail the live check until that metadata is updated.

Horizon Fix adds `HORIZON_FIX` to Water permutations when its companion DLL is
present. Enabling it changes the profile and appropriately fails the default
cache reuse test. Terrain Helper's internal `enabled` flag instead gates texture
setup after looking up `LandscapeDefault`; it does not override `HasShaderDefine`
or add `TERRAIN_HELPER` to cache keys. The suite checks that declaring a macro
name alone does not inject that macro into compiled permutations.

Zero recompilation applies to the tested profile and exercised workload. New
features, custom defines, changed sources, and runtime permutations missing from
the captured configuration can legitimately need compilation. Regenerate
captured configurations only through the live capture workflow, never by hand.
