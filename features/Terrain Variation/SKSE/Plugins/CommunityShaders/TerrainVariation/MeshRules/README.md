# Terrain Variation mesh rules

Place JSON files in `Data/SKSE/Plugins/CommunityShaders/TerrainVariation/MeshRules/`.
Open Shaders loads every `.json` file in this folder and its subfolders once
at game startup. Restart Skyrim after adding, editing, or removing rules.
Mods can supply their own named files without replacing `Default.json`.

Each file contains an `include` whitelist, an `exclude` blacklist, or both.
Both objects accept the same optional arrays:

```json
{
    "include": {
        "directories": ["landscape/mountains/"],
        "paths": ["mytextures/rock.dds"],
        "filenames": ["customcliff.dds"]
    },
    "exclude": {
        "filenames": ["dirtcliffsroots01.dds"],
        "paths": ["landscape/example/specifictexture.dds"],
        "directories": ["landscape/trees/"]
    }
}
```

-   `filenames` matches a texture filename in any directory, including PBR copies.
-   `paths` matches an exact texture path relative to `Data/Textures/`.
-   `directories` matches a directory and all its subdirectories. A trailing slash
    is optional; `landscape/trees` does not match `landscape/trees2`.

Matching ignores case and treats forward and backward slashes equally. Paths
may include the `Textures/` or `Data/Textures/` prefix. Filename and path entries
must end in `.dds`. Wildcards, absolute paths, and parent traversal are unsupported.

Rules from all files are combined, so file order does not matter. A blacklist
match always wins over both the whitelist and automatic matching. Otherwise,
a texture qualifies when any of these conditions holds:

-   It is directly under `landscape/`, with no further subdirectory.
-   It is referenced by a landscape texture record, including seasonal swaps.
-   It matches a whitelist rule.

Rules affect mesh variation only; they do not alter landscape rendering.
Whitelisting changes texture eligibility only. Existing alpha-testing, decal,
wrapping, and shader restrictions still apply.

`Default.json` whitelists the mountain and dirt-cliff directories, including their
PBR equivalents. It blacklists tree textures and dirt-cliff root textures,
including opaque root geometry. Remove an entry from every file that supplies
it to remove that whitelist or blacklist rule.

Invalid files are logged in `CommunityShaders.log` and skipped in full; other
valid files still load. With no valid rules, only the automatic eligibility checks
apply. The previous parent folder is not scanned. Matching results are cached
for the game session and cleared whenever startup data is loaded again. No
rules files are read while rendering.
