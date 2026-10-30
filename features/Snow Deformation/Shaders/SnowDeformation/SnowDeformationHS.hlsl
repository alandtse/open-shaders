#include "SnowDeformation/Common.hlsli"

float EdgeFactor(float3 a, float3 b)
{
	[flatten] if (a.x > b.x || (a.x == b.x && a.y > b.y))
	{
		float3 swap = a;
		a = b;
		b = swap;
	}

	precise float2 windowA = ToWindow(a);
	precise float2 windowB = ToWindow(b);
	precise float2 windowQuarter = lerp(windowA, windowB, 0.25);
	precise float2 windowHalf = lerp(windowA, windowB, 0.5);
	precise float2 windowThreeQuarter = lerp(windowA, windowB, 0.75);
	float activity = max(max(ActivityAt(windowA), ActivityAt(windowB)),
		max(ActivityAt(windowQuarter), max(ActivityAt(windowHalf), ActivityAt(windowThreeQuarter))));
	float blanket = max(max(BlanketAt(windowA), BlanketAt(windowB)), BlanketAt(windowHalf));

	precise float factor = 1.0;
	[branch] if (activity > 0.0 || blanket > 0.01)
	{
		precise float distance = length((a + b) * 0.5 - EyePosition.xyz);
		precise float range = max(1.0, distance * TessParams.z);
		precise float edge = length(b - a);
		precise float fineFactor = clamp(edge * TessParams.y / range, 1.0, TessParams.x);
		precise float blanketFactor = blanket > 0.01 ? clamp(edge * BlanketParams.w / range, 1.0, TessParams.x) : 1.0;
		factor = lerp(blanketFactor, fineFactor, saturate(activity * RangeParams.w));
	}
	return factor;
}

bool IsPatchCulled(float4 p0, float4 p1, float4 p2)
{
	precise float4 lower = DepthColumn * -RangeParams.x;
	precise float4 upper = DepthColumn * RangeParams.y;
	precise float4 corners[6] = { p0 + lower, p0 + upper, p1 + lower, p1 + upper, p2 + lower, p2 + upper };

	float4 outside = 1.0;
	bool inFront = true;
	[unroll] for (uint i = 0; i < 6; i++)
	{
		float4 corner = corners[i];
		inFront = inFront && corner.w > 0.0;
		outside *= float4(corner.x > corner.w, corner.x < -corner.w, corner.y > corner.w, corner.y < -corner.w);
	}
	return inFront && any(outside);
}

PatchConstants PatchConstantFunction(InputPatch<ControlPoint, 3> patch)
{
	PatchConstants output;
	output.Edges[0] = 0.0;
	output.Edges[1] = 0.0;
	output.Edges[2] = 0.0;
	output.Inside = 0.0;

	[branch] if (!IsPatchCulled(patch[0].Position, patch[1].Position, patch[2].Position))
	{
		float3 p0 = ToCameraRelative(patch[0].Position);
		float3 p1 = ToCameraRelative(patch[1].Position);
		float3 p2 = ToCameraRelative(patch[2].Position);

		output.Edges[0] = EdgeFactor(p1, p2);
		output.Edges[1] = EdgeFactor(p2, p0);
		output.Edges[2] = EdgeFactor(p0, p1);
		output.Inside = max(output.Edges[0], max(output.Edges[1], output.Edges[2]));
	}
	return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstantFunction")]
[maxtessfactor(64.0)]
ControlPoint main(InputPatch<ControlPoint, 3> patch, uint id : SV_OutputControlPointID)
{
	return patch[id];
}
