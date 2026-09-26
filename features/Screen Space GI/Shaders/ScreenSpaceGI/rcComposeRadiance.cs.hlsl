#include "ScreenSpaceGI/common.hlsli"

// Compose base RR radiance and reconstructed NRC radiance before RR dispatch.
// t0 = existing raw RR specular signal.
// t1 = NRC prediction grid at the active quality-preset resolution; RGB = prediction, A = validity.
// u0 = combined signal consumed by AMD Ray Regeneration.

Texture2D<float4> srcRRSignal : register(t0);
Texture2D<float4> srcRCRadiance : register(t1);
RWTexture2D<float4> outCombined : register(u0);

static const uint RC_BACKING_GRID_SIZE = 64u;

// Zero disables NRC contribution while preserving the selected output mode.
[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	uint2 frameDim = max(uint2(1u, 1u), uint2(OUT_FRAME_DIM));

	if (dtid.x >= frameDim.x || dtid.y >= frameDim.y)
		return;

	float4 baseSignal = srcRRSignal.Load(uint3(dtid.xy, 0));

	// Map the current pixel into the same normalized screen domain used
	// by the deterministic NRC capture grid.
	float2 uv = (float2(dtid.xy) + 0.5) / float2(frameDim);

	// The capture locations are the centers of the active grid cells:
	// (cell + 0.5) / 8. Reconstruct the sparse field manually so invalid
	// cells do not contribute radiance to neighboring pixels.
	uint rcGridSize = clamp(RadianceCacheResultGridSize, 1u, RC_BACKING_GRID_SIZE);
	float2 gridPos = uv * float(rcGridSize) - 0.5;
	int2 cell0 = int2(floor(gridPos));
	float2 f = frac(gridPos);

	int2 rcMaxCell = int2(rcGridSize - 1u, rcGridSize - 1u);
	int2 c00 = clamp(cell0, int2(0, 0), rcMaxCell);
	int2 c10 = clamp(cell0 + int2(1, 0), int2(0, 0), rcMaxCell);
	int2 c01 = clamp(cell0 + int2(0, 1), int2(0, 0), rcMaxCell);
	int2 c11 = clamp(cell0 + int2(1, 1), int2(0, 0), rcMaxCell);

	float4 r00 = srcRCRadiance.Load(int3(c00, 0));
	float4 r10 = srcRCRadiance.Load(int3(c10, 0));
	float4 r01 = srcRCRadiance.Load(int3(c01, 0));
	float4 r11 = srcRCRadiance.Load(int3(c11, 0));

	float w00 = (1.0 - f.x) * (1.0 - f.y) * saturate(r00.a);
	float w10 = f.x * (1.0 - f.y) * saturate(r10.a);
	float w01 = (1.0 - f.x) * f.y * saturate(r01.a);
	float w11 = f.x * f.y * saturate(r11.a);

	float weightSum = w00 + w10 + w01 + w11;

	float3 rcRadiance = 0.0;
	float rcValidity = 0.0;

	if (weightSum > 1e-6) {
		rcRadiance =
			(r00.rgb * w00 +
				r10.rgb * w10 +
				r01.rgb * w01 +
				r11.rgb * w11) /
			weightSum;

		rcValidity = saturate(weightSum);
	}
	float3 outputRadiance = baseSignal.rgb;

	if (RadianceCacheOutputMode == 1u) {
		// NRC ONLY: show the reconstructed sparse prediction field.
		outputRadiance = rcRadiance * rcValidity;
	} else if (RadianceCacheOutputMode == 2u) {
		// COMBINED: supplement the existing noisy RR observation with
		// reconstructed NRC radiance before Ray Regeneration.
		outputRadiance =
			baseSignal.rgb +
			rcRadiance * rcValidity * RadianceCacheContribution;
	}

	// NRC does not fabricate RR hit distance.
	// Preserve the original SSGI representative view-space hit distance.
	outCombined[dtid.xy] = float4(outputRadiance, baseSignal.a);
}
