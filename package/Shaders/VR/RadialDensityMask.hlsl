// Radial Density Mask Generation Shader
// Handles Skyrim VR stereoscopic layout: Left eye [0, 0.5], Right eye [0.5, 1]

RWTexture2D<uint> OutputTexture : register(u0);

cbuffer Params : register(b0)
{
	float2 CenterLeft;      // Center of left eye in pixels
	float2 CenterRight;     // Center of right eye in pixels
	float InnerRadiusSq;    // Squared radius for full quality zone
	float MiddleRadiusSq;   // Squared radius for half quality zone
	float OuterRadiusSq;    // Squared radius for reduced quality zone
	float HalfWidth;        // Half of total texture width (boundary between eyes)
	float HeightScale;      // Scale Y distance (squash vertically)
	float3 Pad;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	float2 pos = float2(DTid.xy);

	// Determine which eye based on x position
	// Left eye: x < HalfWidth, Right eye: x >= HalfWidth
	float2 center = (pos.x < HalfWidth) ? CenterLeft : CenterRight;

	float2 delta = pos - center;

	// Apply elliptical scaling
	delta.y *= HeightScale;

	float distSq = dot(delta, delta);

	uint value = 0; // Zone 0: Inner (1x1)
	if (distSq > OuterRadiusSq) {
		value = 3; // Zone 3: Outer (4x4 / Cull)
	} else if (distSq > MiddleRadiusSq) {
		value = 2; // Zone 2: Middle (2x2)
	} else if (distSq > InnerRadiusSq) {
		value = 1; // Zone 1: Intermediate (1x2)
	}

	OutputTexture[DTid.xy] = value;
}
