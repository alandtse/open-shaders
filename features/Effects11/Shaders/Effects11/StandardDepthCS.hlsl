Texture2D<float> SceneDepth : register(t0);
RWTexture2D<float> StandardDepth : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint width, height;
	StandardDepth.GetDimensions(width, height);
	if (id.x >= width || id.y >= height) {
		return;
	}

	StandardDepth[id.xy] = 1.0 - SceneDepth[id.xy];
}
