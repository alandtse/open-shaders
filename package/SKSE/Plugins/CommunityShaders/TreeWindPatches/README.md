# Tree Wind Patches

Tree wind settings apply model-specific sensitivity multipliers without redistributing NIF files. Open Shaders loads `NatureOfTheWildLands.json` from this directory at startup. The in-game Tree Meshes tab edits its values live and can save the complete catalog back to that file.

On first load, Open Shaders copies the editable catalog to `NatureOfTheWildLands_backup.json`. The backup is not loaded as settings. If the editable catalog is later missing, Open Shaders restores it from the backup. If neither file exists, no model rules are loaded.

```json
{
    "$schema": "TreeWindPatches.schema.json",
    "version": 1,
    "trees": [
        {
            "mesh": "meshes/landscape/trees/treepineforest01.nif",
            "bendSensitivity": 0.7,
            "leafAmbientSensitivity": 0.9
        }
    ]
}
```

Both sensitivities are multipliers from `0.0` through `4.0`. Omitted fields retain the default multiplier of `1.0`. Matching is case-insensitive and accepts either slash style.
