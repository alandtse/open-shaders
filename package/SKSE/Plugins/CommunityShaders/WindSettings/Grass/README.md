# Grass Wind Settings

Open Shaders loads every `.json` file in deterministic filename order, then applies `zz_OpenShadersGrassAuthoring.json` last. Later files override profiles or mesh bindings with the same name. The in-game Grass Meshes page saves live authoring changes to that final overlay.

Profiles change how a source grass mesh responds to the single shared spring and gust field. They do not create additional spring fields.

```json
{
    "$schema": "GrassWindSettings.schema.json",
    "version": 2,
    "profiles": {
        "fine_tall_grass": {
            "bendSensitivity": 0.85,
            "flutterSensitivity": 1.35,
            "flutterTipConcentration": 1.25,
            "tipFlexibility": 0.75
        }
    },
    "grass": [
        {
            "mesh": "meshes/landscape/grass/grassfield01.nif",
            "profile": "fine_tall_grass"
        }
    ]
}
```

Every profile property is optional. Response multipliers and flutter tip concentration default to `1.0`; tip flexibility inherits the global grass setting. Multiple meshes can reference the same profile. Mesh matching is case-insensitive and accepts either slash style.

-   `bendSensitivity` (`0.0`–`4.0`) scales broad spring-field bending.
-   `flutterSensitivity` (`0.0`–`4.0`) scales ambient and impulse flutter displacement.
-   `flutterTipConcentration` (`0.0`–`3.0`) controls how strongly flutter is concentrated toward the authored blade tip weights. Higher values reduce flutter on the lower blade.
-   `tipFlexibility` (`0.0`–`1.0`) overrides the global broad-bend profile: zero follows authored vertex alpha linearly; one uses squared alpha to concentrate bending toward the tip. Alpha-zero vertices remain anchored at every value. Omit it or set it to `null` to inherit the global value. The menu's **Use Global** checkbox restores inheritance.

Version 1 files remain readable: their old `tipFlexibility` values still control flutter tip concentration. Saving a version 1 authoring overlay migrates those values to `flutterTipConcentration` and writes version 2.

The per-mesh controls change the response to the existing shared field. Flutter wind sensitivity (the sampled-speed input to the amplitude curve), flutter frequency, gust influence, flutter wave scale, low/medium/high wind amplitude response, maximum bend angle, downward wind bend share, spring frequency/damping, transient-specific controls, vanilla wind intensity, and enable toggles remain global. Gust shape, coverage, advection, direction, and field resolution/ranges are shared field settings, rather than mesh properties.
