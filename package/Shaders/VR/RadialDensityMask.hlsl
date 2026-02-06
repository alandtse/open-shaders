// Radial Density Mask Generation Shader
// Handles Skyrim VR stereoscopic layout: Left eye [0, 0.5], Right eye [0.5, 1]

RWTexture2D<uint> OutputTexture : register(u0);

cbuffer Params : register(b0)
{
	float2 CenterLeft;      // Center of left eye in pixels
	float2 CenterRight;     // Center of right eye in pixels
	float InnerRadiusSq;    // Squared radius for full quality zone
	float OuterRadiusSq;    // Squared radius for reduced quality zone
	float HalfWidth;        // Half of total texture width (boundary between eyes)
	float Pad;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	float2 pos = float2(DTid.xy);

	// Determine which eye based on x position
	// Left eye: x < HalfWidth, Right eye: x >= HalfWidth
	float2 center = (pos.x < HalfWidth) ? CenterLeft : CenterRight;

	float2 delta = pos - center;
	float distSq = dot(delta, delta);

	uint value = 0; // Keep (full quality)
	if (distSq > OuterRadiusSq) {
		value = 1; // Discard (lowest quality)
	} else if (distSq > InnerRadiusSq) {
		// Intermediate zone - checkerboard pattern for half resolution
		if ((DTid.x + DTid.y) % 2 == 0) value = 1;
	}

	OutputTexture[DTid.xy] = value;
}
