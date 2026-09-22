#pragma once

#include <array>
#include <cstdint>

namespace NeuralRendering
{
	/**
	 * Hysteretic controller for the optional dynamic foveated crop companion to
	 * adaptive NR.
	 *
	 * The controller intentionally selects only a small set of crop geometries.
	 * A crop geometry is a resource-boundary change in the current foveated
	 * renderer, so this class never asks the renderer to resize every frame.  It
	 * changes one tier at a time and exposes a short handoff alpha for the final
	 * full-eye transition pass to consume.
	 *
	 * Priority is deliberately asymmetric:
	 *
	 *   downshift: NR reaches its configured floor first, then crop coverage
	 *              decreases one tier at a time;
	 *   restore:   NR returns to 100% first, then crop coverage expands one tier
	 *              at a time and on a slower cadence.
	 *
	 * Eye-tracked foveation is a hard lockout.  A gaze-moving crop and an
	 * independently moving performance crop must not compete for the same UVs.
	 */
	class AdaptiveCropController
	{
	public:
		struct Config
		{
			bool enabled = false;
			std::uint32_t minimumCoverage = 75;
			std::uint32_t downshiftFrames = 2;
			std::uint32_t upshiftFrames = 24;
			std::uint32_t minimumDwellFrames = 60;
			std::uint32_t transitionFrames = 8;
		};

		void Reset();

		/**
		 * @param frame Current engine frame. Repeated calls for one frame are
		 *        ignored so VRS, the foveated route, and NR can share this state.
		 * @param config User-facing crop policy.
		 * @param eligible True only for the no-capture, post-upscale, stable VR
		 *        route that can consume the effective UV override.
		 * @param configuredCoverage The user's current crop's smaller dimension,
		 *        in percent. The controller never expands beyond this maximum.
		 * @param geometryCompatible False for asymmetric-size eye regions that the
		 *        shared foveated resource pool cannot represent safely.
		 * @param eyeTrackingEnabled Explicit eye-tracking/gaze lockout state.
		 * @param nrAtMinimum True when adaptive NR is already at its floor.
		 * @param nrTransitioning True while the NR handoff is still settling.
		 * @param nrAtMaximum True when adaptive NR has returned to 100%.
		 * @param overBudget Last adaptive-NR frame sample exceeded its guarded
		 *        application deadline.
		 * @param headroom Last adaptive-NR frame sample had sustained headroom.
		 */
		void Update(std::uint32_t frame, const Config& config, bool eligible,
			std::uint32_t configuredCoverage, bool geometryCompatible,
			bool eyeTrackingEnabled, bool nrAtMinimum, bool nrTransitioning,
			bool nrAtMaximum, bool overBudget, bool headroom);

		[[nodiscard]] std::uint32_t ActiveCoverage() const;
		[[nodiscard]] std::uint32_t TargetCoverage() const;
		[[nodiscard]] std::uint32_t MaximumCoverage() const;
		[[nodiscard]] std::uint32_t MinimumCoverage() const;
		[[nodiscard]] float HandoffAlpha() const;
		[[nodiscard]] bool IsTransitioning() const { return transitionFrameCount_ != 0; }
		[[nodiscard]] bool IsEnabled() const { return enabled_; }
		[[nodiscard]] bool IsRuntimeActive() const { return enabled_ && !eyeTrackingBlocked_; }
		[[nodiscard]] bool IsEyeTrackingBlocked() const { return eyeTrackingBlocked_; }
		[[nodiscard]] bool IsGeometryBlocked() const { return geometryBlocked_; }

		/** @brief Crop tiers used by the adaptive companion, largest first. */
		static constexpr const std::array<std::uint32_t, 7>& CoverageBuckets()
		{
			return kCoverageBuckets;
		}

	private:
		static constexpr std::array<std::uint32_t, 7> kCoverageBuckets{ 100, 95, 90, 85, 80, 75, 70 };

		static std::uint32_t FindBucketAtOrBelow(std::uint32_t coverage);
		static std::uint32_t FindBucketIndexAtOrBelow(std::uint32_t coverage);
		static Config NormalizeConfig(const Config& config);

		void ResetDecisionState();
		void StartTransition(std::uint32_t targetIndex, std::uint32_t frameCount);

		Config config_{};
		bool enabled_ = false;
		bool eyeTrackingBlocked_ = false;
		bool geometryBlocked_ = false;
		std::uint32_t lastFrame_ = UINT32_MAX;
		std::uint32_t activeBucket_ = 0;
		std::uint32_t targetBucket_ = 0;
		std::uint32_t maximumBucket_ = 0;
		std::uint32_t minimumBucket_ = 5;
		std::uint32_t transitionFrame_ = 0;
		std::uint32_t transitionFrameCount_ = 0;
		std::uint32_t dwellFrames_ = 0;
		std::uint32_t overrunFrames_ = 0;
		std::uint32_t headroomFrames_ = 0;
	};
}
