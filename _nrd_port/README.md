# Open Shaders NRD / SSGI / SSR replacement — source integration candidate

This is an overwriting source patch against the supplied `open-shaders.zip` snapshot, using the actual Bottle-Compendium code in the supplied donor archives. It includes 22 new files, 33 modified files, and 7 obsolete files to remove. It is NOT a compiled Skyrim mod, and is NOT runtime-validated. No Windows compiler, FXC or headset is available in the editing environment. Treat the port as experimental until it passes Windows C++ and HLSL compilation and runtime checks. Back up your checkout and settings first.

## Apply

Option A (recommended for a matching clean Git checkout): use the companion `open-shaders-nrd-backend-v2.patch` from the repository root:

```powershell
git apply --check C:\path\to\open-shaders-nrd-backend-v2.patch
git apply C:\path\to\open-shaders-nrd-backend-v2.patch
```

The Git patch applies additions, modifications and deletions. Do NOT apply it on top of the overwrite ZIP.

Option B: extract the ZIP directly into the repository root, preserving paths, overwrite existing files, then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\_nrd_port\remove_obsolete.ps1
```

The cleanup script checks exact SHA-256 hashes before removing obsolete original files, and refuses to remove modified files. It is safe to rerun. The manifest records baseline and output hashes.

## NRD dependency

The archive does not contain NRD, ShaderMake, or any other external dependency. Initialize the exact donor NRD pin from a clean Git checkout. Run from the root:

```powershell
powershell -ExecutionPolicy Bypass -File .\_nrd_port\init_nrd.ps1
```

This clones NRD if absent, checks out `66e616c29e28fdb777e5304b9dc35ccb184fa473`, initializes any nested submodules and verifies HEAD. The added `.gitmodules` entry is not a Git index gitlink; if committing this port, stage `extern/NRD` and `.gitmodules` to record the dependency as a gitlink. Existing upstream submodules must also be initialized as usual. Do not use an arbitrary newer NRD revision: embedded DXBC and the packed guide configuration must match. Preserve donor/NRD licensing and notices when redistributing.

## Build

Use your normal Open Shaders MSVC/Windows SDK/vcpkg setup. **An existing build directory is supported and recommended to conserve disk space**, provided it belongs to this source checkout and uses the same generator, architecture, and toolchain. NRD is static, DXBC enabled, DXIL/SPIR-V/NRI disabled. FXC and ShaderMake must be available. After initializing NRD, explicitly reconfigure using your existing cache and build incrementally. Example if your existing build directory is `build`:

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel 4
```

Use the actual existing directory name. Keep your existing preset/toolchain and environment (e.g. VS developer shell); do not switch generators or architectures in an existing build directory. If using an existing Ninja single-config Release build, `--config Release` is unnecessary; preserve the cached `CMAKE_BUILD_TYPE`. If your cache has `AIO_ZIP_TO_DIST=OFF` and you want an AIO archive, add `-DAIO_ZIP_TO_DIST=ON` to the configure command. Do not run a clean build or delete CMakeCache.txt merely to add NRD. New targets and changed source/header files will still compile, and changed PCH/include dependencies can trigger substantial recompilation; existing installed dependencies and reusable objects are retained. NRD and ShaderMake require additional space. If configuration fails because the cache belongs to another source directory or incompatible generator/toolchain, resolve that specific issue before considering a fresh build.

Rebuild AIO or the required feature packages, including the NRD core shader files. Do not combine old and new backend feature packages. Clear the Community Shaders shader cache with the existing cache mechanism before launching. Preserve your old mod and config for rollback.

## v2 menu follow-up

- Native SSGI Disabled / Custom re-enabling and changed presets persist user settings and queue a diffuse history reset, including both eyes and multibounce. Also reset specular history because SSR samples the diffuse-composited scene. No shader recompile requested for a pure enable/disable or step-count change. Custom remains a derived quality label, not an independently persisted preset mode.
- Native SSR and full-menu NRD/SSGI/SSR enable changes queue history resets. Effects11-owned native rows reject user edits. Native default display accounts for VR defaults.
- NRD, SSGI and SSR buffer viewers expose eye-specific trees in VR and keep the original flat layout in SE/AE. Right-eye buffers are shown only once allocated; no new GPU buffer allocation is made for debugging. Existing rendering synchronization assumptions remain; VR UI behavior not tested.
- All nine non-English locale files are updated for the new/changed keys. Reuse matching donor translations and exact matching target translations; provide newly authored translations for many controls and eye labels. Missing translations intentionally use the existing English fallback, rather than display old or mismatched text. See `locale_coverage.json` for precise counts. This is not a complete native-speaker-reviewed localization. Unrelated translation values are preserved.
- This full v2 archive supersedes v1 and can be extracted over v1 or the original matching source snapshot. The smaller v2 menu-fixes archive is only for a checkout that already has v1 installed; it contains the changed files and refreshed metadata only.

## Changes

- Donor DX11 NRD service, REBLUR integration and guide encoding; pinned static NRD build integration.
- Donor diffuse visibility-bitmask tracing, full/checkerboard/rotating-quarter sampling, diffuse SH and non-SH REBLUR, multibounce history, and standalone Hi-Z SSR with REBLUR specular. Old SSGI backend removed, not selectable.
- Merge into target deferred orchestration, target feature constant-buffer layout, target settings/feature registration and native menu. Early diffuse composite before SSR; target final composite applies AO and blends SSR/cubemap.
- Keep target global `Color.hlsli` and target color conventions; add only local radiance helper. Adapt donor fallback to existing target IBL/Skylighting. Do not copy donor global ColorManagement or PreExposure.
- Add independent VR per-eye feature/NRD instances and per-eye outputs. Crop packed inputs into eye-local resources, including depth crop shader, and use eye-local camera matrices and motion guide copies. Cross-eye SSGI tracing reuse and stereo synchronization of the new radiance representation are NOT implemented; the old SSGI-specific stereo reuse is disabled/removed. Other VR stereo infrastructure remains. VR correctness, foveation and eye offsets require testing.
- Effects11 setting registration and handoff integration, English translation keys, target GPU profiler scopes. Non-English locale strings are merged/updated in v2; untranslated entries fall back to English/default. No independent algorithm improvements.
- Migrate compatible legacy toggles, map old non-full resolution to quarter sampling, reset incompatible tuning to donor defaults. VR defaults conservative for missing settings (AO only / SSR disabled). The donor SSR requires Dynamic Cubemaps. Multibounce defaults off.
- If denoising initialization fails, attempt donor full-resolution undenoised path. Sparse results are not published if denoiser dispatch fails. Resource and shader failure handling still requires runtime validation.

## Validation status and limits

Performed: source-level merge and inspection, search for old backend consumers, patch whitespace and applicability checks, JSON parse, archive integrity and manifest verification. No Windows C++ compilation, HLSL compiler execution, graphics debug layer, runtime visual tests, performance or VR tests. Source archives did not include external dependency contents, complete CI/test tooling, or Git metadata. Exact upstream revisions cannot be independently authenticated from the archives. No parity guarantee. Check the newly adapted C++ APIs, shader permutations, constant buffers, motion vectors, guide encodings, ambient subtraction, LL/PBR color units, dynamic-resolution borders, optional dependencies, map/load history resets and VR before using on a primary game installation. Run the first test on SE/AE with SSGI only, then SSR, then multibounce and VR. Compare against donor and original with identical game settings. Report build errors and shader compile logs rather than treating this package as a finished release.
