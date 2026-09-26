#include "Common/Color.hlsli"
#include "Common/SceneExposure.hlsli"
#include "Upscaling/NeuralRendering/ColorContract.hlsli"

// Local GPU exposure estimate for the neural-rendering proxy. It runs once per frame over both
// eyes so the left and right eye share one exposure, and it measures the geometric-mean luminance
// in the same linear domain Prepare multiplies it into. Two dispatches: Reduce sums log2
// luminance per group, Adapt folds the groups into one adapted luminance and the exposure.
cbuffer NRExposure : register(b0)
{
	uint SourceWidth;  // full stereo source extent, both eyes
	uint SourceHeight;
	uint GroupColumns;
	uint GroupRows;
	uint ResetAdaptation;  // 1 = snap to the measured target instead of adapting from history
	float MinLuminance;
	float MaxLuminance;
	float pad0;
	float DeltaTime;
	float Tau;
	float pad1;
	float pad2;
};

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float> ExposurePartials : register(u0);
// [0] adapted luminance of the previous frame, [1] the exposure Prepare multiplies by.
RWStructuredBuffer<float> ExposureState : register(u1);

groupshared float s_partial[64];

[numthreads(8, 8, 1)] void Reduce(uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID) {
	const uint2 tile = uint2((SourceWidth + GroupColumns - 1) / GroupColumns, (SourceHeight + GroupRows - 1) / GroupRows);
	const uint2 origin = groupID.xy * tile;
	float sum = 0.0;
	for (uint y = threadID.y; y < tile.y; y += 8) {
		for (uint x = threadID.x; x < tile.x; x += 8) {
			const uint2 pixel = origin + uint2(x, y);
			if (pixel.x >= SourceWidth || pixel.y >= SourceHeight)
				continue;
			// One non-finite sample would otherwise make the whole reduction non-finite.
			const float logLuminance = NR::ExposureLogLuminance(Source[pixel].rgb);
			if (isfinite(logLuminance))
				sum += logLuminance;
		}
	}
	const uint index = threadID.y * 8 + threadID.x;
	s_partial[index] = sum;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = 32; stride > 0; stride >>= 1) {
		if (index < stride)
			s_partial[index] += s_partial[index + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (index == 0)
		ExposurePartials[groupID.y * GroupColumns + groupID.x] = s_partial[0];
}

	[numthreads(64, 1, 1)] void Adapt(uint3 threadID : SV_GroupThreadID)
{
	const uint index = threadID.x;
	const uint groupCount = GroupColumns * GroupRows;
	float sum = 0.0;
	for (uint i = index; i < groupCount; i += 64)
		sum += ExposurePartials[i];
	s_partial[index] = sum;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = 32; stride > 0; stride >>= 1) {
		if (index < stride)
			s_partial[index] += s_partial[index + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (index != 0)
		return;
	const float pixelCount = max(float(SourceWidth) * float(SourceHeight), 1.0);
	const float meanLogLuminance = s_partial[0] / pixelCount;
	// A floor of -13.3 per sample makes a zero sum mean nothing accumulated at all: a source with
	// no finite texel leaves the exposure at 1 rather than extrapolating from a mean that is not a
	// luminance.
	if (!isfinite(meanLogLuminance) || s_partial[0] == 0.0) {
		ExposureState[1] = 1.0;
		return;
	}
	const float target = exp2(meanLogLuminance);
	const float adapted = ResetAdaptation != 0 ?
	                          target :
	                          lerp(ExposureState[0], target, 1.0 - exp(-DeltaTime / max(Tau, 1e-4)));
	ExposureState[0] = adapted;
	ExposureState[1] = SceneExposure::Evaluate(adapted, float2(MinLuminance, MaxLuminance), 1.0);
}
