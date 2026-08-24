# Per-Tree Wind Bend Profiles

## Status

Deferred design only. Do not implement this as part of the current tree-wind
work.

## Problem

A single height at which a tree reaches full displacement creates a plateau:
vertices above that height receive the same displacement. This can make the
upper tree appear rigidly offset from the trunk. The profile needs to describe
where bending starts and how its sensitivity increases toward the treetop.

## Proposed Authoring Model

Each tree mesh may have an editable bend-profile graph:

-   The horizontal axis is normalized mesh height: base at `0`, treetop at `1`.
-   The vertical axis is normalized trunk-displacement weight: no motion at `0`,
    maximum motion at `1`.
-   The first editable point defines where bending begins.
-   The final point remains fixed at `(1, 1)` so maximum displacement is reached
    at the treetop.
-   Interior points shape a smooth, top-biased response.
-   Control points must be monotonic in both axes to prevent inverted response,
    downward kinks, and displacement plateaus.

The default profile should remain available for meshes without an explicit
profile. Per-mesh profiles belong beside the existing mesh-path wind rule,
along with trunk and leaf sensitivity.

## Runtime Representation

The graph editor is menu-only. On load or edit, bake its curve to a compact
fixed-size lookup table or equivalent polynomial coefficients. For each tree
mesh draw, bind only that mesh's profile with its existing bounds and
sensitivities; the shader must not search or bind all tree profiles.

A small baked lookup table is suitable for the vertex shader: normalize the
vertex height, sample adjacent entries, and interpolate. Do not evaluate an
arbitrary Bezier curve per vertex, since an editor with movable horizontal
control points would require solving the curve for height.

## UX Considerations

-   Start with one default curve plus optional per-mesh overrides.
-   Provide reset, copy, and paste operations for mesh profiles.
-   Consider reusable presets for related tree meshes.
-   A later painted per-vertex mask may supplement the curve where a safe,
    unused mesh attribute exists; it should not be required for general mod
    compatibility.

## Performance Expectations

Curve storage is small (for example, 16 floats per mesh profile). Runtime work
is one per-vertex profile lookup and interpolation, while only the active
mesh's profile is bound for a draw. Measure the final implementation before
making a performance claim.
