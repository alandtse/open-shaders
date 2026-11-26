# DX12 shadow offload validation

`tools/dx12-offload-validation.py` answers two questions on the standard
`DragonsreachTorchThirdPerson` route: does shadow offload engage, and is it worth its cost?
It drives an SE/AE game you hold through devbench. DX12 does not run on VR.

## What it measures

| Signal                                              | Source                                                         | Meaning                                                                                                                         |
| --------------------------------------------------- | -------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| Strength 4 vs strength 0 at one pose                | `ShadowStrength` toggled live                                  | Whether the RT shadow layer reaches the frame at all                                                                            |
| Prepared shapes, tile states, top rejection reasons | `DX12NativeScene` `status` query, `grid`                       | Whether subcell preparation found the scene's rigid geometry, and what it rejected                                              |
| TLAS instances                                      | `status`, `tlas`                                               | How many shapes the ray-traced scene holds                                                                                      |
| `castersTraced` / `castersKeptRaster`               | `status`, `ownership`                                          | Static caster draws moved from raster to ray tracing, against static casters of owned lights the TLAS lacks (those stay raster) |
| Raster vs offload screenshots                       | three fixed poses, Mode 1 then Mode 0                          | Image change, compared with a repeat raster capture as the noise floor                                                          |
| Frame time, p99, shadow GPU time                    | `inspect kind=profiler` during raster, offload, raster replays | Cost of offload against raster in the same session; the two raster runs bound run-to-run noise                                  |

`Mode` 1 is raster comparison (DX12 active, no offload) and switches live, so `ab` compares both
in one launch. The DX12 master switch is restart-gated, so the DX12-off baseline needs its own launch.

## Procedure

1. Deploy with `ALL-WITH-AUTO-DEPLOYMENT-SE` while you hold the SE lock, and hash-check the deployed DLL.
2. With DX12 enabled in `SettingsUser.json`, launch, load a save, then run:
   `python tools/dx12-offload-validation.py ab --out <dir>/ab --game-dir "<Skyrim Special Edition>"`
3. Disable DX12, relaunch, then run `baseline --out <dir>/baseline --game-dir ...`.
4. `python tools/dx12-offload-validation.py compare --out <dir>/compare --ab <dir>/ab --baseline <dir>/baseline`.

The `ab` phase takes about five minutes (three 62 s replays); `baseline` about three.

## Reading the result

- **FAIL** on `tiles prepared`, `TLAS published` or `raster casters offloaded`: offload does not
  engage, so no performance number means anything. The tile rejection reasons say why.
- **FAIL** on `RT shadow layer reaches the frame`: offload removes raster static shadows but the ray-traced
  replacement never lands, so the scene loses those shadows.
- **REVIEW** on a pose: open the `_diff.png` (red is darker under offload, green brighter). Shadows
  should keep their shape; uniform darkening means the native layer is double-shadowing.
- Frame time is reported against the 16.7 ms frame budget. A `perf:` claim needs `offload` at or
  below `baseline` across repeated runs, not one sample.
