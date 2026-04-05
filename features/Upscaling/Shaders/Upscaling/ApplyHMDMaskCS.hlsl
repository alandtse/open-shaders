// Applies a pre-rasterized HMD hidden area mask to upscaler inputs or outputs.
//
// MaskTex (t0): R8_UNORM texture, 1=hidden, 0=visible. Rasterized from the OpenVR
//   GetHiddenAreaMesh() at the target resolution so the boundary is resolution-exact.
//
// ColorInOut (u0): zeroed where exactly masked — prevents the upscaler from temporally
//   accumulating sky/ambient color at the hidden area boundary.
//
// When UPDATE_REACTIVE is defined (pre-upscale pass only):
//   ReactiveOut (u1): set to 1.0 where masked OR within DILATION_RADIUS pixels of the
//     mask boundary. The dilation covers the temporal reconstruction neighborhood so
//     DLSS/FSR does not blend historical sky-blue into the boundary ring during fast
//     head movement. Color is only zeroed for exactly-masked pixels; boundary pixels
//     keep their actual scene color but discard temporal history.
//   TransparencyOut (u2): set to 0.0 where masked (exact, not dilated).

Texture2D<float> MaskTex : register(t0);

RWTexture2D<float4> ColorInOut : register(u0);
#if defined(UPDATE_REACTIVE)
RWTexture2D<float> ReactiveOut : register(u1);
RWTexture2D<float> TransparencyOut : register(u2);
#endif

// Dilation radius for reactive mask: covers DLSS/FSR temporal influence at boundary.
// Set to ~half the upscaler's search radius; 8 pixels is conservative but safe.
static const int DILATION_RADIUS = 8;

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint w, h;
	ColorInOut.GetDimensions(w, h);
	if (id.x >= w || id.y >= h)
		return;

	bool exactMasked = MaskTex[id.xy] > 0.5;

	if (exactMasked) {
		ColorInOut[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
	}

#if defined(UPDATE_REACTIVE)
	// Dilate mask for reactive marking: check if any neighbor within radius is masked.
	bool dilatedMasked = exactMasked;
	[loop] for (int dy = -DILATION_RADIUS; dy <= DILATION_RADIUS && !dilatedMasked; ++dy)
	{
		[loop] for (int dx = -DILATION_RADIUS; dx <= DILATION_RADIUS && !dilatedMasked; ++dx)
		{
			int2 sampleCoord = int2(id.xy) + int2(dx, dy);
			if (sampleCoord.x >= 0 && sampleCoord.y >= 0 &&
				sampleCoord.x < (int)w && sampleCoord.y < (int)h) {
				if (MaskTex[sampleCoord] > 0.5)
					dilatedMasked = true;
			}
		}
	}

	if (dilatedMasked) {
		ReactiveOut[id.xy] = 1.0;
		TransparencyOut[id.xy] = 0.0;
	}
#endif
}
