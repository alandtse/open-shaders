// VR hidden-area mesh vertex shader. The engine's own copy passes the mesh through at clip depth 0,
// the far plane under reverse-Z, so the mask is pinned to the near plane instead.

#include "Common/ReverseZ.hlsli"

float4 main(float3 position : POSITION) : SV_Position
{
	return float4(position.xy, FrameBuffer::NearPlaneDepth(), 1.0);
}
