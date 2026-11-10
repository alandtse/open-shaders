# Open Shaders — `open-RT`

Experimental DX12/DXR development branch of [Open Shaders](https://github.com/alandtse/open-shaders), itself derived from [Community Shaders](https://github.com/community-shaders/skyrim-community-shaders) for Skyrim SE/AE/VR. This branch is for active development and in-game testing, not a statement that every renderer path is production-ready.

The runtime compatibility identity remains `CommunityShaders` (including `CommunityShaders.dll`, SKSE paths, and settings). See [COPYING](COPYING) and retained third-party licenses for redistribution terms and attribution.

## Current implementation

- **DX12 Native Scene:** prepared-cell geometry/materials, DXR acceleration structures, runtime geometry acquisition, residency/streaming experiments, and an independent early ray-traced shadow mask.
- **Native Lighting:** independently switchable DX12 direct diffuse/specular ray evaluations with trace-density and validation controls; off by default.
- **Native Composite:** captures the raster color input into a DX12 frame, optionally combines native direct lighting and the independent shadow layer, and manages downstream frame lifetime.
- **Native PBR / Native LLF / Native SCM:** bridge observed material, light and shadow-caster information to the native scene. Skyrim/DX11 remains a source of captured data; the native consumers evaluate their own DX12 work.
- **DX12 Resources:** interior discovery, preparation/companion work, cache management and experimental streaming support.
- **Upscaling:** runtime FSR/DX12 integration exists, but this branch does **not** claim an end-to-end DX12-only presentation path or completed ray-regeneration support.

Native features and their individual controls are intended to remain runtime-selectable. Existence of a path or setting is not proof of successful in-game execution. For ownership boundaries, source files, and validation priorities, see [current development notes](docs/development/README.md).

## Build (Windows)

Prerequisites: Visual Studio 2026 with C++/CMake tools, CMake 4.2+, Git, and vcpkg with `VCPKG_ROOT` configured. Initialize submodules.

```powershell
git clone --branch open-RT --recurse-submodules https://github.com/edgy-the-hedgy/open-shaders.git
cd open-shaders
cmake --preset ALL
cmake --build --preset ALL --config Release
```

Alternatively open this folder in Visual Studio, or use `BuildRelease.bat ALL`. The tracked build presets are in [CMakePresets.json](CMakePresets.json). Local auto-deployment requires your own settings and is not assumed.

## Documentation policy

Document **present code**, the current execution boundary, required setup, and reproducible validation. Do not add historical progress diaries, speculative guarantees, duplicate feature walkthroughs, or stale links. Runtime results must be explicitly identified as verified by a tester. Keep detailed implementation behavior beside the code.

This repository preserves upstream project and contributor attribution; it does not replace or speak for upstream release documentation.
