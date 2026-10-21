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
            "flutterTipConcentration": 1.25
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

Every profile property is optional. Response multipliers and flutter tip concentration default to `1.0`. Multiple meshes can reference the same profile. Mesh matching is case-insensitive and accepts either slash style.

-   `bendSensitivity` (`0.0`–`4.0`) scales broad spring-field bending.
-   `flutterSensitivity` (`0.0`–`4.0`) scales ambient and impulse flutter displacement.
-   `flutterTipConcentration` (`0.0`–`3.0`) controls how strongly flutter is concentrated toward the authored blade tip weights. Higher values reduce flutter on the lower blade.

Broad wind bending follows authored vertex alpha linearly. Alpha-zero vertices remain anchored. Version 1 files remain readable for supported properties; saves write version 2.

The per-mesh controls change the response to the existing shared field. Flutter wind sensitivity (the signed fine-response gain), flutter travel rate, gust influence, flutter patch density, low/medium/high wind amplitude response, maximum bend angle, downward wind bend share, spring frequency/damping, transient-specific controls, vanilla wind intensity, and enable toggles remain global. Gust shape, coverage, advection, direction, and field resolution/ranges are shared field settings, rather than mesh properties.

Ambient flutter samples the signed detail band of the shared traveling fine wind directly. It has no separate repeating waveform. Sustained ambient wind supplies background motion, and local positive gusts increase its amplitude without restarting the pattern. Grass travel rate is integrated, so changing wind speed or the rate does not jump phase. Fine Wind Strength zero disables this ambient motion; impulse flutter remains independently controlled.

Bending receives the broad gust field and slow fine band through the existing spring. Flutter rotates the vertical deformation lever across the ambient wind direction before the spring rotation carries it into the bent pose. Its axis follows ambient wind rather than the instantaneous spring direction, so spring rebound does not reverse flutter. Horizontal mesh offsets remain intact; neither vertex normals nor inferred blade roots drive displacement. Vertex alpha controls both motions; alpha-zero vertices stay anchored. This supports authored grass meshes, but without individual blade pivots and segment lengths it remains a weighted deformation, not an inextensible blade simulation.

Flutter Strength and per-mesh Flutter Sensitivity scale the signed field response into a smooth angular limit of 10 degrees. The available range shrinks symmetrically near Maximum Bend Angle, instead of clipping one half of the oscillation and pushing against the spring. Downward compression is applied after both rotations. Flutter Tip Concentration retains the authored alpha weighting; no new vertex channels are required. Current/previous field responses and ambient directions drive the same deformation for motion vectors, and lighting normals follow the composed rotations. Detail fades when the field resolution cannot represent it. Over the outer 15% of the far field, local gusts, impacts, and flutter fade into a simple mean-wind bend that continues beyond simulation coverage.

Default Gust Amplitude is `1.0` (broad wind varies from `0x` to `2x` its base speed), Flutter Gust Influence is `2.0`, Flutter Patch Density is `0.5`, and low/medium/high flutter response is `1.0 / 1.0 / 1.01`. Fine Wind Strength defaults to `1.0` and supports `0..4`; additional airflow direction variation is limited to four degrees. Fine Wind Baseline is `0.25`, Ambient Coupling `0.35`, Gust Coupling `1.02`, and Travel Rate `1.0`. Saved settings retain their values; reset the Wind Field and Grass pages to adopt these defaults.
