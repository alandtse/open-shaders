#pragma once

#include "Utils/Region.h"

#include <algorithm>
#include <cmath>

/** @brief Crop parameters for the most-prominent-visible-actor source NR tracks. */
namespace NR::ActorRegion
{
	/** @brief Furthest a tracked actor may sit from the camera, in world units. */
	inline constexpr float kMaxActorDistance = 4096.0f;

	/** @brief Share of one eye a candidate must cover to be tracked at all. */
	inline constexpr float kMinVisibleAreaFraction = 0.005f;

	/** @brief Falloff width of the Gaussian weighting a candidate's score toward the frame centre, in NormalizedCenterDistance units. */
	inline constexpr float kCentralitySigma = 0.25f;

	/**
	 * @brief Multiplier the tracked actor's own score gets, so a near-tie does not flip the crop to a
	 *        similarly prominent actor and reset NR's temporal history.
	 */
	inline constexpr float kIncumbentScoreBonus = 1.2f;

	/** @brief Most actors a group crop may cover, the tracked actor included. */
	inline constexpr uint32_t kMaxGroupActors = 4;

	/** @brief Most candidates a group crop will test for line of sight each frame, best score first. */
	inline constexpr uint32_t kMaxGroupCandidatesTested = 6;

	/** @brief Largest share of an eye a group crop may cover; a bigger union would save NR little. */
	inline constexpr float kMaxGroupAreaFraction = 0.5f;

	/** @brief Bounds on the group crop area a calibrated cost knee may set. */
	inline constexpr float kMinCalibratedGroupAreaFraction = 0.1f, kMaxCalibratedGroupAreaFraction = 0.9f;

	/**
	 * @brief Largest share of an eye a group crop may cover: the measured cost knee when a
	 *        calibration produced one, else the default. Growing past the knee costs GPU time.
	 * @param a_kneeFraction Calibrated knee, or zero when none has been measured.
	 */
	inline float GroupAreaCap(float a_kneeFraction)
	{
		return a_kneeFraction > 0.0f ?
		           std::clamp(a_kneeFraction, kMinCalibratedGroupAreaFraction, kMaxCalibratedGroupAreaFraction) :
		           kMaxGroupAreaFraction;
	}

	/** @brief Padding added around the actor's projected bounds to build its crop. */
	inline constexpr Util::Region::Padding kPadding{ 1.0f / 8.0f, 16.0f, 32.0f, 96.0f };

	/** @brief Padding that adds no margin: the crop is the grid-aligned projected bounds alone. */
	inline constexpr Util::Region::Padding kTightPadding{ 0.0f, 0.0f, 0.0f, 0.0f };

	/** @brief Hold, shrink-window and shrink-threshold values the crop stabiliser runs with. */
	inline constexpr Util::Region::StabilizerPolicy kStabilizerPolicy{ 30, 60, 0.75f };

	/** @brief When a change to the published crop invalidates NR's temporal history. */
	inline constexpr Util::Region::ResetPolicy kResetPolicy = Util::Region::ResetPolicy::kOnChange;

	/** @brief Largest crop-edge drift that leaves NR's temporal history usable, in pixels. */
	inline constexpr float kHistoryTolerancePixels = static_cast<float>(Util::Region::kDefaultPixelAlignment);

	/**
	 * @brief Prominence of one eye's projected bounds: screen coverage, falling off with distance from
	 *        the frame centre, with a bonus for the actor the crop already follows.
	 */
	inline float ActorScore(const Util::Region::ScreenBounds& a_bounds, bool a_incumbent)
	{
		const float offset = Util::Region::NormalizedCenterDistance(a_bounds) / kCentralitySigma;
		const float score = Util::Region::AreaFraction(a_bounds) * std::exp(-0.5f * offset * offset);
		return a_incumbent ? score * kIncumbentScoreBonus : score;
	}
}
