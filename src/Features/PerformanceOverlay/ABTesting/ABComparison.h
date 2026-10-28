#pragma once

#include <vector>

enum class ABVariant
{
	A,
	B
};

/** @brief Smallest headline delta (ms) reported as a change even when the measured uncertainty is smaller. */
inline constexpr float kVerdictNoiseFloorMs = 0.05f;

/** @brief Fewest headline samples an interval needs before the comparison uses it. */
inline constexpr size_t kMinimumHeadlineSamples = 10;

/** @brief Per-frame GPU cost samples of one measured A/B interval: compositor application GPU in VR, profiler total on flat. */
struct ABHeadlineInterval
{
	ABVariant variant = ABVariant::A;
	std::vector<float> samplesMs;
};

enum class ABVerdict
{
	NoChange,
	Faster,
	Slower,
};

/** @brief Variant B against variant A over the headline cost, with the noise that limits how far to trust it. */
struct ABComparison
{
	bool valid = false;  ///< False until a B interval is bracketed by usable A intervals.
	float meanAMs = 0.0f;
	float meanBMs = 0.0f;
	float deltaMs = 0.0f;        ///< meanB - meanA; negative means B is faster.
	float uncertaintyMs = 0.0f;  ///< Largest of the drift between bracketing A intervals, the spread of per-pair deltas and the frame-level error.
	ABVerdict verdict = ABVerdict::NoChange;
};

/**
 * @brief Compares variant B with variant A using each B interval's neighbouring A intervals as its baseline.
 *
 * Bracketing cancels slow drift in the scene or GPU clocks; whatever drift remains becomes uncertainty.
 * The verdict is Faster or Slower only when the delta exceeds both the uncertainty and kVerdictNoiseFloorMs.
 */
ABComparison ComputeABComparison(const std::vector<ABHeadlineInterval>& intervals);
