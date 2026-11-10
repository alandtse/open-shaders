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


## Automatic CELL identification (no manual plugin/ID entry)

DX12 Resources now automatically chooses the exact current closed interior from PlayerCharacter::GetParentCell while playing. When the Skyrim Main Menu is open, it scans normal redirected-Documents Skyrim SE/GOG Saves directories on a background worker, selects the most recently modified valid ESS file, parses its bounded uncompressed save header, and extracts the structured filename CELL EditorID when present. Main-thread lookup resolves the hint against registered interior CELL forms. A unique exact EditorID takes precedence over a unique display/location-name match. Duplicates, exteriors, missing saves, inaccessible profiles and unmatched location names leave the target unresolved; the feature does not guess a FormID. Returning to the main menu invalidates an earlier active-cell detection. The Refresh detected interior button rechecks the current context; Prepare Detected Interior (source discovery) runs the same asynchronous, non-renderable report workflow as before. Profile-local saves outside the standard folders are not yet discovered automatically; changing the selected save is not yet tracked independently of Refresh.


## Plugin-scoped interior browser

The DX12 Resources panel also offers a **Browse interior cells by plugin** section. It enumerates only Skyrim's **loaded** regular and ESL/light plugins from TESDataHandler, so players select an existing plugin from a dropdown rather than entering a filename or FormID. Plugin-name and interior-name filters are included, and the interior list shows display names, EditorIDs, original source-plugin/local FormIDs, and whether a CELL is **defined** or **overridden** by the selected plugin. Unknown/unloaded mods are not fabricated.

Selecting a plugin initiates an explicit, on-demand CELL scan. The loaded CELL form array is scanned in 512-record batches on the UI thread; only matching closed interiors are copied to owned metadata. This does not access live RE pointers from detached workers. The list can be rescanned manually, and the work does **not** automatically prepare all listed cells. The selector never uses the plugin's override identity as a replacement for the CELL's original owning identity.

**Known-interior gate removed:** A selected plugin CELL can start **Prepare Selected Interior (source discovery)** independently of the auto-detected current cell, latest save, or whether the destination cell is currently attached. The original target lookup and IsInteriorCell() validation remain in place. Actual full NIF/REFR geometry/light preprocessing has not yet been ported; the output is still a non-renderable source discovery report and is not eligible for RT scene publication. The plugin browser adds reliable user-selected targeting, not an unsupported claim of resource completeness.
