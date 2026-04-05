// Applies a pre-rasterized HMD hidden area mask to upscaler inputs or outputs.
//
// MaskTex (t0): R8_UNORM texture, 1=hidden, 0=visible. Rasterized from the OpenVR
//   GetHiddenAreaMesh() at the target resolution so the boundary is resolution-exact.
//
// ColorInOut (u0): zeroed where exactly masked — prevents the upscaler from temporally
//   accumulating sky/ambient color at the hidden area boundary.
//
// When UPDATE_REACTIVE is defined (pre-upscale pass only):
//   ReactiveOut (u1): set to 1.0 where masked OR within dilationRadius pixels of the
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

// Velocity-adaptive dilation radius set by the CPU each frame from HMD angular velocity.
// Covers the expected boundary shift per frame: ceil(angVel_rad_per_frame * px_per_rad).
// Separable (horizontal then vertical) 1D max-filter: O(r) per pixel instead of O(r^2).
//
// velocityFactor [0,1]: global reactive weight applied to ALL visible pixels.
// At 0 (stationary) only the dilated boundary ring is reactive.  At 1 (fast rotation)
// every visible pixel gets reactive=1, forcing the upscaler to trust current-frame data
// and preventing sky-history ghosting during rapid head movements.
cbuffer DilationCB : register(b0)
{
	uint dilationRadius;
	float velocityFactor;
	uint2 pad;
};
#endif

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
	// Separable 1D dilation: scan horizontal row then vertical column.
	// Equivalent to a rhombus-shaped dilation (L1 distance), cheaper than square for large r.
	int dr = (int)dilationRadius;
	bool dilatedMasked = exactMasked;
	if (!dilatedMasked) {
		[loop] for (int dx = -dr; dx <= dr && !dilatedMasked; ++dx)
		{
			int sx = clamp((int)id.x + dx, 0, (int)w - 1);
			if (MaskTex[int2(sx, id.y)] > 0.5)
				dilatedMasked = true;
		}
	}
	if (!dilatedMasked) {
		[loop] for (int dy = -dr; dy <= dr && !dilatedMasked; ++dy)
		{
			int sy = clamp((int)id.y + dy, 0, (int)h - 1);
			if (MaskTex[int2(id.x, sy)] > 0.5)
				dilatedMasked = true;
		}
	}

	if (dilatedMasked) {
		ReactiveOut[id.xy] = 1.0;
		if (exactMasked)
			TransparencyOut[id.xy] = 0.0;
	} else {
		// Velocity-proportional reactive weight for pixels far from the boundary.
		// During rapid head rotation, the upscaler accumulates sky-tinted history at pixels
		// that previously faced the sky.  Scaling reactive weight with angular velocity
		// forces the upscaler to discard stale history at interior pixels too, eliminating
		// sky ghosting without sacrificing temporal smoothness when the head is still.
		ReactiveOut[id.xy] = velocityFactor;
	}
#endif
}
