#ifndef NEURAL_RENDERING_RECONSTRUCTION_HLSLI
#define NEURAL_RENDERING_RECONSTRUCTION_HLSLI

#include "NeuralRenderingFinite.hlsli"

namespace NeuralRenderingReconstruction
{
	static const float kGuidanceStrength = 16.0;
	static const float kMinimumNormalization = 1e-6;

	float Lanczos3Weight(float distance)
	{
		const float x = abs(distance);
		if (x < 1e-5)
			return 1.0;
		if (x >= 3.0)
			return 0.0;
		const float angle = 3.14159265358979323846 * x;
		return (sin(angle) / angle) * (sin(angle / 3.0) / (angle / 3.0));
	}

	float Similarity(float3 reference, float3 proxy, float3 luma)
	{
		const float3 difference = reference - proxy;
		return rcp(1.0 + kGuidanceStrength * dot(difference * difference, luma));
	}

	bool RgbWeights(float3 reference, float3 p00, float3 p10, float3 p01, float3 p11,
		float4 spatial, float3 luma, out float4 weights)
	{
		weights = spatial;
		if (!all(NeuralRenderingFinite::IsFinite(reference)) || !all(NeuralRenderingFinite::IsFinite(p00)) || !all(NeuralRenderingFinite::IsFinite(p10)) ||
			!all(NeuralRenderingFinite::IsFinite(p01)) || !all(NeuralRenderingFinite::IsFinite(p11)))
			return false;
		const float3 magnitude = max(abs(reference), max(max(abs(p00), abs(p10)), max(abs(p01), abs(p11))));
		const float scale = max(magnitude.x, max(magnitude.y, magnitude.z));
		if (scale <= kMinimumNormalization)
			return true;
		const float3 normalized = reference / scale;
		const float4 candidate = spatial * float4(Similarity(normalized, p00 / scale, luma),
											   Similarity(normalized, p10 / scale, luma), Similarity(normalized, p01 / scale, luma), Similarity(normalized, p11 / scale, luma));
		const float normalization = dot(candidate, 1.0);
		if (!all(NeuralRenderingFinite::IsFinite(candidate)) || any(candidate < 0.0) || !NeuralRenderingFinite::IsFinite(normalization) || normalization <= kMinimumNormalization)
			return false;
		weights = candidate / normalization;
		return true;
	}
}

#endif
