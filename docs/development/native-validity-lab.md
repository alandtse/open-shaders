# Native Lighting Validity Lab

Validity Lab is a runtime experiment surface. The lighting producer publishes
separate R32_UINT OriginalValidity, EffectiveValidity, and RejectionReason textures.
Original validity retains the strict renderer's decision; effective validity may
reflect selected depth, material, or experimental mask policies. Values are 0
unavailable, 1 evaluated, 2 culled, and 3 unsupported. RejectionReason uses its
low byte for the first strict reason, bit 31 for forced acceptance, bit 30 for an
experimental mask cull, and bit 29 for initialized finite lighting evaluation.

Reasons are 1 invalid camera/ray, 2 no primary hit, 3 raster-depth mismatch,
4 unsupported material, 5 material evaluation failure, 6 no eligible lights,
7 invalid secondary visibility, 8 non-finite lighting, 9 authoritative mask cull,
and 10 successful evaluation. The existing mask is never treated as authoritative
in strict mode. Experimental culls retain the original reason and use bit 30.
The mask is sampled only when requested for diagnostics or experimental use and
only when its frame, scene, device, dimensions, fence and resource are valid.

Reference-compatibility defaults are enabled for hands-on coverage testing: the
selected absolute receiver-depth tolerance is 5.0 (the old RT-Direct threshold),
secondary visibility uses reference-like candidate skipping, and primary traversal
skips rejected unsupported candidates rather than invalidating the entire pixel.
Native Composite defaults to Effective acceptance for both direct lobes so
recovered finite radiance is visible. OriginalValidity still uses the current strict
2.0-unit depth test and records rejected primary candidates as unsupported, allowing
strict vs effective comparisons without changing resource or geometry safety. The
reference-like policies may expose surfaces behind unsupported geometry or cause
light leaks; switch them off for strict behavior. Only the compatibility policy
is changed here: no temporal reconstruction, RR, raster replacement or lighting
BRDF changes are introduced.
Selected depth tolerance, diagnostic continuation, or disabling agreement can
recover an otherwise safely evaluated receiver. Diagnostic material fallback uses
a neutral material only after triangle and normal data are validated. Geometry-only
mode visualizes confirmed DXR triangle candidates and does not manufacture a valid
material for unsafe or unknown-opacity geometry. Raster receiver identity is not
currently captured, so identity matching is unavailable.

Native Composite independently selects strict, effective, or finite-produced data
for diffuse and specular. Its diagnostic modes show original/effective verdicts,
forced acceptance, reasons, primary triangle candidates, depth error, mask state,
raw lobes, and strict-versus-effective lighting difference. Alpha is preserved.
These debug views replace the raster RGB only when explicitly selected; ordinary
lighting remains additive over the raster baseline. The UI includes the color legend.

Experimental settings are not persisted unless their feature's persistence option
is enabled. Both features expose `validityLab` and `validityLabStatus` DevBench
actions. Policy revisions travel with a frame and reset the native FSR history when
the active producer or compositor policy changes.

Lighting diagnostics report GPU submissions, GPU-completed readbacks, original
reason counts, recovered pixels and experimental mask culls. Composite diagnostics
report submitted/completed work, accepted, modified and visualization pixels after
readback. `visuallyConsumedPixels` is null: these counters do not prove presentation.
The render-to-DX11 return counter is a queued-copy count, not a displayed-pixel
count. CPU readbacks are polled only when a slot's GPU fence has completed.

Resource existence, matching frame and device, valid geometry and descriptors,
finite ray inputs, and GPU lifetime/fences are never bypassed. Missing scene,
acceleration or lighting resources remain unavailable. VR remains on its existing
path. Runtime visual behavior, recovery counts and strict-versus-forced comparisons
require the owner's in-game verification.

The reference-like visibility policy (enabled by default for testing) skips unsupported shadow-candidate
materials after checking the instance and geometry buffers, matching the reference's
candidate handling. It also skips lights whose shadow segment is too short to trace.
The original visibility rejection remains recorded; these approximations can cause
light leaks or missing nearby light. Select effective or finite acceptance in Native
Composite to display recovered results. Receiver depth and material checks have their
own separate controls; missing resources and invalid geometry remain hard failures.
