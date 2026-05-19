# AI Development Instructions

This file provides guidance for AI assistants working with the Open Shaders codebase — a fork of [Community Shaders](https://github.com/community-shaders/skyrim-community-shaders) ([Nexus](https://www.nexusmods.com/skyrimspecialedition/mods/180419)). The runtime layout (DLL name, settings path, log file) intentionally matches upstream Community Shaders so users can switch without losing settings; only the public display name and in-game branding are "Open Shaders".

## Primary Documentation

**For comprehensive development guidance, see `.claude/CLAUDE.md`** which provides detailed information on:

-   Build commands and development setup
-   Architecture overview and critical dependencies (CommonLibSSE-NG)
-   Runtime targeting system for SE/AE/VR compatibility
-   Core architecture including Globals system and feature registry
-   Shader architecture (base shaders in `package/Shaders/`, feature shaders, compute shader patterns)
-   Development workflows and best practices
-   Common pitfalls and testing requirements

## Quick Reference

### Project Type

SKSE plugin providing advanced DirectX 11 graphics modifications for Skyrim SE/AE/VR.

### Essential Commands

-   **Build**: `./BuildRelease.bat [PRESET]` (WSL: use `powershell.exe -Command`)
-   **Shader Test**: `hlslkit-compile --shader-dir [target]` (install via pip first)
-   **Feature Access**: `globals::features::*` namespace

### Build Options

**Runtime Presets**: `ALL` (universal), `SE`, `AE`, `VR`, `PRE-AE`, `FLATRIM`, `ALL-TRACY`

**CMake Options** (set in user preset):

-   `AUTO_PLUGIN_DEPLOYMENT=ON` - Auto-copy to `CommunityShadersOutputDir`
-   `ZIP_TO_DIST=ON` (default) - Create individual feature 7z packages
-   `AIO_ZIP_TO_DIST=ON` (default) - Create all-in-one 7z package
-   `TRACY_SUPPORT=ON` - Enable Tracy profiler integration

### Custom CMake Targets

**Quick targets** (common):

-   `PREPARE_AIO`, `prepare_shaders`, `COPY_SHADERS`, `AIO_ZIP_PACKAGE`
-   `FORMAT_CODE`, `generate_shader_configs`

For full details about manual packaging targets (Package-Core, Package-AIO-Manual, Package-<Feature>, AIO) and example workflows, see the "Manual packaging targets (detailed)" section in `.claude/CLAUDE.md` to avoid duplication.

### AI Assistant Role

**Act as an experienced graphics programming and Skyrim modding expert.**

**Key Focus**: Performance impact awareness, runtime compatibility (SE/AE/VR), complete working solutions, DirectX/HLSL best practices.

**Style directives** (see `.claude/CLAUDE.md` "Code Quality Expectations" for full text):

-   **Concise comments**: explain _why_, not _what_. Don't paraphrase the next 4 lines in 4 lines of comment.
-   **Minimal churn**: PRs touch only what the change requires. Out-of-scope cleanups go in a follow-up, not the current diff.
-   **Present-tense comments**: describe what the code _is_, not what it used to be. No "previously", "the old version", or references to past incidents/PRs/SHAs in code. Exception: a load-bearing regression warning ("do not revert; absence caused X"). History belongs in commit messages, not comments.

For detailed explanations, examples, and comprehensive guidance, refer to `.claude/CLAUDE.md`.
