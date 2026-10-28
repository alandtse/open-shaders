#include "ABComparison.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace
{
	double MeanOf(const std::vector<float>& values)
	{
		return values.empty() ? 0.0 : std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
	}

	double VarianceOf(const std::vector<float>& values, double mean)
	{
		if (values.size() < 2)
			return 0.0;
		double sum = 0.0;
		for (float value : values)
			sum += (value - mean) * (value - mean);
		return sum / static_cast<double>(values.size() - 1);
	}

	bool Usable(const ABHeadlineInterval& interval, std::vector<float> ABHeadlineInterval::* series)
	{
		return (interval.*series).size() >= kMinimumHeadlineSamples;
	}

	ABMetric CompareSeries(const std::vector<ABHeadlineInterval>& intervals, std::vector<float> ABHeadlineInterval::* series)
	{
		std::vector<double> pairDeltas;
		double sumA = 0.0, sumB = 0.0, maxDrift = 0.0, frameVariance = 0.0;
		size_t countA = 0, countB = 0;

		for (size_t i = 0; i < intervals.size(); ++i) {
			const auto& interval = intervals[i];
			if (interval.variant != ABVariant::B || !Usable(interval, series))
				continue;

			std::vector<double> baselines;
			std::vector<const ABHeadlineInterval*> neighbours;
			for (const size_t j : { i - 1, i + 1 }) {
				if (j < intervals.size() && intervals[j].variant == ABVariant::A && Usable(intervals[j], series)) {
					baselines.push_back(MeanOf(intervals[j].*series));
					neighbours.push_back(&intervals[j]);
				}
			}
			if (baselines.empty())
				continue;

			const double baseline = std::accumulate(baselines.begin(), baselines.end(), 0.0) / static_cast<double>(baselines.size());
			const double meanB = MeanOf(interval.*series);
			pairDeltas.push_back(meanB - baseline);
			sumB += meanB;
			sumA += baseline;
			++countB;
			++countA;
			if (baselines.size() == 2)
				maxDrift = std::max(maxDrift, std::abs(baselines[0] - baselines[1]) / 2.0);

			double variance = VarianceOf(interval.*series, meanB) / static_cast<double>((interval.*series).size());
			for (const auto* neighbour : neighbours)
				variance += VarianceOf(neighbour->*series, MeanOf(neighbour->*series)) /
				            static_cast<double>((neighbour->*series).size()) / static_cast<double>(neighbours.size() * neighbours.size());
			frameVariance += variance;
		}

		ABMetric result;
		if (pairDeltas.empty())
			return result;

		const double n = static_cast<double>(pairDeltas.size());
		const double delta = std::accumulate(pairDeltas.begin(), pairDeltas.end(), 0.0) / n;
		const auto [lowest, highest] = std::minmax_element(pairDeltas.begin(), pairDeltas.end());
		const double spread = (*highest - *lowest) / 2.0;
		const double frameError = std::sqrt(frameVariance) / n;

		result.valid = true;
		result.meanAMs = static_cast<float>(sumA / static_cast<double>(countA));
		result.meanBMs = static_cast<float>(sumB / static_cast<double>(countB));
		result.deltaMs = static_cast<float>(delta);
		result.uncertaintyMs = static_cast<float>(std::max({ maxDrift, spread, frameError }));
		if (std::abs(result.deltaMs) > std::max(result.uncertaintyMs, kVerdictNoiseFloorMs))
			result.verdict = result.deltaMs < 0.0f ? ABVerdict::Faster : ABVerdict::Slower;
		return result;
	}
}

ABComparison ComputeABComparison(const std::vector<ABHeadlineInterval>& intervals, bool frameLeads)
{
	const ABMetric gpu = CompareSeries(intervals, &ABHeadlineInterval::samplesMs);
	ABComparison result;
	result.valid = gpu.valid;
	result.meanAMs = gpu.meanAMs;
	result.meanBMs = gpu.meanBMs;
	result.deltaMs = gpu.deltaMs;
	result.uncertaintyMs = gpu.uncertaintyMs;
	result.verdict = gpu.verdict;
	result.frame = CompareSeries(intervals, &ABHeadlineInterval::frameMs);

	if (frameLeads && result.frame.valid) {
		if (result.frame.verdict != ABVerdict::NoChange)
			result.verdict = result.frame.verdict;
		else if (gpu.verdict != ABVerdict::NoChange) {
			result.headroomOnly = true;
			result.verdict = gpu.verdict;
		}
	}
	return result;
}
