# Repository Architecture & Core Systems

This document provides a detailed overview of the Open Shaders plugin architecture, DirectX 11 integration, runtime targeting system, and codebase organization.

## Architecture Overview

### Plugin Architecture

Open Shaders uses a feature-driven modular system where each graphics enhancement is an independent `Feature` class that can be enabled/disabled at runtime.

-   **`Feature`** ([src/Feature.h](file:///E:/Documents/source/repos/open-shaders/src/Feature.h)) - Base class for all graphics features.
-   **`State`** ([src/State.h](file:///E:/Documents/source/repos/open-shaders/src/State.h)) - Global singleton managing feature lifecycle.
-   **`ShaderCache`** ([src/ShaderCache.h](file:///E:/Documents/source/repos/open-shaders/src/ShaderCache.h)) - Runtime shader compilation and caching.
-   **`Menu`** ([src/Menu.h](file:///E:/Documents/source/repos/open-shaders/src/Menu.h)) - ImGui-based in-game configuration interface.

### Feature Implementation Pattern

Each feature follows a consistent structure:

1. **C++ Implementation:** `src/Features/FeatureName.cpp/h` inheriting from `Feature`.
2. **Shader Assets:** `features/FeatureName/Shaders/` containing HLSL shaders.
3. **Configuration:** `features/FeatureName/Shaders/Features/FeatureName.ini` with versioned settings.
4. **Core Features:** Features with a `CORE` marker file bundle with the main mod.

### DirectX Integration

-   **Hooking System:** Uses the Detours library to intercept DirectX 11 API calls in [src/Hooks.cpp](file:///E:/Documents/source/repos/open-shaders/src/Hooks.cpp).
-   **Deferred Rendering:** Custom deferred pipeline in [src/Deferred.cpp](file:///E:/Documents/source/repos/open-shaders/src/Deferred.cpp) with feature integration points.
-   **Shader Management:** Runtime compilation with include system (`package/Shaders/Common/`) for shared utilities.
-   **Base Shader Library:** `package/Shaders/` contains Skyrim's core rendering shaders (Lighting.hlsl, Water.hlsl, Sky.hlsl, etc.).

---

## Critical Dependencies & Runtime Targeting

### CommonLibSSE-NG (`extern/CommonLibSSE-NG`)

Essential reverse engineering library providing interfaces to interact with Skyrim's game engine safely.

-   **`RE::` Namespace:** Skyrim game objects and classes (`BSShader`, `TESObjectREFR`, etc.).
-   **`REL::` Namespace:** Relative addressing and version management.
-   **`SKSE::` Namespace:** SKSE plugin interfaces and utilities.

### Runtime Targeting System

CommonLibSSE-NG supports multiple Skyrim versions through sophisticated runtime targeting. See [CommonLibSSE-NG Runtime Targeting Wiki](https://github.com/CharmedBaryon/CommonLibSSE-NG/wiki/Runtime-Targeting) for details.

#### Single-Runtime Pattern (Compile-Time)

When targeting one specific version, conditional compilation checks (like `ENABLE_SKYRIM_VR`) are used:

```cpp
#ifdef ENABLE_SKYRIM_VR
    virtual void Unk_09(UI_MENU_Unk09 a_unk);  // VR-only virtual function
#endif
```

#### Multi-Runtime Patterns (Runtime Detection)

When targeting `ALL` versions in a single binary, use runtime accessors and relocations:

```cpp
// Runtime member access with different offsets per version
auto& GetRuntimeData() {
    return REL::RelocateMemberIfNewer<PLAYER_RUNTIME_DATA>(
        SKSE::RUNTIME_SSE_1_6_629, this, 0x3D8, 0x3E0);
}

// VR-specific runtime data (only exists in VR)
auto& GetVRRuntimeData() {
    return REL::RelocateMember<VR_PLAYER_RUNTIME_DATA>(this, 0, 0x3D8);
}

// Runtime detection
if (REL::Module::IsVR()) {
    // VR-specific code path
}
```

-   **`REL::RelocateMember<T>()`** - Access members with different offsets.
-   **`REL::RelocateVirtual<T>()`** - Call virtual functions with variant vtables.
-   **`REL::Module::IsVR()`, `IsAE()`, `IsSE()`** - Runtime version detection.
-   **`REL::RelocationID()`** - Dynamic address resolution based on version.

---

## Core Subsystems

### Global System ([src/Globals.h](file:///E:/Documents/source/repos/open-shaders/src/Globals.h))

Central coordination point providing access to all major subsystems:

-   **Core Systems:** `globals::state`, `globals::deferred`, `globals::menu`, `globals::shaderCache`
-   **Graphics Integration:** `globals::d3d::*` (DirectX device/context), `globals::game::*` (renderer state, shaders), `globals::upscaling` (FidelityFX & Streamline), `globals::dx12SwapChain`.
-   **Feature Registry (`globals::features::`):**
    -   _Lighting:_ `lightLimitFix`, `volumetricLighting`, `skylighting`, `ibl`
    -   _Terrain:_ `terrainShadows`, `terrainBlending`, `terrainVariation`, `terrainHelper`
    -   _Materials:_ `extendedMaterials`, `hairSpecular`, `subsurfaceScattering`
    -   _Effects:_ `screenSpaceGI`, `screenSpaceShadows`, `waterEffects`, `wetnessEffects`
    -   _Environment:_ `cloudShadows`, `dynamicCubemaps`, `weatherEditor`, `skySync`
    -   _VR:_ `vr` (VR-specific adaptations & transformations)

### Shared Utilities (`src/Utils/`)

-   `UI.h/cpp` - ImGui utilities and input mapping.
-   `D3D.h/cpp` - DirectX utilities and helper functions (including `Util::SetResourceName`).
-   `Game.h/cpp` - Skyrim-specific game state and object utilities.
-   `VRUtils.h/cpp` - VR-specific utilities and coordinate transformations.
-   `FileSystem.h/cpp` - File I/O and path helpers.
-   `Format.h/cpp` - String formatting and conversion.
-   `Serialize.h/cpp` - JSON serialization helpers.

---

## Feature Development Workflow

When implementing new features, follow these step-by-step guidelines:

1. **Use Template:** Start by copying the template folder in `template/` as a base.
2. **Implement Feature Interface:** Implement the C++ class inheriting from `Feature` ([src/Feature.h](file:///E:/Documents/source/repos/open-shaders/src/Feature.h)) with:
    - `DrawSettings()` - ImGui configuration UI with performance impact warnings.
    - `LoadSettings()` - JSON settings deserialization.
    - `SaveSettings()` - JSON settings serialization.
    - Feature-specific rendering hooks (interception and processing).
3. **Add Shader Assets:** Put shaders in `features/NewFeature/Shaders/`. Optimize heavy operations with compute shaders.
4. **Create Config:** Add a versioned `.ini` configuration in `features/NewFeature/Shaders/Features/NewFeature.ini`. Identify and document performance-related settings.
5. **Register Feature:** Register your class in `globals::features` and the relevant source initialization files.
6. **Performance Verification:** Measure GPU impact and provide settings/toggles to disable the feature or lower its cost.
