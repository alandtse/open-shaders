# Agent guidance for `open-RT`

This file is intentionally brief. **Source code and current branch state outrank documentation and old discussions.** Work on the explicitly requested branch, check its head before writing, and do not overwrite concurrent changes.

## Project rules

- Preserve the `CommunityShaders` runtime name, DLL, SKSE paths, identifiers and settings compatibility. Public fork name: Open Shaders. Preserve `COPYING`, third-party licenses, copyright and upstream credits.
- Keep changes scoped and reviewable. Do not alter unrelated shaders, CMake inputs, runtime settings or assets during documentation maintenance.
- DX12 work belongs to its native owner. DX11 capture/engine hooks may supply observations; do not claim their work moved to DX12 without checking execution.
- Native Scene, independent shadow mask, Native Lighting, Native Composite, Native PBR, Native LLF and Native SCM have separate responsibilities and runtime controls. Avoid accidental feature coupling.
- Preserve producer/consumer fence, scene generation and resource-lifetime safety. Allocation decisions use current DXGI telemetry.
- Native shaders may use DXC/HLSL6 while legacy FXC compilation still exists. Avoid claiming a DXC-only pipeline.
- Keep non-VR implementation the primary test path without casually breaking SE/AE/VR compatibility.
- Use targeted tests and compile checks for code changes. Never claim runtime verification unless performed and documented by the tester.
- No force-pushes, unnecessary history rewrites, version bumps or hand-created release tags. Use conventional, concise commit subjects.

## Documentation scope

[README.md](README.md) is the entry point; [docs/development/README.md](docs/development/README.md) is the *current* architecture and validation index. Documentation should say what exists, what it owns, and what is unverified. Keep historical experiments, work logs and future proposals in commits/issues/PRs. Update docs when source reality changes; never duplicate old audit journals.

For the detailed behavior of a function, read its current implementation first.
