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
#if defined(VR)
	float4 InverseRow0R;
	float4 InverseRow1R;
	float4 InverseRow2R;
	float4 TranslationR;
	float4 DepthColumnR;
	float4 WindowOffsetR;
	float4 EyePositionR;
#endif
};

#if defined(VR)
cbuffer VRValues : register(b13)
{
	float AlphaTestRefRS : packoffset(c0);
	float StereoEnabled : packoffset(c0.y);
	float2 EyeOffsetScale : packoffset(c0.z);
	float4 EyeClipEdge[2] : packoffset(c1);
}
#	define EYE_VALUE(name, eye) ((eye) ? name##R : name)
#else
#	define EYE_VALUE(name, eye) (name)
#endif

Texture2D<float> Field : register(t0);
Texture2D<float> Activity : register(t1);
Texture2D<float> Blanket : register(t2);
Texture2D<float> Field1 : register(t3);
Texture2D<float> Activity1 : register(t4);
SamplerState FieldSampler : register(s0);

float4 UndoVRRemap(float4 position, uint eye)
{
#if defined(VR)
	precise float scale = (2.0 - StereoEnabled) * 0.5;
	precise float offset = EyeOffsetScale[eye] * StereoEnabled;
	precise float4 clip = float4((position.x - offset * position.w) / scale, position.yzw);
	return clip;
#else
	return position;
#endif
}

float4 ApplyVRRemap(float4 clip, uint eye)
{
#if defined(VR)
	precise float scale = (2.0 - StereoEnabled) * 0.5;
	precise float offset = EyeOffsetScale[eye] * StereoEnabled;
	precise float4 position = float4(clip.x * scale + offset * clip.w, clip.yzw);
	return position;
#else
	return clip;
#endif
}

float3 ToCameraRelative(float4 clipPosition, uint eye)
{
	float4 translation = EYE_VALUE(Translation, eye);
	precise float3 clip = float3(clipPosition.xy, translation.w > 0.5 ? clipPosition.w : clipPosition.z) - translation.xyz;
	precise float3 cameraRelative = float3(dot(EYE_VALUE(InverseRow0, eye).xyz, clip), dot(EYE_VALUE(InverseRow1, eye).xyz, clip), dot(EYE_VALUE(InverseRow2, eye).xyz, clip));
	return cameraRelative;
}

// Vertices outside the frustum make the two eyes' remapped ranges overlap. The hull stage never receives the engine's
// clip distance, so the eye is the one whose decode reproduces the vertex's own camera-relative world position.
float EyeMismatch(float4 position, float3 worldPosition, uint eye)
{
#if defined(VR)
	return length(ToCameraRelative(UndoVRRemap(position, eye), eye) - worldPosition);
#else
	return 0.0;
#endif
}

float2 ToWindow(float3 cameraRelative, uint eye)
{
	precise float2 windowPosition = cameraRelative.xy + EYE_VALUE(WindowOffset, eye).xy;
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

float4 DisplaceClipPosition(float4 clipPosition, float displacement, uint eye)
{
	bool clamped = RangeParams.z > 0.5;
	precise float applied = (clamped && clipPosition.z <= 0.0) ? 0.0 : displacement;
	precise float4 displaced = clipPosition + EYE_VALUE(DepthColumn, eye) * applied;
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

// The driver links tessellation stages by register, so VR lighting keeps the vertex shader's order: clip and cull
// sit at r11 after COLOR1. The hull stage cannot declare them as system values there, so patches carry r11 as TEXCOORD13.
struct ControlPoint
{
#if defined(SD_LAYOUT)
	SD_LAYOUT
#else
	float4 Position: SV_POSITION0;
#	if defined(VR) && defined(DEPTH_ONLY) && defined(SD_STOCK_DEPTH)
	nointerpolation uint EyeIndex: EYEIDX0;
	float4 ClipCull: TEXCOORD13;
#	elif defined(VR) && defined(DEPTH_ONLY)
	float ClipDistance: SV_ClipDistance0;
	float CullDistance: SV_CullDistance0;
	nointerpolation uint EyeIndex: EYEIDX0;
#	endif
#	if !defined(DEPTH_ONLY)
	float4 TexCoord0: TEXCOORD0;
	float3 TBN0: TEXCOORD1;
	float3 TBN1: TEXCOORD2;
	float3 TBN2: TEXCOORD3;
	float4 LandBlendWeights1: TEXCOORD6;
	float4 LandBlendWeights2: TEXCOORD7;
	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
	float4 Color: COLOR0;
	float4 FogParam: COLOR1;
#		if defined(VR)
	float4 ClipCull: TEXCOORD13;
#		endif
	float3 ModelPosition: TEXCOORD12;
#	endif
#endif
};

#if defined(SD_OUT_LAYOUT)
struct DomainOutput
{
	SD_OUT_LAYOUT
};
#elif defined(VR) && !defined(DEPTH_ONLY)
struct DomainOutput
{
	float4 Position: SV_POSITION0;
	float4 TexCoord0: TEXCOORD0;
	float3 TBN0: TEXCOORD1;
	float3 TBN1: TEXCOORD2;
	float3 TBN2: TEXCOORD3;
	float4 LandBlendWeights1: TEXCOORD6;
	float4 LandBlendWeights2: TEXCOORD7;
	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
	float4 Color: COLOR0;
	float4 FogParam: COLOR1;
	float ClipDistance: SV_ClipDistance0;
	float CullDistance: SV_CullDistance0;
	float3 ModelPosition: TEXCOORD12;
};
#elif defined(VR) && defined(DEPTH_ONLY) && defined(SD_STOCK_DEPTH)
struct DomainOutput
{
	float4 Position: SV_POSITION0;
	nointerpolation uint EyeIndex: EYEIDX0;
	float ClipDistance: SV_ClipDistance0;
	float CullDistance: SV_CullDistance0;
};
#else
typedef ControlPoint DomainOutput;
#endif

struct PatchConstants
{
	float Edges[3] : SV_TessFactor;
	float Inside: SV_InsideTessFactor;
#if defined(VR)
	nointerpolation uint Eye: TEXCOORD0;
#endif
};

float ControlPointClip(ControlPoint controlPoint)
{
#if defined(SD_CLIPCULL) || (defined(VR) && !defined(SD_LAYOUT) && (!defined(DEPTH_ONLY) || defined(SD_STOCK_DEPTH)))
	return controlPoint.ClipCull.x;
#elif defined(VR) && defined(DEPTH_ONLY)
	return controlPoint.ClipDistance;
#else
	return 0.0;
#endif
}

float3 ControlPointWorld(ControlPoint controlPoint)
{
#if defined(SD_WORLD_POSITION)
	return controlPoint.SD_WORLD_POSITION.xyz;
#elif !defined(DEPTH_ONLY)
	return controlPoint.WorldPosition.xyz;
#else
	return 0.0;
#endif
}

uint PatchEyeOf(ControlPoint p0, ControlPoint p1, ControlPoint p2)
{
#if defined(VR) && defined(DEPTH_ONLY) && !defined(SD_LAYOUT)
	return p0.EyeIndex;
#elif defined(VR)
	uint eye = 0;
	[branch] if (StereoEnabled >= 0.5)
	{
		float left = EyeMismatch(p0.Position, ControlPointWorld(p0), 0) + EyeMismatch(p1.Position, ControlPointWorld(p1), 0) + EyeMismatch(p2.Position, ControlPointWorld(p2), 0);
		float right = EyeMismatch(p0.Position, ControlPointWorld(p0), 1) + EyeMismatch(p1.Position, ControlPointWorld(p1), 1) + EyeMismatch(p2.Position, ControlPointWorld(p2), 1);
		eye = right < left ? 1 : 0;
	}
	return eye;
#else
	return 0;
#endif
}

uint PatchEye(PatchConstants constants)
{
#if defined(VR)
	return constants.Eye;
#else
	return 0;
#endif
}

float DisplacedClipDistance(float4 displacedClip, float interpolatedDistance, uint eye)
{
#if defined(VR)
	return StereoEnabled > 0.5 ? dot(displacedClip, EyeClipEdge[eye]) : interpolatedDistance;
#else
	return interpolatedDistance;
#endif
}

#endif
