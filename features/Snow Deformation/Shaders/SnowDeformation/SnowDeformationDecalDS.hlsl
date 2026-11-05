#include "SnowDeformation/Common.hlsli"

#define SD_INTERPOLATE(member) output.member = patch[0].member * bary.x + patch[1].member * bary.y + patch[2].member * bary.z
#define SD_COPY(member) output.member = patch[0].member

[domain("tri")] DomainOutput main(PatchConstants constants, float3 bary : SV_DomainLocation, const OutputPatch<ControlPoint, 3> patch) {
	DomainOutput output;

	uint eye = PatchEye(constants);
	precise float4 clipPosition = UndoVRRemap(patch[0].Position * bary.x + patch[1].Position * bary.y + patch[2].Position * bary.z, eye);
	precise float2 windowPosition = ToWindow(ToCameraRelative(clipPosition, eye), eye);
	precise float displacement = SnowSurface(BlanketAt(windowPosition), SampleField(windowPosition));
	precise float4 displacedPosition = DisplaceClipPosition(clipPosition, displacement, eye);
	output.Position = ApplyVRRemap(displacedPosition, eye);
#if defined(VR)
	output.ClipDistance = output.CullDistance = DisplacedClipDistance(displacedPosition, ControlPointClip(patch[0]) * bary.x + ControlPointClip(patch[1]) * bary.y + ControlPointClip(patch[2]) * bary.z, eye);
#endif

	SD_FIELDS
	SD_WORLD

	return output;
}
