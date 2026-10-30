#pragma once
#include "ABTestAggregator.h"

#include <array>
#include <cstdint>

/** @brief One SteamVR compositor frame, reduced to the fields the A/B test compares. */
struct CompositorSample
{
	float applicationGpuMs = 0.0f;       ///< Application scene GPU time (m_flPreSubmitGpuMs + m_flPostSubmitGpuMs).
	float totalRenderGpuMs = 0.0f;       ///< Post-present to the end of compositor work (m_flTotalRenderGpuMs); includes compositor GPU time.
	float compositorRenderGpuMs = 0.0f;  ///< Compositor distortion, overlay and chaperone GPU time.
	float compositorRenderCpuMs = 0.0f;  ///< CPU time the compositor spent submitting that work.
	float clientFrameIntervalMs = 0.0f;  ///< Time between WaitGetPoses calls.
	uint32_t droppedFrames = 0;          ///< Extra scan-outs of the previous frame.
	bool reprojected = false;            ///< The compositor reprojected the frame because the app missed it (CPU or GPU reason).
};

/** @brief Mean compositor timing for one A/B variant. */
struct CompositorVariantSummary
{
	uint32_t frames = 0;
	uint32_t droppedFrames = 0;
	uint32_t reprojectedFrames = 0;
	float meanApplicationGpuMs = 0.0f;
	float meanTotalRenderGpuMs = 0.0f;
	float meanCompositorRenderGpuMs = 0.0f;
	float meanCompositorRenderCpuMs = 0.0f;
	float meanClientFrameIntervalMs = 0.0f;
};

/**
 * @brief Accumulates SteamVR compositor frame timing separately for A/B variants A and B.
 *
 * Samples that cannot be real frame costs (non-positive or above kMaxOutlierFrameTime) are
 * counted as rejected and left out of the means, as the draw-call aggregator does.
 */
class CompositorStatsAccumulator
{
public:
	/** @brief Adds one sample to a variant; returns false when it was rejected as an outlier. */
	bool Add(ABVariant variant, const CompositorSample& sample);

	/** @brief Returns the mean timing recorded for a variant. */
	CompositorVariantSummary Summarize(ABVariant variant) const;

	/** @brief Samples rejected since the last Reset. */
	uint32_t RejectedFrames() const { return rejectedFrames; }

	/** @brief Variant B minus variant A mean application GPU time; 0 until both have samples. */
	float DeltaApplicationGpuMs() const;

	/** @brief Variant B minus variant A mean total render GPU time; 0 until both have samples. */
	float DeltaTotalRenderGpuMs() const;

	/** @brief Discards every sample. */
	void Reset();

private:
	struct Sums
	{
		uint32_t frames = 0;
		uint32_t droppedFrames = 0;
		uint32_t reprojectedFrames = 0;
		double applicationGpuMs = 0.0;
		double totalRenderGpuMs = 0.0;
		double compositorRenderGpuMs = 0.0;
		double compositorRenderCpuMs = 0.0;
		double clientFrameIntervalMs = 0.0;
	};

	static size_t Index(ABVariant variant) { return variant == ABVariant::A ? 0 : 1; }

	std::array<Sums, 2> sums{};
	uint32_t rejectedFrames = 0;
};
