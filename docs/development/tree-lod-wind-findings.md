# Tree LOD Wind Findings

Tree LOD wind work was paused after the experiments made on top of stable
commit `43d4db324` (`feat(grass): add elastic wind response`). The normal tree
and grass implementation at that commit remains the baseline.

## Confirmed rendering tiers

-   Full trees use the existing Lighting and Utility paths. Their trunk,
    foliage, previous-frame displacement, and vanilla `TREE_ANIM` integration
    work at the stable baseline.
-   DynDOLOD `passthru_lod` 3D models are individual geometry. Runtime logs
    identified `DoNothing FlatTrunk`, `Crown`, and `trunk` shapes below nodes
    such as `spruce_forest_big03_summer_0260B19Epassthru_lod`. An experimental
    Utility-shader displacement visibly moved these models.
-   Object LOD contains geometry named `treepassthru` and
    `treepassthru-LargeRef`. The renderer submitted these through Lighting with
    passes such as `4800042E` and `48000C2E`. Runtime selection and the shared
    permutation buffer both reached these draws, but the attempted deformation
    did not visibly bend the desired intermediate-distance trees.
-   A farther tree tier did react through the dedicated `DistantTree.hlsl`
    experiment. This tier uses instanced tree inputs, including per-instance
    position, scale, and rotation data.
-   Broad BTO/object-LOD experiments could move very distant combined geometry,
    but they were not selective enough and did not address the desired
    intermediate tier.

## Shader-path findings

-   LOD Blending proves object LOD is rendered by Lighting permutations with
    `LODOBJECTS` or `LODOBJECTSHD` pixel-shader definitions.
-   Open Shaders deliberately normalizes the Lighting `LODObjects` and
    `LODObjectHD` vertex techniques to the default Lighting vertex descriptor in
    `State::ModifyShaderLookup`. Therefore an HLSL vertex branch guarded only by
    `LODOBJECTS` or `LODOBJECTSHD` is not a reliable way to reach those vertices.
-   A per-draw bit in the existing shared permutation constant buffer can reach
    tagged `treepassthru` draws without allocating another constant-buffer slot.
-   Texture V is not a reliable height coordinate for combined object LOD.
    Local Z was tested as an alternative, matching the full-tree bend formula,
    but it still did not visibly deform the desired tier.
-   Every visible pass must use the same displaced position. Lighting, depth,
    shadow, alpha-tested, and previous-frame positions must agree or foliage can
    flicker, turn white, swim, or leave color at its original position.
-   BTO/object LOD batches can combine many references. A draw-level flag can
    identify a tree-containing batch, but it cannot necessarily separate every
    tree or provide a per-tree root without using the batch's instance data.

## Runtime evidence

The experimental log markers established the following:

-   `[PassThruLODBend]` fired for individual 3D hybrid LOD shapes.
-   `[PassThruLODBinding]` confirmed the shared permutation buffer was bound to
    vertex slot `b4` for those draws.
-   `[TreePassThruBend]` fired repeatedly for `treepassthru` and
    `treepassthru-LargeRef` Lighting draws.
-   The final attempted local-Z deformation still produced no visible movement
    in the target intermediate LODs.

## Recommended next investigation

1. Capture a non-moving intermediate tree draw and a working farther
   `DistantTree` draw in RenderDoc, then compare shader class, vertex layout,
   instance buffers, constant buffers, and render passes.
2. Trace the engine setup used for `treepassthru` object LOD rather than
   inferring the layout from NIF names or parsing NIF files at runtime.
3. Determine how the combined object-LOD vertex data encodes each instance's
   root and height. Do not resume bending until that coordinate system is
   known.
4. Reintroduce support tier by tier: color Lighting pass first, then matching
   depth/shadow passes, and finally previous-frame displacement.
5. Preserve the dedicated `DistantTree` and `passthru_lod` approaches as
   separate future changes; they target different renderer paths.
