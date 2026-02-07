// Radial Density Mask Generation Shader
// Handles Skyrim VR stereoscopic layout: Left eye [0, 0.5], Right eye [0.5, 1]
// Based on VRPerfKit (fholger/vrperfkit) - MIT License

RWTexture2D<uint> OutputTexture : register(u0);

cbuffer Params : register(b0)
{
	float2 CenterLeft;      // Center of left eye in pixels
	float2 CenterRight;     // Center of right eye in pixels
	float InnerRadiusSq;    // Squared radius for full quality zone
	float MiddleRadiusSq;   // Squared radius for half quality zone
	float OuterRadiusSq;    // Squared radius for reduced quality zone
	float EdgeRadiusSq;     // Squared radius for soft transition zone (reduces white outlines)
	float HalfWidth;        // Half of total texture width (boundary between eyes)
	float HeightScale;      // Scale Y distance (squash vertically)
	float2 Pad;
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

	uint value = 0; // Zone 0: Inner (1x1 - Full Quality)

	// VRPerfKit-style gradual transition zones
	// Values: 0=Full, 1=Half, 2=Quarter, 3=Sixteenth (edge transition), 4=Cull
	// However, R8_UINT can store 0-255, so we use:
	// 0=Keep all, 1=1/2, 2=1/4, 3=1/16 (edge), 4=Cull

	if (distSq > EdgeRadiusSq) {
		// Beyond edge: Full cull
		value = 4;
	} else if (distSq > OuterRadiusSq) {
		// Outer to Edge transition: 1/16 density (finest checkerboard)
		// This is the key to reducing white outlines - gradual density falloff
		value = 3;
	} else if (distSq > MiddleRadiusSq) {
		// Middle ring: 1/4 density (2x2 pattern)
		value = 2;
	} else if (distSq > InnerRadiusSq) {
		// Inner ring: 1/2 density (1x2 pattern)
		value = 1;
	}

	OutputTexture[DTid.xy] = value;
}
