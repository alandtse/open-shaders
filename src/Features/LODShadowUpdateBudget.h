#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

/** @brief Estimates shadow batch throughput from recent submitted work and GPU time. */
class LODShadowUpdateBudget
{
public:
	/** @brief Records a measured batch without letting a tiny batch dominate the estimate. */
	void Record(uint32_t a_triangles, double a_elapsedMs, double a_targetMs)
	{
		if (!a_triangles || !std::isfinite(a_elapsedMs) || a_elapsedMs <= 0 || !std::isfinite(a_targetMs) || a_targetMs <= 0)
			return;
		samples[cursor] = { a_triangles, a_elapsedMs };
		cursor = (cursor + 1) % samples.size();
		uint64_t triangles = 0;
		double elapsedMs = 0;
		for (const auto& sample : samples) {
			triangles += sample.triangles;
			elapsedMs += sample.elapsedMs;
		}
		const double estimate = double(triangles) * a_targetMs / elapsedMs;
		triangleBudget = uint32_t(std::clamp(std::min(estimate, triangleBudget * kMaximumGrowth), double(kMinimumTriangles), double(kMaximumTriangles)));
	}

	/** @brief Returns the triangle allowance for one ordinary update batch. */
	uint32_t Triangles() const { return triangleBudget; }

private:
	static constexpr uint32_t kMinimumTriangles = 256;
	static constexpr uint32_t kInitialTriangles = 8192;
	static constexpr uint32_t kMaximumTriangles = 4194304;
	static constexpr std::size_t kSampleCount = 16;
	static constexpr double kMaximumGrowth = 1.1;
	struct Sample
	{
		uint32_t triangles = 0;
		double elapsedMs = 0;
	};
	std::array<Sample, kSampleCount> samples{};
	std::size_t cursor = 0;
	uint32_t triangleBudget = kInitialTriangles;
};
