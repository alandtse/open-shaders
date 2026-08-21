# World-Space Wind Noise Plan

## Status

This is a deferred design plan. No runtime or shader implementation exists yet.

## Goal

Add an effectively infinite wind field that coordinates trees, 3D tree object
LOD, grass, and other wind consumers across the world. The field should create
gust fronts and local variation without allocating a map-sized texture.

The existing weather direction, weather strength, and global gust controller
remain the baseline. World-space noise modulates that shared signal rather than
replacing it.

## Field Model

Evaluate procedural noise from absolute world coordinates:

```hlsl
float2 fieldPosition =
    originalWorldPosition.xy / wavelength -
    windDirection * elapsedTime * propagationSpeed;

float regionalWind = SmoothNoise(fieldPosition);
```

`SmoothNoise` hashes the four integer lattice cells surrounding the sample and
interpolates between them. Evaluation cost and memory use are constant no
matter how large the worldspace becomes.

Combine several scales to avoid a visibly uniform or repetitive result:

```text
weather baseline
    * global gust strength
    * broad traveling gust field
    * medium wind waves
    * subtle local variation
    * consumer sensitivity
```

Suggested initial wavelengths:

-   Broad gust field: 16,000 to 40,000 Skyrim units.
-   Medium waves: 4,000 to 12,000 Skyrim units.
-   Local variation: 1,000 to 3,000 Skyrim units.

Every scale, speed, strength, and seed should be exposed for live tuning.

## Gust-Envelope Propagation

Procedural noise alone does not make the existing gust controller propagate.
Multiplying every location by the controller's current gust value would still
make the main pickup and letup happen everywhere simultaneously. The moving
noise would add local variation, but it would not let downwind vegetation keep
the previous gust until the new front arrives.

Preserve the existing random gust formula, including its pickup, hold, and
letup behavior, but retain a small rolling history of its transitions. Evaluate
that history at a position-dependent delayed time:

```hlsl
float downwindDistance = dot(
    originalWorldPosition.xy - gustWaveOrigin,
    windDirection);
float delayedTime = currentTime - downwindDistance / gustTravelSpeed;
float localGustEnvelope = EvaluateGustHistory(delayedTime);
```

This creates the intended propagation:

```text
upwind vegetation   = new gust
transition region   = gust front
downwind vegetation = previous gust
```

The history is global, not per tree. A compact buffer containing roughly the
last 8 to 16 gust transitions, their timestamps, and interpolation parameters
is sufficient, provided it covers the maximum visible travel delay. Trees,
grass, and other consumers sample the same delayed history at their location.

Procedural noise should then distort and vary the traveling front rather than
replace its envelope:

```text
weather baseline
    * delayed gust envelope
    * traveling procedural variation
    * consumer sensitivity
```

The current and previous frames must independently evaluate the history using
their actual times and wind state so motion vectors remain correct. A change in
wind direction also needs a defined transition policy; abruptly projecting old
history onto a new direction would visibly rotate the gust front.

## Shared Consumers

Trees and grass must sample the same current and previous wind-field state.
Their response curves and sensitivities may differ, but a traveling gust should
reach both systems at the same world location and time.

Other consumers can use the evaluated global result later. The wind-field
implementation should therefore live in shared wind state and shared HLSL
helpers instead of belonging exclusively to the tree or grass feature.

## Sampling Rules

1. Sample undeformed world position. Sampling a displaced vertex feeds motion
   back into the field and causes swimming.
2. Sample a stable object or plant anchor when one is available. This keeps all
   vertices of one object on the same wind value.
3. Keep current-frame and actual previous-frame field inputs. Do not assume a
   fixed frame interval when generating motion vectors.
4. Use the same absolute coordinates, direction, seed, and timing at every LOD
   level so transitions do not change wind phase.
5. Keep noise deterministic. Camera movement must not alter the sampled field.

## World-Scale Precision

Do not animate small noise cells directly with large 32-bit world coordinates.
Split each sample into an integer lattice cell and a small fractional offset,
or evaluate relative to a stable regional origin while retaining an absolute
integer cell index for hashing.

Camera-relative rebasing is acceptable only if it produces exactly the same
field value before and after a rebase. A purely camera-relative field would
make the wind pattern follow the player and must not be used.

## 3D BTO Constraint

Generated BTO object LOD merges many placed trees and unrelated objects into
material-oriented draw batches. The runtime geometry therefore lacks a normal
per-tree transform or origin.

The planned first step is to bake a bend-weight gradient into vertex alpha on
the source 3D tree LOD NIFs before DynDOLOD generation. DynDOLOD can preserve
vertex colors for passthru LOD, allowing the shader to distinguish flexible
tree vertices from ordinary object LOD after merging.

Broad world-space noise can be sampled per BTO vertex because its value changes
very little across one tree. This is visually coherent but not mathematically
identical across every vertex. Strictly constant per-tree variation would need
LODGen to stamp a placement seed, or a generated sidecar/vertex mapping that
survives BTO merging.

## Proposed Implementation Order

1. Add a shared deterministic 2D value-noise helper with precision-safe lattice
   addressing.
2. Add a compact CPU-side gust transition history and a shared GPU evaluator.
3. Add current and previous field parameters to the existing shared wind data.
4. Visualize the delayed gust envelope and one noise layer in the debug UI.
5. Apply the broad field to full trees and grass using stable object anchors.
6. Add medium and fine layers and tune their relative amplitudes.
7. Verify synchronized gust travel and correct motion vectors at varied frame
   rates.
8. Bake source 3D tree LOD vertex-alpha bend weights and regenerate DynDOLOD.
9. Apply the same broad field to marked 3D BTO vertices, then validate LOD
   transitions and ensure non-tree object LOD remains stationary.
10. Decide whether spatially smooth BTO variation is sufficient or whether a
    generator-side per-tree seed is worth the additional tooling.

## Validation

-   Standing still must show gust fronts moving through the world.
-   Moving the camera must not move or reset the field.
-   Nearby trees and grass must react to the same passing gust.
-   Downwind vegetation must retain the previous gust until the new envelope
    reaches it; the main pickup and letup must not occur globally at once.
-   A full tree and its 3D BTO replacement must match direction and phase through
    the transition.
-   Rocks, buildings, terrain, and unmarked BTO geometry must remain stationary.
-   Current and previous deformation must remain stable with variable frame time.
-   Skyrim VR must retain its existing stereo, eye-index, clip, and cull paths.
