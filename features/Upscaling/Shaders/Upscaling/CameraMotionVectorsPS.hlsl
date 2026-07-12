#include "Common/Math.hlsli"
#include "Common/VR.hlsli"

Texture2D<float> DepthTexture : register(t0);

// b1: the slot Util::FullscreenPassScope saves and restores.
cbuffer CameraMotionVectorsCB : register(b1)
{
	// Byte layout and mul() convention match FrameBuffer's cb12 matrices; index 1 unused in flat.
	row_major float4x4 CurViewProjUnjitteredInverse[2];
	row_major float4x4 PrevViewProjUnjittered[2];
};

struct PS_INPUT
{
	float4 Position: SV_POSITION;
	float2 TexCoord: TEXCOORD;
};

static const float2 kNDCToUVVelocityScale = float2(-0.5, 0.5);

// Camera-only reprojection for frames where no geometry pass writes motion vectors
// (the main menu). Only the camera moves there, so depth + the view-proj delta
// fully determine each pixel's motion. Not a substitute for geometry MVs in gameplay.
float2 main(PS_INPUT input) : SV_Target
{
	float depth = DepthTexture.Load(int3(input.Position.xy, 0));
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(input.TexCoord);
	float2 monoUV = Stereo::ConvertFromStereoUV(input.TexCoord, eyeIndex);

	float4 clipPos = float4(2 * monoUV.x - 1, 1 - 2 * monoUV.y, depth, 1);
	// Homogeneous throughout: the intermediate world position needs no divide.
	float4 world = mul(CurViewProjUnjitteredInverse[eyeIndex], clipPos);
	float4 prevClip = mul(PrevViewProjUnjittered[eyeIndex], world);
	if (prevClip.w <= EPSILON_DIVISION)
		return 0;

	float2 prevNDC = prevClip.xy / prevClip.w;
	return kNDCToUVVelocityScale * (clipPos.xy - prevNDC);
}
