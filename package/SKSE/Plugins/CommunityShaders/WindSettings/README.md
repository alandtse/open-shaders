# Wind Settings

Tree response files live in this directory. Grass response profiles and mesh bindings live in the `Grass` subdirectory and are documented there.

Place tree-response JSON files in this directory. Open Shaders scans every `.json` file in deterministic filename order at startup; `WindSettings.schema.json` and `*_backup.json` files are ignored. If multiple files define the same parameter for the same mesh, the later file wins and one warning lists every conflicting JSON file.

If this directory contains no settings JSONs, every tree uses the built-in defaults for every response parameter.

In-game edits remain live until saved or reverted. Saving writes only changed properties to the last-scanned JSON containing that mesh, so omitted properties remain omitted. Reverting before a save restores the values loaded from disk.

```json
{
    "$schema": "WindSettings.schema.json",
    "version": 1,
    "trees": [
        {
            "mesh": "meshes/landscape/trees/treepineforest01.nif",
            "bendSensitivity": 1.0,
            "leafAmbientSensitivity": 2.01,
            "upperBendRange": 100,
            "maximumDisplacementPercent": 10,
            "trunkGustInfluence": 0.5,
            "leafGustInfluence": 1.0,
            "transientWindInfluence": 1.0,
            "leafTransientWindInfluence": 5.0,
            "leafTransientFlutterMaximum": 10.05,
            "transientMaximumBendMultiplier": 2.49
        }
    ]
}
```

Every response property is optional. Omit any property to use its coded default; an entry containing only `mesh` is also valid. Use omission rather than JSON `null`.

The coded defaults are:

-   `bendSensitivity`: `1.0` (`0.0`–`4.0`)
-   `leafAmbientSensitivity`: `2.01` (`0.0`–`4.0`)
-   `upperBendRange`: `100` (`5`–`100` percent of measured height)
-   `maximumDisplacementPercent`: `10` (`0`–`10` percent)
-   `trunkGustInfluence`: `0.5` (`0.0`–`2.0`)
-   `leafGustInfluence`: `1.0` (`0.0`–`2.0`)
-   `transientWindInfluence`: `1.0` (`0.0`–`5.0`), scaling trunk response to short-lived impulses such as Unrelenting Force
-   `leafTransientWindInfluence`: `5.0` (`0.0`–`5.0`), scaling leaf response to short-lived impulses independently of the trunk
-   `leafTransientFlutterMaximum`: `10.05` (`0.0`–`20.0`), capping only the transient contribution to vanilla leaf flutter for each mesh
-   `transientMaximumBendMultiplier`: `2.49` (`0.0`–`5.0`), capping the trunk response to those impulses

Mesh matching is case-insensitive and accepts either slash style. The in-game Global Override is runtime-only and can temporarily replace every per-tree response for testing.

Tree spring state is simulated on the GPU using the global Wind settings. Broad wind and gusts drive this spring; a slower signed fine-wind band adds coherent trunk sway after it. Faster fine detail drives managed `TreeAnim` geometry through its existing vertex alpha and normals. The same detail deformation is used for visible and shadow geometry, with current/previous field history for motion vectors. Disabling Trunk Bend leaves managed detail animation available.

`TreeAnim` does not reliably identify individual leaves and branches. The detail response therefore reuses authored animation weights without requiring new textures or mesh authoring. It does not apply rapid detail displacement to trunk-only geometry. Tree height and shared probes already shape structural bending; the traveling ambient field is horizontal, with no automatic height-dependent increase in air speed.

The Wind Field page exposes Fine Wind Strength, Baseline, Ambient Coupling, Gust Coupling, and Travel Rate. Fine motion continues under sustained wind with no passing gust; positive gusts increase its amplitude without restarting phase. Slow and detail bands have separate limits so fast detail cannot leak into trunk sway through a shared clamp. Transient impacts retain their separate response controls. Per-mesh trunk/leaf gust influences tune the fine gust boost; global tree Gust Scale and Gust Soft Limit control the broad spring target.

The default gust fronts are 600 units along the wind and 1100 units across it, with distortion strength/size `1.0`, distortion rate `0.25`, coverage `0.5`, and edge softness `0.31`. Saved configurations retain their values; reset the Wind Field page to adopt new defaults. Start with shared weather presets, then use Scene Manager overrides to fine-tune locations. Grass and tree global controls remain shared within the active scene, while mesh profiles customize individual vegetation responses.
