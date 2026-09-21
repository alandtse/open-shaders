Texture2D<float4> RRDenoised : register(t0);
RWTexture2D<float4> GiSpecular : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	uint w, h;
	RRDenoised.GetDimensions(w, h);
	if (dtid.x >= w || dtid.y >= h)
		return;
	float4 oldValue = GiSpecular[dtid.xy];
	float4 rr = RRDenoised.Load(int3(dtid.xy, 0));
	// RR alpha is hit distance; Open Shaders alpha is visibility/occlusion.
	GiSpecular[dtid.xy] = float4(rr.rgb, oldValue.a);
}
