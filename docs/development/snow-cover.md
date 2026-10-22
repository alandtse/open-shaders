# Snow Cover

Snow Cover ports the seasonal snow, weather accumulation, foliage tint and
placed-fire melting from Bottle Shaders' `Bottle-Compendium` branch at
[`4e093c99451b130981f15777fd2dc0edf9d14a08`](https://github.com/InTheBottle/Bottled-Shaders/tree/4e093c99451b130981f15777fd2dc0edf9d14a08).
It includes the source snow map, fallback material textures, world configs
and geometry exclusion lists. This feature does not swap meshes or textures
through Seasons of Skyrim.

## Configuration

The feature appears as **Snow Cover** in the Sky category and is disabled
by default. **Enable Snow Cover** is the master switch for snow, foliage
tint, fire melting and Survival Mode cold. Turning it off stops snow updates,
releases its textures and skips snow shading through the shared enable flag.
Both states use the same shader permutations, so the master switch does not
clear or regenerate the shader cache.
The worldspace checkbox applies only within the enabled feature and is saved
in the world config. Older configs enable it by default. The master switch
preserves unsaved world controls and texture-path edits.
User settings control the altitude offset, detailed
foliage snow, movable objects, harvestable plant tint and fire melting.
Exposed building roofs can receive snow below the surrounding snow line.
The optional Survival Mode integration makes snow-covered ground count as
freezing, with hysteresis to avoid switching at the coverage boundary.

World configs live in `Data/Shaders/SnowCover/<EditorID>.json`. The bundled
configs cover Tamriel, MarkarthWorld, RiftenWorld, SolitudeWorld,
WhiterunWorld and WindhelmWorld. An interior cell uses its own editor ID.
Worlds without a config start with snow disabled. Save a world config and
reload it to initialize its textures. Texture paths are relative to Data
and omit `.dds`; material sets also load `_n.dds` and `_rmaos.dds`.
Textures can be loose files or BSA resources. Missing material textures use the bundled defaults. A missing altitude
map uses a neutral single-pixel texture.

Config edits take effect in memory except texture paths, which require
**Save worldspace config** followed by **Reload config**. Saving is explicit.
Reload also refreshes `whitelist.txt` and `blacklist.txt`. Entries are exact
geometry names, with optional `#` comments. Character shaders are excluded;
the object blacklist overrides movable-object and whitelist allowances.
Animated trees retain Bottle's tree snow path.

The existing devbench feature interface exposes user settings and the
`Reload`, `SaveConfig` and `WorldDefaults` commands. Diagnostics report the
world, active state, fractional month, seasonal altitude, accumulated
weather snow and fire counts.

## Integration and cost

-   Lighting applies snow after skylighting/water-height evaluation and
    before wetness. Grass and distant trees apply snow before lighting.
    Existing PBR and legacy color paths remain separate.
-   Snow uses PS resources `t38` through `t44`. Its 336-byte feature data is
    appended to `b6`, preserving existing fields' offsets. Permutations built
    without the feature omit the new declarations.
-   No additional draw, dispatch, render target or full-screen pass is added.
    Snow runs at the affected geometry's render resolution: one altitude-map
    sample per environmental evaluation, then three material samples for
    covered surfaces. Foliage uses an albedo sample and optionally one extra
    alpha sample. Tree tint and surface snow each evaluate the snow map.
-   PBR snow uses the existing glint precomputation and adds one noise-map
    fetch per direct-light BRDF evaluation where glints are active.
-   Fire melting scans nearby references every 0.25 seconds, classifies at
    most 512 new base forms per scan, and uploads at most eight fire regions.
    Covered pixels evaluate up to eight distance falloffs. Disabling fire
    melting skips the scan and uploads zero regions.
-   Base-form classification is cached until a world change, scene reset or
    config reload. It stores form IDs and booleans, not scene-node pointers.
    Each scan reads the current visible geometry of known fire references.
-   Geometry exclusions use the existing render-pass contract. They update
    CPU flags for the normal draw path's existing buffer upload and restore
    them after the pass. No extra per-pass buffer upload or engine hook is added.
-   R8 and BC4 altitude maps get a CPU maximum hierarchy once per world config load.
    Object bounds query at most four hierarchy texels; unknown map regions retain
    the full shader path. Positive weather accumulation also retains that path.
    Foliage samples its edge alpha only when seasonal or weather snow can appear.
-   Snow-only HSV helpers opt into `Common/Color.hlsli`. Snow constants are pixel-only,
    so vertex permutations retain the same preprocessed content and can reuse
    Content Store blobs without changing shader flags or output.
-   CPU scopes expose frame updates and fire scans to the built-in profiler.
    Survival's condition global is written from the existing game-thread weather
    callback and cleared on runtime disable. No plugin or save file is written.

Season interpolation wraps across the year and supports either ordering
of the summer/winter peaks. Weather accumulation uses elapsed game days
converted to hours so full-day waits are retained. Settings are validated
when loaded or edited. Resource failures disable the world overlay.

## Validation

Build the universal Release DLL with `BuildRelease.bat`. Prepare shaders
before targeted FXC validation. Exercise Lighting, RunGrass and DistantTree
with Snow Cover enabled, including PBR, legacy, terrain, LOD, alpha test,
excluded character materials, VR and reverse-Z permutations. The C++
`[snow-cover]` test tag checks seasonal boundaries and weather accumulation.

For a runtime check, use AE on this workstation and validate VR permutations offline. In the same exterior,
compare summer and winter, snowfall and rain, and nearby placed fires.
Check grass, full trees, distant trees, terrain/LOD transitions and excluded
actors; then enter an unconfigured interior and return. Confirm the shader
cache is idle with no failures. Compilation and numerical tests do not
establish visual parity or measured frame cost.
