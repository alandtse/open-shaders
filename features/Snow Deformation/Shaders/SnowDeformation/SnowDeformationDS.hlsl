#include "SnowDeformation/Common.hlsli"

#define SD_INTERPOLATE(member) output.member = patch[0].member * bary.x + patch[1].member * bary.y + patch[2].member * bary.z

[domain("tri")]
ControlPoint main(PatchConstants constants, float3 bary : SV_DomainLocation, const OutputPatch<ControlPoint, 3> patch)
{
	ControlPoint output;

	precise float4 clipPosition = patch[0].Position * bary.x + patch[1].Position * bary.y + patch[2].Position * bary.z;
	precise float2 windowPosition = ToWindow(ToCameraRelative(clipPosition));
	precise float raise = BlanketAt(windowPosition);
	precise float field = SampleField(windowPosition);
	precise float displacement = SnowSurface(raise, field);
	precise float4 displacedPosition = DisplaceClipPosition(clipPosition, displacement);
	output.Position = displacedPosition;

#if !defined(DEPTH_ONLY)
	SD_INTERPOLATE(TexCoord0);
	SD_INTERPOLATE(TBN0);
	SD_INTERPOLATE(TBN1);
	SD_INTERPOLATE(TBN2);
	SD_INTERPOLATE(LandBlendWeights1);
	SD_INTERPOLATE(LandBlendWeights2);
	SD_INTERPOLATE(WorldPosition);
	SD_INTERPOLATE(PreviousWorldPosition);
	SD_INTERPOLATE(Color);
	SD_INTERPOLATE(FogParam);
	SD_INTERPOLATE(ModelPosition);

	output.WorldPosition.z += displacement;
	output.PreviousWorldPosition.z += displacement;

	float gradientStep = GradientStep(windowPosition);
	float2 gradient = float2(
						  SnowSurface(raise, SampleField(windowPosition + float2(gradientStep, 0.0))) - SnowSurface(raise, SampleField(windowPosition - float2(gradientStep, 0.0))),
						  SnowSurface(raise, SampleField(windowPosition + float2(0.0, gradientStep))) - SnowSurface(raise, SampleField(windowPosition - float2(0.0, gradientStep)))) *
	                  (0.5 / gradientStep);

	float3 normal = float3(output.TBN0.z, output.TBN1.z, output.TBN2.z);
	[branch] if (dot(gradient, gradient) > 1e-8 && normal.z > 0.05 * length(normal))
	{
		float3 tangent = float3(output.TBN0.x, output.TBN1.x, output.TBN2.x);
		float3 bitangent = float3(output.TBN0.y, output.TBN1.y, output.TBN2.y);
		normal = normalize(normal);

		float3 displacedNormal = normalize(float3(normal.xy / normal.z - gradient, 1.0));
		float minZ = min(TessParams.w, normal.z);
		[flatten] if (displacedNormal.z < minZ)
			displacedNormal = float3(displacedNormal.xy * (sqrt(1.0 - minZ * minZ) / max(length(displacedNormal.xy), 1e-6)), minZ);
		float handedness = dot(cross(normal, tangent), bitangent) < 0.0 ? -1.0 : 1.0;
		float3 displacedTangent = tangent - displacedNormal * dot(displacedNormal, tangent);
		displacedTangent = dot(displacedTangent, displacedTangent) > 1e-8 ? normalize(displacedTangent) : normalize(cross(float3(0.0, 1.0, 0.0), displacedNormal));
		float3 displacedBitangent = cross(displacedNormal, displacedTangent) * handedness;

		output.TBN0 = float3(displacedTangent.x, displacedBitangent.x, displacedNormal.x);
		output.TBN1 = float3(displacedTangent.y, displacedBitangent.y, displacedNormal.y);
		output.TBN2 = float3(displacedTangent.z, displacedBitangent.z, displacedNormal.z);
	}

	[branch] if (MaterialLayers.x >= 0 && field < 0.0)
	{
		float4 weights1 = output.LandBlendWeights1;
		float2 weights2 = output.LandBlendWeights2.xy;
		float total = dot(weights1, 1.0) + weights2.x + weights2.y;
		uint mask = (uint)MaterialLayers.y;
		float4 snowSelect1 = float4((mask & 1u) != 0, (mask & 2u) != 0, (mask & 4u) != 0, (mask & 8u) != 0);
		float2 snowSelect2 = float2((mask & 16u) != 0, (mask & 32u) != 0);
		float snowFraction = saturate((dot(weights1, snowSelect1) + dot(weights2, snowSelect2)) / max(total, 1e-4));
		float amount = saturate(-field * MaterialBlend.y) * MaterialBlend.x * snowFraction;
		int reveal = MaterialLayers.x;
		float4 target1 = float4(reveal == 0, reveal == 1, reveal == 2, reveal == 3) * total;
		float2 target2 = float2(reveal == 4, reveal == 5) * total;
		output.LandBlendWeights1 = lerp(weights1, target1, amount);
		output.LandBlendWeights2.xy = lerp(weights2, target2, amount);
	}
#endif

	return output;
}
