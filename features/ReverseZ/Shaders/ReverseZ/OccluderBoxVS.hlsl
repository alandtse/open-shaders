// Occlusion-culling box vertex shader for the VR engine. The engine's own copy hardcodes a
// standard-depth near value for boxes reaching behind the camera, which fails under reverse-Z.

#include "Common/ReverseZ.hlsli"
#include "Common/VR.hlsli"

struct Instance
{
	row_major float4x4 Transform;
};

StructuredBuffer<Instance> Instances : register(t0);

cbuffer OcclusionCameraCB : register(b0)
{
	row_major float4x4 EyeViewProj[2] : packoffset(c0);
	float4 EyePosAdjust[2] : packoffset(c8);
};

static const float ClipEdgeLimit = 0.999;

struct VS_INPUT
{
	float4 Position: POSITION;
	uint VertexID: SV_VertexID;
	uint InstanceID: SV_InstanceID;
};

struct VS_OUTPUT
{
	float4 Position: SV_Position;
	float ClipDistance: SV_ClipDistance0;
	float CullDistance: SV_CullDistance0;
	nointerpolation uint Instance: TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT output;

	bool stereo = StereoEnabled > 0.0;
	uint eye = stereo ? (input.InstanceID & 1) : 0;
	uint instanceIndex = stereo ? (input.InstanceID >> 1) : input.InstanceID;

	float4 corner = float4(
		(input.VertexID & 4) ? -1.0 : 1.0,
		(input.VertexID & 2) ? -1.0 : 1.0,
		(input.VertexID & 1) ? -1.0 : 1.0,
		1.0);

	row_major float4x4 transform = Instances[instanceIndex].Transform;
	transform._m03_m13_m23 -= EyePosAdjust[eye].xyz;
	float4 clip = mul(EyeViewProj[eye], mul(transform, corner));

	// A vertex behind the camera cannot be projected: pin it to the near plane so the box stays testable.
	if (clip.w < 0.0) {
		clip.xy = clamp(clip.xy, -ClipEdgeLimit, ClipEdgeLimit);
		clip.zw = float2(FrameBuffer::NearPlaneDepth(), 1.0);
	}

	float eyeClip = stereo ? dot(clip, EyeClipEdge[eye]) : 1.0;

	output.Position.x = clip.x * (2.0 - StereoEnabled) * 0.5 + clip.w * EyeOffsetScale[eye] * StereoEnabled;
	output.Position.yzw = clip.yzw;
	output.ClipDistance = eyeClip;
	output.CullDistance = eyeClip;
	output.Instance = instanceIndex;
	return output;
}
