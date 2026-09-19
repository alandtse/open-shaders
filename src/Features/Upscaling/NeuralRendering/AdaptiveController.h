#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace NeuralRendering
{
	/**
	 * Small hysteretic controller for the opt-in adaptive Neural Rendering test.
	 *
	 * The controller only selects a native resolution bucket. It does not own
	 * graphics resources and it never changes the display or compositor mode.
	 */
	class AdaptiveController
	{
	public:
		struct Config
		{
			bool enabled = false;
			bool allowDownshift = true;
			bool allowUpshift = true;
			bool memoryPressure = false;
			std::uint32_t refreshHz = 80;
			// Zero derives the budget from refreshHz; otherwise use 15–60 FPS.
			std::uint32_t targetFps = 0;
			std::uint32_t minimumResolution = 70;
			std::uint32_t downshiftFrames = 4;
			std::uint32_t upshiftFrames = 12;
			std::uint32_t minimumDwellFrames = 30;
			float guardTimeMs = 1.0f;
		};

		void Reset();
		/** @brief Negative workload means unavailable; paced frame intervals never drive quality. */
		void Update(std::uint32_t frame, const Config& config, bool eligible, float workloadMs = -1.0f, float elapsedMs = -1.0f);
		[[nodiscard]] const char* DecisionReason() const { return decisionReason_; }

		[[nodiscard]] std::uint32_t ActiveResolution() const;
		[[nodiscard]] std::uint32_t TargetResolution() const;
		[[nodiscard]] float HandoffAlpha() const;
		[[nodiscard]] float LastFrameTimeMs() const { return lastFrameTimeMs_; }
		[[nodiscard]] float SmoothedFrameTimeMs() const { return smoothedFrameTimeMs_; }
		[[nodiscard]] float ApplicationDeadlineMs() const { return applicationDeadlineMs_; }
		[[nodiscard]] float ApplicationTargetFps() const;
		[[nodiscard]] bool IsTransitioning() const { return transitionFrameCount_ != 0; }
		[[nodiscard]] bool IsEnabled() const { return enabled_; }
		[[nodiscard]] bool LastSampleOverBudget() const { return lastSampleOverBudget_; }
		[[nodiscard]] bool LastSampleHadHeadroom() const { return lastSampleHadHeadroom_; }
		[[nodiscard]] bool IsAtMinimum() const { return activeBucket_ >= minimumBucket_; }
		[[nodiscard]] bool IsAtMaximum() const { return activeBucket_ == 0; }
		/** @brief Session memory ceiling; only an explicit Reset clears learned pressure. */
		[[nodiscard]] std::uint32_t MemoryCeiling() const { return memoryCeiling_; }

		/** @brief Returns the supported controller refresh/budget targets. */
		static constexpr const std::array<std::uint32_t, 4>& RefreshTargets()
		{
			return kRefreshTargets;
		}

		/** @brief Returns the native buckets used by the adaptive test. */
		static constexpr const std::array<std::uint32_t, 7>& ResolutionBuckets()
		{
			return kResolutionBuckets;
		}

	private:
		static constexpr std::array<std::uint32_t, 4> kRefreshTargets{ 70, 72, 80, 90 };
		static constexpr std::array<std::uint32_t, 7> kResolutionBuckets{ 100, 95, 90, 85, 80, 75, 70 };

		static std::uint32_t NormalizeRefresh(std::uint32_t refreshHz);
		static float ResolveTargetFps(const Config& config);
		static std::uint32_t FindNearestBucket(std::uint32_t resolution);
		static std::uint32_t FindBucketIndex(std::uint32_t resolution);
		static Config NormalizeConfig(const Config& config);

		void ResetDecisionState();
		void StartTransition(std::uint32_t targetBucket, std::uint32_t frameCount);

		Config config_{};
		std::uint32_t memoryCeiling_ = 100;
		bool enabled_ = false;
		bool hasTimestamp_ = false;
		std::uint32_t lastFrame_ = UINT32_MAX;
		std::chrono::steady_clock::time_point lastTimestamp_{};
		std::uint32_t activeBucket_ = 0;
		std::uint32_t targetBucket_ = 0;
		std::uint32_t minimumBucket_ = 6;
		std::uint32_t transitionFrame_ = 0;
		std::uint32_t transitionFrameCount_ = 0;
		std::uint32_t dwellFrames_ = 0;
		std::uint32_t overrunFrames_ = 0;
		std::uint32_t headroomFrames_ = 0;
		float applicationDeadlineMs_ = 25.0f;
		float transitionElapsedMs_ = 0.0f;
		float previousTransitionMs_ = 0.0f;
		float transitionDurationMs_ = 0.0f;
		const char* decisionReason_ = "initial";
		float lastFrameTimeMs_ = 0.0f;
		float smoothedFrameTimeMs_ = 0.0f;
		bool lastSampleOverBudget_ = false;
		bool lastSampleHadHeadroom_ = false;
	};
}
