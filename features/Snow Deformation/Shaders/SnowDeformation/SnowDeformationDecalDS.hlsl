#include "SnowDeformation/Common.hlsli"

#define SD_INTERPOLATE(member) output.member = patch[0].member * bary.x + patch[1].member * bary.y + patch[2].member * bary.z
#define SD_COPY(member) output.member = patch[0].member

[domain("tri")]
ControlPoint main(PatchConstants constants, float3 bary : SV_DomainLocation, const OutputPatch<ControlPoint, 3> patch)
{
	ControlPoint output;

	precise float4 clipPosition = patch[0].Position * bary.x + patch[1].Position * bary.y + patch[2].Position * bary.z;
	precise float2 windowPosition = ToWindow(ToCameraRelative(clipPosition));
	precise float displacement = SnowSurface(BlanketAt(windowPosition), SampleField(windowPosition));
	output.Position = DisplaceClipPosition(clipPosition, displacement);

	SD_FIELDS
	SD_WORLD

	return output;
}
