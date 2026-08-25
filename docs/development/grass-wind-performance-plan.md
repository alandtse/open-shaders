# Grass Wind Performance Plan

## Goal

Recover the frame time lost after the shared grass-wind work while preserving
the useful ambient bending, gust response, transient impulses, and temporal
stability.

This is a living decision record. Update each item when its implementation or
runtime A/B result changes.

## Current cost model

The ambient grass path evaluates the wind field in the vertex shader for both
the current and previous positions. Each grass vertex therefore repeats work
that is identical for every vertex belonging to the same grass instance:

-   current and previous ambient-band evaluation;
-   current and previous procedural-noise evaluation;
-   current and previous transient-source evaluation;
-   spring target reconstruction;
-   current and previous deformation for motion vectors.

Changing the spring formulation did not measurably change performance. Gust
count was also tested at zero without a noticeable improvement, so gust count
is not currently treated as the primary regression.

## Decisions and progress

### 1. Evaluate only active transient impulses

**Decision:** Implement now.

**Status:** Implemented and retained.

Keep current and previous active counts, keep both arrays compact, and make the
shader loops terminate at those counts. The normal no-impulse case should
execute zero transient-source iterations instead of checking all 24 slots for
both temporal snapshots.

Temporarily removing this optimization did not restore the large regression.
It was not the primary cause, but it remains a valid reduction when transient
impulses are sparse or absent.

### 2. Remove duplicated vanilla flutter from ambient grass

**Decision:** Implement now.

**Status:** Implemented; runtime A/B identified the vanilla layer as the primary
regression.

When ambient grass wind is enabled, do not also calculate Skyrim's vanilla
displacement for current and previous frames and rotate both results through
the ambient bend. Instead, retain visible flutter with one inexpensive
per-instance phase that slightly modulates the resolved bend angle. Current and
previous phases preserve temporal motion. The existing strength and frequency
controls drive this replacement, and the vanilla path stays available when
ambient grass wind is disabled.

In the tested dense area, the instrumented aggregate `Grass::Draw` interval is
approximately 2 ms without the ambient path and 4 ms with it. This same-scene
A/B is useful, although the interval includes the whole contiguous grass draw
and should not be interpreted as the cost of replacement flutter alone.

### 3. Reduce ambient gust count

**Decision:** Do not pursue as a performance change.

**Status:** Rejected by runtime observation.

Setting the gust count to zero produced no noticeable performance improvement.
Keep gust-count tuning as a visual control, but do not expect it to recover the
missing frame time.

### 4. Cache the shared field in a texture

**Decision:** Keep as a possible future structural optimization.

**Status:** Deferred.

Evaluate ambient wind once per field texel per frame, then let grass sample
current and previous field textures with `SampleLevel`. A player-centered 2D XY
field is the smallest useful first stage because ambient gust bands are
horizontal. Counted transient impulses can remain analytic initially; a 3D
field remains an option if vertical transient behavior or additional consumers
justify it.

This changes the scaling model from wind-source work per grass vertex to
wind-source work per field texel.

### 5. Fast previous-frame deformation

**Decision:** Keep only as a last-resort performance option.

**Status:** Deferred.

Reuse the current ambient bend when producing `PreviousWorldPosition`, or use a
cheaper previous approximation. This removes the second field evaluation but
reduces wind-specific motion-vector accuracy and may increase TAA ghosting.

### 6. Evaluate wind once per grass instance

**Decision:** Investigate after the field-cache step.

**Status:** Design only.

All vertices in one blade share the same instance root, so the expensive wind
sample and rigid-bend target can be computed once per instance. A practical GPU
implementation would:

1. identify the grass instance count and expose the instance transforms or root
   positions to a compute pass;
2. allocate a structured buffer containing current and previous bend axis,
   angle, and compression per instance;
3. dispatch one compute thread per instance before grass drawing;
4. index the result from the grass vertex shader with a stable instance index;
5. retain only the cheap height/tip-weight deformation in each vertex.

The main integration risk is obtaining a stable instance index and matching the
engine's grass draw batches without copying or rebuilding large instance data
on the CPU. A cached field texture should be implemented first because it
captures most of the same reuse without requiring engine draw restructuring.

### 7. Remove redundant per-vertex shader work

**Decision:** Do before the field-cache prototype.

**Status:** In progress.

1. **Implemented:** Reuse the already calculated model-space position for the
   previous-frame output. Each mutually exclusive grass shader variant now
   copies that position before applying different current/previous wind offsets
   instead of evaluating its instance transform twice.
2. **Implemented:** Add a velocity-based wind-field sampling path. Grass
   previously calculated `length(baseVelocity)` and the shared sampler then
   reconstructed the same vector with
   `normalize(baseVelocity) * windSpeed` for both temporal samples.
3. **Next round:** Specialize ambient deformation for its known vertical blade
   vector. The general Rodrigues rotation can be reduced to one sine/cosine pair
   and direct component arithmetic. In the lighting variant, reuse that pair
   when rotating the current normal rather than evaluating the angle again.

These changes preserve the existing wind response. Item 1 remains available as
a small low-risk cleanup. Item 3 is the next planned implementation round.

### 8. Reduce transcendental arithmetic

**Decision:** Do not pursue replacement-flutter skipping or approximation.

**Status:** Rejected for replacement flutter; bend-target approximation remains
unplanned.

-   Approximate or simplify the bend-target `tanh` response only if the preceding
    work is insufficient, because that changes the tuning curve and therefore
    carries more visual risk.

## Z-axis assessment

Allowing nonzero Z wind does not materially increase the shader instruction
count. The grass shader already transforms a three-component velocity and
calculates downward compression even when Z is zero. A nonzero value changes
the result, not the executed algorithm.

Z motion can indirectly alter blade coverage and pixel overdraw, but it does
not change vertex count and is unlikely by itself to explain a large frame-time
regression. Treat a grass-only `velocity.z = 0` A/B as a diagnostic visual and
overdraw check, not as a primary optimization.

## Measurement queue

-   Measure each item 7 change independently against the current retained
    active-count and replacement-flutter baseline.
-   If the regression remains substantial, prototype the cached 2D field before
    spending time on spring arithmetic or gust-count tuning.
-   Keep scene, camera, resolution, upscaler, grass density, and weather fixed for
    every A/B comparison.
