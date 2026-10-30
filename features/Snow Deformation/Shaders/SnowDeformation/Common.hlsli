#ifndef __SNOW_DEFORMATION_COMMON_HLSLI__
#define __SNOW_DEFORMATION_COMMON_HLSLI__

cbuffer DrawParams : register(b0)
{
	float4 InverseRow0;
	float4 InverseRow1;
	float4 InverseRow2;
	float4 Translation;
	float4 DepthColumn;
	float4 WindowOffset;
	float4 EyePosition;
	float4 FieldParams;
	float4 FadeParams;
	float4 TessParams;
	float4 RangeParams;
	int4 SlotParams;
	float4 BlanketParams;
	float4 WindowOffset1;
	float4 FieldParams1;
	float4 FadeParams1;
	int4 SlotParams1;
	float4 BlanketWindow;
	float4 DetailParams;
};

Texture2D<float> Field : register(t0);
Texture2D<float> Activity : register(t1);
Texture2D<float> Blanket : register(t2);
Texture2D<float> Field1 : register(t3);
Texture2D<float> Activity1 : register(t4);
SamplerState FieldSampler : register(s0);

float3 ToCameraRelative(float4 clipPosition)
{
	precise float3 clip = float3(clipPosition.xy, Translation.w > 0.5 ? clipPosition.w : clipPosition.z) - Translation.xyz;
	precise float3 cameraRelative = float3(dot(InverseRow0.xyz, clip), dot(InverseRow1.xyz, clip), dot(InverseRow2.xyz, clip));
	return cameraRelative;
}

float2 ToWindow(float3 cameraRelative)
{
	precise float2 windowPosition = cameraRelative.xy + WindowOffset.xy;
	return windowPosition;
}

float WindowFade(float2 windowPosition)
{
	float2 fromCentre = abs(windowPosition - FadeParams.x);
	return 1.0 - saturate((max(fromCentre.x, fromCentre.y) - FadeParams.y) * FadeParams.z);
}

float WindowFade1(float2 windowPosition1)
{
	float2 fromCentre = abs(windowPosition1 - FadeParams1.x);
	return FadeParams1.w > 0.5 ? 1.0 - saturate((max(fromCentre.x, fromCentre.y) - FadeParams1.y) * FadeParams1.z) : 0.0;
}

float SampleField(float2 windowPosition)
{
	float fade0 = WindowFade(windowPosition);
	precise float2 windowPosition1 = windowPosition + WindowOffset1.xy;
	float fade1 = WindowFade1(windowPosition1);
	float fine = 0.0;
	float coarse = 0.0;
	[branch] if (fade0 > 0.0)
	{
		precise float2 texel = windowPosition * FieldParams.x + FieldParams.yz + 0.5;
		fine = Field.SampleLevel(FieldSampler, texel * FieldParams.w, 0);
	}
	[branch] if (fade1 > 0.0 && fade0 < 1.0)
	{
		precise float2 texel = windowPosition1 * FieldParams1.x + FieldParams1.yz + 0.5;
		coarse = Field1.SampleLevel(FieldSampler, texel * FieldParams.w, 0) * fade1;
	}
	return lerp(coarse, fine, fade0);
}

float GradientStep(float2 windowPosition)
{
	return lerp(FieldParams1.w, FadeParams.w, WindowFade(windowPosition));
}

float BlanketAt(float2 windowPosition)
{
	float raise = 0.0;
	precise float2 inside = windowPosition - BlanketWindow.xy;
	[branch] if (BlanketParams.z > 0.5 && all(inside >= 0.0) && all(inside < 8128.0))
	{
		precise float2 texel = windowPosition * (1.0 / 64.0) + BlanketParams.xy + 0.5;
		raise = Blanket.SampleLevel(FieldSampler, texel * (1.0 / 128.0), 0);
	}
	return raise;
}

float SnowSurface(float raise, float field)
{
	precise float surface = raise + field;
	return BlanketParams.z > 0.5 ? max(surface, 0.0) : surface;
}

float4 DisplaceClipPosition(float4 clipPosition, float displacement)
{
	bool clamped = RangeParams.z > 0.5;
	precise float applied = (clamped && clipPosition.z <= 0.0) ? 0.0 : displacement;
	precise float4 displaced = clipPosition + DepthColumn * applied;
	displaced.z = clamped ? max(displaced.z, 0.0) : displaced.z;
	return displaced;
}

float ActivityAt(float2 windowPosition)
{
	float activity = 0.0;
	precise float2 scaled = windowPosition * WindowOffset.w;
	int2 tile = (int2)floor(scaled);
	[branch] if (all(abs(tile) <= SlotParams.z))
		activity = Activity.Load(int3((tile + SlotParams.xy) & SlotParams.w, 0));
	[branch] if (FadeParams1.w > 0.5)
	{
		precise float2 scaled1 = (windowPosition + WindowOffset1.xy) * WindowOffset1.w;
		int2 tile1 = (int2)floor(scaled1);
		[branch] if (all(abs(tile1) <= SlotParams1.z))
			activity = max(activity, Activity1.Load(int3((tile1 + SlotParams1.xy) & SlotParams1.w, 0)));
	}
	return activity;
}

struct ControlPoint
{
#if defined(SD_LAYOUT)
	SD_LAYOUT
#else
	float4 Position : SV_POSITION0;
#	if !defined(DEPTH_ONLY)
	float4 TexCoord0 : TEXCOORD0;
	float3 TBN0 : TEXCOORD1;
	float3 TBN1 : TEXCOORD2;
	float3 TBN2 : TEXCOORD3;
	float4 LandBlendWeights1 : TEXCOORD6;
	float4 LandBlendWeights2 : TEXCOORD7;
	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
	float4 Color : COLOR0;
	float4 FogParam : COLOR1;
	float3 ModelPosition : TEXCOORD12;
#	endif
#endif
};

struct PatchConstants
{
	float Edges[3] : SV_TessFactor;
	float Inside : SV_InsideTessFactor;
};

#endif
