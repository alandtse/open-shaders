#ifndef __NEURAL_RENDERING_REUSE_DEPENDENCY_HLSL__
#define __NEURAL_RENDERING_REUSE_DEPENDENCY_HLSL__

namespace NeuralRenderingReuse
{
	/** @brief Largest relative depth difference a reprojected gain tolerates before the surface under it counts as changed. */
	static const float kReuseDepthTolerance = 0.05f;
	/** @brief Largest absolute log2 input-luminance difference a reprojected gain tolerates; beyond it the lighting changed. */
	static const float kReuseLumaTolerance = 0.5f;
	/** @brief Denominator floor of the relative depth test, so two depths at the same place still compare finitely. */
	static const float kDepthEpsilon = 1e-6f;

	/**
	 * @brief The guide's raw depth as a linear view-space distance, which is the domain the engine's own
	 *        depth comparisons use.
	 *        Mirrors SharedData::GetScreenDepth's two projection branches, which the standalone NR shaders
	 *        cannot call: they compile without the REVERSE_Z define, so the convention arrives as a runtime
	 *        value, the same one the pass hands Feature 18 as DLSSNR.DepthInverted.
	 * @param cameraData Projection terms SharedData carries: x far, y near, z far - near, w far * near.
	 */
	float LinearDepth(float depth, float4 cameraData, bool depthReversed)
	{
		return depthReversed ? cameraData.w / (depth * cameraData.z + cameraData.y) :
		                       cameraData.w / (-depth * cameraData.z + cameraData.x);
	}

	/** @brief True when two guide depths are close enough in relative terms to be the same surface. */
	bool DepthAgrees(float currentDepth, float historyDepth, float4 cameraData, bool depthReversed)
	{
		const float current = LinearDepth(currentDepth, cameraData, depthReversed);
		const float history = LinearDepth(historyDepth, cameraData, depthReversed);
		if (!isfinite(current) || !isfinite(history))
			return false;
		const float scale = max(max(abs(current), abs(history)), kDepthEpsilon);
		return abs(current - history) <= kReuseDepthTolerance * scale;
	}

	/**
	 * @brief True when the current input luminance is close enough in log2 to the stored one; a flash or a
	 *        lighting change fails it. A non-finite value on either side fails the comparison too.
	 */
	bool LumaAgrees(float currentLogLuma, float historyLogLuma)
	{
		return abs(currentLogLuma - historyLogLuma) <= kReuseLumaTolerance;
	}

	/**
	 * @brief Whether a reprojected gain may be reused at all: the sample must be on screen and over the same
	 *        surface and lighting it was stored for. A rejected sample reuses no gain rather than a stale one.
	 */
	bool SampleAgrees(bool inBounds, float currentDepth, float historyDepth, float4 cameraData, bool depthReversed, float currentLogLuma, float historyLogLuma)
	{
		return inBounds && DepthAgrees(currentDepth, historyDepth, cameraData, depthReversed) && LumaAgrees(currentLogLuma, historyLogLuma);
	}

	/**
	 * @brief The reused proxy in linear light: the current proxy scaled by the stored log gain, so a consumer
	 *        taking the ratio of the two luminances sees exactly exp2(toneDelta).
	 */
	float3 ApplyGainLinear(float3 currentLinear, float toneDelta)
	{
		return currentLinear * exp2(toneDelta);
	}
}

#endif  // __NEURAL_RENDERING_REUSE_DEPENDENCY_HLSL__
