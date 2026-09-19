#include "AdaptiveController.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NeuralRendering
{
	std::uint32_t AdaptiveController::NormalizeRefresh(std::uint32_t refreshHz)
	{
		for (const auto target : kRefreshTargets)
			if (refreshHz == target)
				return target;
		return 80;
	}

	float AdaptiveController::ResolveTargetFps(const Config& config)
	{
		return config.targetFps != 0 ? static_cast<float>(config.targetFps) : static_cast<float>(config.refreshHz) * 0.5f;
	}

	std::uint32_t AdaptiveController::FindNearestBucket(std::uint32_t resolution)
	{
		std::uint32_t nearest = kResolutionBuckets.front();
		std::uint32_t distance = std::numeric_limits<std::uint32_t>::max();
		for (const auto bucket : kResolutionBuckets) {
			const auto candidateDistance = bucket > resolution ? bucket - resolution : resolution - bucket;
			if (candidateDistance < distance) {
				nearest = bucket;
				distance = candidateDistance;
			}
		}
		return nearest;
	}

	std::uint32_t AdaptiveController::FindBucketIndex(std::uint32_t resolution)
	{
		const auto bucket = FindNearestBucket(resolution);
		for (std::uint32_t index = 0; index < kResolutionBuckets.size(); ++index)
			if (kResolutionBuckets[index] == bucket)
				return index;
		return 0;
	}

	AdaptiveController::Config AdaptiveController::NormalizeConfig(const Config& config)
	{
		Config normalized = config;
		normalized.refreshHz = NormalizeRefresh(normalized.refreshHz);
		if (normalized.targetFps != 0)
			normalized.targetFps = std::clamp(normalized.targetFps, 15u, 60u);
		normalized.minimumResolution = FindNearestBucket(normalized.minimumResolution);
		normalized.downshiftFrames = std::clamp(normalized.downshiftFrames, 1u, 16u);
		normalized.upshiftFrames = std::clamp(normalized.upshiftFrames, 4u, 64u);
		normalized.minimumDwellFrames = std::clamp(normalized.minimumDwellFrames, 4u, 240u);
		normalized.guardTimeMs = std::clamp(normalized.guardTimeMs, 0.0f, 5.0f);
		return normalized;
	}

	void AdaptiveController::ResetDecisionState()
	{
		enabled_ = false;
		activeBucket_ = 0;
		targetBucket_ = 0;
		minimumBucket_ = 6;
		transitionFrame_ = 0;
		transitionFrameCount_ = 0;
		dwellFrames_ = 0;
		overrunFrames_ = 0;
		headroomFrames_ = 0;
		applicationDeadlineMs_ = 25.0f;
		lastFrameTimeMs_ = 0.0f;
		smoothedFrameTimeMs_ = 0.0f;
		lastSampleOverBudget_ = false;
		lastSampleHadHeadroom_ = false;
	}

	void AdaptiveController::Reset()
	{
		memoryCeiling_ = 100;
		config_ = {};
		hasTimestamp_ = false;
		lastFrame_ = UINT32_MAX;
		lastTimestamp_ = {};
		ResetDecisionState();
	}

	void AdaptiveController::StartTransition(std::uint32_t targetBucket, std::uint32_t frameCount)
	{
		if (targetBucket == activeBucket_)
			return;
		activeBucket_ = targetBucket;
		targetBucket_ = targetBucket;
		transitionFrame_ = 0;
		transitionFrameCount_ = std::max(frameCount, 1u);
		transitionElapsedMs_ = 0.0f;
		previousTransitionMs_ = 0.0f;
		transitionDurationMs_ = std::clamp(frameCount * applicationDeadlineMs_, 120.0f, 800.0f);
		dwellFrames_ = 0;
		overrunFrames_ = 0;
		headroomFrames_ = 0;
	}

	void AdaptiveController::Update(std::uint32_t frame, const Config& requestedConfig, bool eligible, float workloadMs, float elapsedMs)
	{
		const auto now = std::chrono::steady_clock::now();
		if (lastFrame_ == frame)
			return;
		lastFrame_ = frame;

		float frameTimeMs = 0.0f;
		if (hasTimestamp_) {
			frameTimeMs = std::chrono::duration<float, std::milli>(now - lastTimestamp_).count();
			if (!std::isfinite(frameTimeMs) || frameTimeMs <= 0.0f)
				frameTimeMs = 0.0f;
			frameTimeMs = std::clamp(frameTimeMs, 0.0f, 250.0f);
		}
		lastTimestamp_ = now;
		hasTimestamp_ = true;
		const float visualDeltaMs = std::clamp(std::isfinite(elapsedMs) && elapsedMs >= 0.0f ? elapsedMs : frameTimeMs, 0.0f, 50.0f);
		frameTimeMs = std::isfinite(workloadMs) && workloadMs > 0.0f && workloadMs <= 250.0f ? workloadMs : 0.0f;
		decisionReason_ = "hold";

		const Config config = NormalizeConfig(requestedConfig);
		const bool configurationChanged = config.enabled != config_.enabled ||
		                                  config.refreshHz != config_.refreshHz || config.targetFps != config_.targetFps ||
		                                  config.minimumResolution != config_.minimumResolution ||
		                                  config.downshiftFrames != config_.downshiftFrames || config.upshiftFrames != config_.upshiftFrames ||
		                                  config.minimumDwellFrames != config_.minimumDwellFrames ||
		                                  std::abs(config.guardTimeMs - config_.guardTimeMs) > 0.001f;
		config_ = config;

		const bool shouldRun = config.enabled && eligible;
		if (!shouldRun) {
			decisionReason_ = config.enabled ? "eligibility-loss" : "disabled";
			ResetDecisionState();
			// Do not carry an ineligible/capture/menu interval into the next
			// adaptive sample as if it were a real application frame.
			hasTimestamp_ = false;
			lastFrame_ = UINT32_MAX;
			return;
		}

		enabled_ = true;
		applicationDeadlineMs_ = 1000.0f / std::max(ResolveTargetFps(config), 1.0f);
		minimumBucket_ = FindBucketIndex(config.minimumResolution);
		if (config.memoryPressure)
			memoryCeiling_ = std::min(memoryCeiling_, ActiveResolution());
		if (ActiveResolution() > memoryCeiling_)
			frameTimeMs = std::max(frameTimeMs, applicationDeadlineMs_ * 1.3f);
		if (activeBucket_ > minimumBucket_)
			activeBucket_ = minimumBucket_;
		if (targetBucket_ > minimumBucket_)
			targetBucket_ = minimumBucket_;
		if (configurationChanged) {
			decisionReason_ = "configuration-change";
			transitionFrame_ = 0;
			transitionFrameCount_ = 0;
			dwellFrames_ = 0;
			overrunFrames_ = 0;
			headroomFrames_ = 0;
		}

		if (frameTimeMs > 0.0f) {
			lastFrameTimeMs_ = frameTimeMs;
			smoothedFrameTimeMs_ = smoothedFrameTimeMs_ == 0.0f ? frameTimeMs :
			                                                      smoothedFrameTimeMs_ * 0.90f + frameTimeMs * 0.10f;
		}

		if (transitionFrameCount_ != 0) {
			previousTransitionMs_ = transitionElapsedMs_;
			transitionElapsedMs_ += visualDeltaMs;
			if (transitionElapsedMs_ >= transitionDurationMs_) {
				transitionFrame_ = 0;
				transitionFrameCount_ = 0;
			} else {
				++transitionFrame_;
			}
		}

		++dwellFrames_;
		if (frameTimeMs == 0.0f) {
			decisionReason_ = "timing-unavailable";
			lastSampleOverBudget_ = false;
			lastSampleHadHeadroom_ = false;
			overrunFrames_ = 0;
			headroomFrames_ = 0;
			return;
		}

		const float guardedDeadline = std::max(1.0f, applicationDeadlineMs_ - config.guardTimeMs);
		const bool emergencyOverrun = frameTimeMs > applicationDeadlineMs_ * 1.25f;
		const bool overrun = emergencyOverrun || frameTimeMs > guardedDeadline ||
		                     (smoothedFrameTimeMs_ > guardedDeadline && frameTimeMs > applicationDeadlineMs_ * 0.95f);
		const float restorationBudget = std::min(applicationDeadlineMs_ * 0.85f,
			applicationDeadlineMs_ - config.guardTimeMs * 2.0f);
		const bool headroom = frameTimeMs < restorationBudget && smoothedFrameTimeMs_ < restorationBudget;
		lastSampleOverBudget_ = overrun;
		lastSampleHadHeadroom_ = headroom;
		if (overrun) {
			++overrunFrames_;
			headroomFrames_ = 0;
		} else if (headroom) {
			overrunFrames_ = 0;
			++headroomFrames_;
		} else {
			overrunFrames_ = 0;
			headroomFrames_ = 0;
		}
		if (!config.allowDownshift)
			overrunFrames_ = 0;

		if (IsTransitioning())
			return;
		if (dwellFrames_ >= config.minimumDwellFrames &&
			config.allowDownshift && (overrunFrames_ >= config.downshiftFrames || emergencyOverrun) && activeBucket_ < minimumBucket_) {
			decisionReason_ = "workload-pressure";
			StartTransition(activeBucket_ + 1, config.downshiftFrames);
			if (config.memoryPressure)
				memoryCeiling_ = std::min(memoryCeiling_, ActiveResolution());
			return;
		}

		if (config.allowUpshift && !config.memoryPressure && dwellFrames_ >= config.minimumDwellFrames &&
			headroomFrames_ >= config.upshiftFrames && activeBucket_ > 0 &&
			kResolutionBuckets[activeBucket_ - 1] <= memoryCeiling_) {
			decisionReason_ = "workload-headroom";
			StartTransition(activeBucket_ - 1, config.upshiftFrames);
		}
	}

	float AdaptiveController::ApplicationTargetFps() const
	{
		return ResolveTargetFps(config_);
	}

	std::uint32_t AdaptiveController::ActiveResolution() const
	{
		return kResolutionBuckets[std::min(activeBucket_, static_cast<std::uint32_t>(kResolutionBuckets.size() - 1))];
	}

	std::uint32_t AdaptiveController::TargetResolution() const
	{
		return kResolutionBuckets[std::min(targetBucket_, static_cast<std::uint32_t>(kResolutionBuckets.size() - 1))];
	}

	float AdaptiveController::HandoffAlpha() const
	{
		if (transitionFrameCount_ == 0)
			return 1.0f;
		const auto smooth = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
		const float previous = smooth(previousTransitionMs_ / transitionDurationMs_);
		const float current = smooth(transitionElapsedMs_ / transitionDurationMs_);
		// History already contains previous blends; incremental weight preserves the intended fade.
		return std::clamp((current - previous) / std::max(1.0f - previous, 0.0001f), 0.0f, 1.0f);
	}
}
