# DX12 Resources — manual-first preparation and parity contract

## What is committed

DX12 Resources is an independent suite feature with its own Utility-category menu panel. Enter a source plugin (for example, Skyrim.esm) and an exact local hexadecimal interior CELL FormID. It uses TESDataHandler::LookupForm<TESObjectCELL> after DataLoaded, rejects missing or exterior cells, copies source identity to a background worker and writes a SHA-256-verified JSON report under Data/SKSE/Plugins/CommunityShaders/DX12-cache/reports/.

This report deliberately has kind=cell-source-discovery, geometryReady=false, materialsReady=false, lightsReady=false, preprocessorParity=false, renderable=false. A successful form lookup is NOT successful interior preprocessing. No native scene consumes this report and no legacy offline fallback is enabled.

## Parity reference recovered

The original first-stage generator exists in the repository's open-RXRT branch, at tools/redstone-scene-prep:

- Program.cs: Mutagen/Bethesda load-order resolution, winning CELL/REFR contexts, persistent/temporary references, placed light baselines.
- PgAssets.cs: ParallaxGen and deployment-source provenance, CRC32 and SHA256 checks.
- MeshCompiler.cs: Nifly 1.1.0 full-source static mesh processing and shape eligibility.
- NifTransforms.cs and PlacementTransform.cs: NIF block hierarchy, shape-to-model and shape-to-cell transforms.
- GeometryBlob.cs and ShadingBlob.cs: RSGM positions/indices and RSGA shading buffers.
- CellLayout.cs: instance assembly, bounds, rooms, placed light records and omissions.

The original generator itself covered a conservative static baseline, not arbitrary runtime scripts or animated scene state.

The current branch contains a prepared-scene LOADER, not this complete native source-extraction algorithm. Correct parity requires the equivalent winning-record walk and verified effective NIF reads even when the target cell is not instantiated. This cannot be achieved just by calling TESObjectCELL::IsInteriorCell or enumerating a loaded cell's references.

## Implementation gate

PreprocessorParity.h defines explicit readiness stages: winning record resolution, reference sweep, NIF provenance, geometry, transforms, shading attributes, static lights and validated output. The new feature currently reports all incomplete. It does not invoke an external .NET CLI from the main menu. Native source acquisition remains the required next stage.

## Local verification

Build the Windows Release DLL and deploy the DX12Resources.ini. From the Skyrim main-menu Open Shaders panel, open DX12 Resources and use Prepare Selected Cell (source discovery) with a known interior plugin/local hex FormID. Confirm the source-discovery report path and renderable=false. Missing and exterior IDs should be rejected. No CI or Skyrim runtime build/test of these new commits has been performed.


## Selected plugin source fingerprint

The discovery worker additionally opens the selected Data/<plugin> source as an ordinary file with FILE_FLAG_OPEN_REPARSE_POINT, rejects directory/reparse handles, hashes its bytes via SHA-256 and records selectedPluginSha256, selectedPluginBytes and selectedPluginVerified. The hash is a provenance clue, not a substitute for resolving winning CELL overrides or original NIF asset sources. Failure to read the plugin leaves source verification false and cannot make the report renderable.
