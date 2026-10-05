#include "Common/ReverseZ.hlsli"

RWTexture2D<float> BlendedDepthTexture : register(u0);
RWTexture2D<unorm float> BlendedDepthTexture16 : register(u1);
RWTexture2D<float> MainDepthCopy : register(u2);  // R32_FLOAT snapshot replaces CopyResource(terrainDepth <- mainDepth)

Texture2D<SCENE_DEPTH_FORMAT> MainDepthTexture : register(t0);
Texture2D<SCENE_DEPTH_FORMAT> TerrainDepthTexture : register(t1);

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	float mainDepth = MainDepthTexture[DTid.xy];
	float mixedDepth = FrameBuffer::NearerDepth(mainDepth, TerrainDepthTexture[DTid.xy]);
	BlendedDepthTexture[DTid.xy] = mixedDepth;
	BlendedDepthTexture16[DTid.xy] = mixedDepth;
	MainDepthCopy[DTid.xy] = mainDepth;
}
