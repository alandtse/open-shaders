#include "ABCompositorStats.h"

bool CompositorStatsAccumulator::Add(ABVariant variant, const CompositorSample& sample)
{
	if (!(sample.totalRenderGpuMs > 0.0f) || sample.totalRenderGpuMs > kMaxOutlierFrameTime) {
		++rejectedFrames;
		return false;
	}

	auto& entry = sums[Index(variant)];
	++entry.frames;
	entry.droppedFrames += sample.droppedFrames;
	entry.reprojectedFrames += sample.reprojected ? 1u : 0u;
	entry.applicationGpuMs += sample.applicationGpuMs;
	entry.totalRenderGpuMs += sample.totalRenderGpuMs;
	entry.compositorRenderGpuMs += sample.compositorRenderGpuMs;
	entry.compositorRenderCpuMs += sample.compositorRenderCpuMs;
	entry.clientFrameIntervalMs += sample.clientFrameIntervalMs;
	return true;
}

CompositorVariantSummary CompositorStatsAccumulator::Summarize(ABVariant variant) const
{
	const auto& entry = sums[Index(variant)];
	CompositorVariantSummary summary;
	summary.frames = entry.frames;
	summary.droppedFrames = entry.droppedFrames;
	summary.reprojectedFrames = entry.reprojectedFrames;
	if (entry.frames == 0)
		return summary;

	const double frames = entry.frames;
	summary.meanApplicationGpuMs = static_cast<float>(entry.applicationGpuMs / frames);
	summary.meanTotalRenderGpuMs = static_cast<float>(entry.totalRenderGpuMs / frames);
	summary.meanCompositorRenderGpuMs = static_cast<float>(entry.compositorRenderGpuMs / frames);
	summary.meanCompositorRenderCpuMs = static_cast<float>(entry.compositorRenderCpuMs / frames);
	summary.meanClientFrameIntervalMs = static_cast<float>(entry.clientFrameIntervalMs / frames);
	return summary;
}

float CompositorStatsAccumulator::DeltaApplicationGpuMs() const
{
	const auto a = Summarize(ABVariant::A);
	const auto b = Summarize(ABVariant::B);
	if (a.frames == 0 || b.frames == 0)
		return 0.0f;
	return b.meanApplicationGpuMs - a.meanApplicationGpuMs;
}

float CompositorStatsAccumulator::DeltaTotalRenderGpuMs() const
{
	const auto a = Summarize(ABVariant::A);
	const auto b = Summarize(ABVariant::B);
	if (a.frames == 0 || b.frames == 0)
		return 0.0f;
	return b.meanTotalRenderGpuMs - a.meanTotalRenderGpuMs;
}

void CompositorStatsAccumulator::Reset()
{
	sums = {};
	rejectedFrames = 0;
}
