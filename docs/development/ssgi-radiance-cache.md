# Screen Space GI radiance-cache ownership

## Position contract

`Util::GetCameraData(eye)` reads `RendererShadowState::cameraData`.
`ScreenSpaceGI::UpdateSB` inverts its `viewMat` into `InvViewMat[eye]`.
The optional VR unjittered path uses the cached engine `CameraViewInverse`
instead. Neither path adds the camera position adjustment.

These matrices operate in Skyrim's camera-relative world space. The
captured AE matrix in `tests/cpp/test_camera_reprojection.cpp` has no
absolute camera translation. Existing SSGI reprojection uses the same
contract: `radianceDisocc.cs.hlsl` adds `CameraPosAdjust` after current
inverse-view reconstruction and `CameraPreviousPosAdjust` after previous
inverse-view reconstruction. `Common/TemporalReproject.hlsli` likewise
converts between these origins explicitly.

The RC capture path is:

1. Read signed linear working depth through `READ_DEPTH` at the configured
   full/half/quarter-resolution mip.
2. Reconstruct view position with `ScreenToViewPosition`.
3. Apply `InvViewMat[eye]` with homogeneous division to obtain
   camera-relative world position.
4. Add `FrameBuffer::CameraPosAdjust[eye].xyz` **once** to obtain absolute
   world position. Normals and view directions receive rotation only.
5. Normalize against the snapped absolute world center and the producing
   half extent: `u = (worldPosition - center) / (2 * extent) + 0.5`.

`Util::GetEyePosition(0)` already returns the engine `posAdjust` in absolute
world coordinates. The volume center snaps that position directly. Adding
another position adjustment to either the center or the shader would
violate this contract. Both VR eyes use their own relative origin before
normalizing into the same world-space volume.

The provider receives these normalized inputs unchanged; the CPU only
validates and compacts them. It does not add an offset. Predictions retain
the original physical 64x64 backing index. `rcComposeRadiance.cs.hlsl`
interpolates the valid screen-grid predictions using their producing grid
size; it performs no world-position reconstruction or camera adjustment.
This is source and captured-matrix evidence, not new in-game acceptance.

## Delayed capture and reset contract

Each of the three paired staging slots owns its grid size, producing half
extent, and domain generation. Enqueue records these from the constant
buffer snapshot used by the shader, after both copies are queued and
before marking the slot pending. A full ring preserves the occupied slot.
A quality change affects only future captures.

Both staging maps use `D3D11_MAP_FLAG_DO_NOT_WAIT`. If the teacher is still
busy after the input becomes ready, the input is unmapped and the whole
slot remains pending. Metadata remains unchanged until both maps succeed.
P95 conversion uses `abs(u - 0.5) * 2 * producingExtent` per axis, never the
live adaptive controller's extent.

A snapped-center or extent change advances the domain generation and
requests a provider reset. Pending staging resources remain owned by their
slots until normal nonblocking readiness and retirement. An older-generation
capture contributes zero samples, so it cannot adapt, train, or infer in
the new normalized domain. No GPU drain or early resource reuse is needed.
Generations also distinguish a change away from and back to an old domain.

The provider's existing single in-flight batch is polled to completion.
Its producing grid and generation are replaced only when
`a_outputSubmitted` is true. The backend initializes that flag to false,
leaves it false on busy, poll-only and failure paths, and sets it true only
after command-list execution and successful fence signaling. The caller
snapshots the completed batch metadata before dispatch can submit another
batch in the same call. Old-generation completions are retired without
publication.

On publication, the radiance texture and its result grid-size constant are
updated before composition. The constant-buffer upload uses its existing
write-discard path and the current frame's retained data, preserving every
other shader input. Composition requires a published result from the
current generation. No new fence wait or staging-map wait is introduced.

## Regression coverage

`tests/test_ssgi_radiance_cache.py` compiles production method bodies and
capture/readback blocks against CPU recording mocks. It exercises delayed
paired readiness, occupied-slot preservation and reuse, anisotropic extent
ownership, quality changes, reset generations, in-flight polling,
completion plus resubmission, poll-only completion, failure paths, stale
completion rejection, and matching GPU publication constants.
