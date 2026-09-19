#include "AdaptiveCropController.h"

#include <algorithm>

namespace NeuralRendering
{
	std::uint32_t AdaptiveCropController::FindBucketAtOrBelow(std::uint32_t coverage)
	{
		for (const auto bucket : kCoverageBuckets) {
			if (bucket <= coverage)
				return bucket;
		}
		return kCoverageBuckets.back();
	}

	std::uint32_t AdaptiveCropController::FindBucketIndexAtOrBelow(std::uint32_t coverage)
	{
		const auto bucket = FindBucketAtOrBelow(coverage);
		for (std::uint32_t index = 0; index < kCoverageBuckets.size(); ++index) {
			if (kCoverageBuckets[index] == bucket)
				return index;
		}
		return static_cast<std::uint32_t>(kCoverageBuckets.size() - 1);
	}

	AdaptiveCropController::Config AdaptiveCropController::NormalizeConfig(const Config& config)
	{
		Config normalized = config;
		normalized.minimumCoverage = FindBucketAtOrBelow(normalized.minimumCoverage);
		normalized.downshiftFrames = std::clamp(normalized.downshiftFrames, 1u, 16u);
		normalized.upshiftFrames = std::clamp(normalized.upshiftFrames, 8u, 240u);
		normalized.minimumDwellFrames = std::clamp(normalized.minimumDwellFrames, 8u, 600u);
		normalized.transitionFrames = std::clamp(normalized.transitionFrames, 2u, 24u);
		return normalized;
	}

	void AdaptiveCropController::ResetDecisionState()
	{
		enabled_ = false;
		activeBucket_ = 0;
		targetBucket_ = 0;
		maximumBucket_ = 0;
		minimumBucket_ = 5;
		transitionFrame_ = 0;
		transitionFrameCount_ = 0;
		dwellFrames_ = 0;
		overrunFrames_ = 0;
		headroomFrames_ = 0;
	}

	void AdaptiveCropController::Reset()
	{
		config_ = {};
		eyeTrackingBlocked_ = false;
		geometryBlocked_ = false;
		lastFrame_ = UINT32_MAX;
		ResetDecisionState();
	}

	void AdaptiveCropController::StartTransition(std::uint32_t targetIndex, std::uint32_t frameCount)
	{
		if (targetIndex == activeBucket_)
			return;
		activeBucket_ = targetIndex;
		targetBucket_ = targetIndex;
		transitionFrame_ = 0;
		transitionFrameCount_ = std::max(frameCount, 1u);
		dwellFrames_ = 0;
		overrunFrames_ = 0;
		headroomFrames_ = 0;
	}

	void AdaptiveCropController::Update(std::uint32_t frame, const Config& requestedConfig, bool eligible,
		std::uint32_t configuredCoverage, bool geometryCompatible, bool eyeTrackingEnabled,
		bool nrAtMinimum, bool nrTransitioning, bool nrAtMaximum, bool overBudget, bool headroom)
	{
		if (lastFrame_ == frame)
			return;
		lastFrame_ = frame;

		const Config config = NormalizeConfig(requestedConfig);
		const std::uint32_t maximumBucket = FindBucketIndexAtOrBelow(configuredCoverage);
		const std::uint32_t minimumBucket = std::max(
			maximumBucket, FindBucketIndexAtOrBelow(config.minimumCoverage));
		const bool configurationChanged = config.enabled != config_.enabled ||
			config.minimumCoverage != config_.minimumCoverage ||
			config.downshiftFrames != config_.downshiftFrames ||
			config.upshiftFrames != config_.upshiftFrames ||
			config.minimumDwellFrames != config_.minimumDwellFrames ||
			config.transitionFrames != config_.transitionFrames ||
			maximumBucket != maximumBucket_;
		config_ = config;
		maximumBucket_ = maximumBucket;
		minimumBucket_ = minimumBucket;

		eyeTrackingBlocked_ = eyeTrackingEnabled;
		geometryBlocked_ = !geometryCompatible || configuredCoverage < kCoverageBuckets.back();
		const bool shouldRun = eligible && config.enabled && !eyeTrackingBlocked_ && !geometryBlocked_;
		if (!shouldRun) {
			ResetDecisionState();
			// Keep the reason flags for UI/telemetry after ResetDecisionState().
			return;
		}

		enabled_ = true;
		if (configurationChanged) {
			activeBucket_ = maximumBucket_;
			targetBucket_ = maximumBucket_;
			transitionFrame_ = 0;
			transitionFrameCount_ = 0;
			dwellFrames_ = 0;
			overrunFrames_ = 0;
			headroomFrames_ = 0;
		}

		// The user's selected crop is the upper bound. If a profile is loaded
		// with a smaller crop, never silently enlarge it during restoration.
		activeBucket_ = std::clamp(activeBucket_, maximumBucket_, minimumBucket_);
		targetBucket_ = std::clamp(targetBucket_, maximumBucket_, minimumBucket_);

		if (transitionFrameCount_ != 0) {
			if (transitionFrame_ + 1 >= transitionFrameCount_) {
				transitionFrame_ = 0;
				transitionFrameCount_ = 0;
			} else {
				++transitionFrame_;
			}
		}

		++dwellFrames_;
		// Do not accumulate headroom while NR is moving between native tiers.
		// Otherwise the crop could expand on the very first frame after the NR
		// handoff, creating the compound visual/resource transition this controller
		// is specifically meant to avoid.
		if (nrTransitioning) {
			overrunFrames_ = 0;
			headroomFrames_ = 0;
			return;
		}
		if (overBudget) {
			++overrunFrames_;
			headroomFrames_ = 0;
		} else if (headroom) {
			overrunFrames_ = 0;
			// Headroom earned while NR is below 100% is reserved for NR's
			// restoration. It must not be banked and immediately widen the crop
			// on the first frame after NR reaches its maximum.
			if (nrAtMaximum)
				++headroomFrames_;
			else
				headroomFrames_ = 0;
		} else {
			overrunFrames_ = 0;
			headroomFrames_ = 0;
		}

		// NR must spend its own handoff before crop geometry is changed. This
		// is the key guard that prevents both resource domains from moving in
		// the same frame and creating a compound hitch.
		if (dwellFrames_ >= config.minimumDwellFrames && !nrTransitioning && nrAtMinimum &&
			overrunFrames_ >= config.downshiftFrames && activeBucket_ < minimumBucket_) {
			StartTransition(activeBucket_ + 1, config.transitionFrames);
			return;
		}

		// Restoration is intentionally gated on NR being fully restored first,
		// and uses the longer headroom window from the crop config.
		if (dwellFrames_ >= config.minimumDwellFrames && !nrTransitioning && nrAtMaximum &&
			headroomFrames_ >= config.upshiftFrames && activeBucket_ > maximumBucket_) {
			StartTransition(activeBucket_ - 1, config.transitionFrames);
		}
	}

	std::uint32_t AdaptiveCropController::ActiveCoverage() const
	{
		return kCoverageBuckets[std::min(activeBucket_, static_cast<std::uint32_t>(kCoverageBuckets.size() - 1))];
	}

	std::uint32_t AdaptiveCropController::TargetCoverage() const
	{
		return kCoverageBuckets[std::min(targetBucket_, static_cast<std::uint32_t>(kCoverageBuckets.size() - 1))];
	}

	std::uint32_t AdaptiveCropController::MaximumCoverage() const
	{
		return kCoverageBuckets[std::min(maximumBucket_, static_cast<std::uint32_t>(kCoverageBuckets.size() - 1))];
	}

	std::uint32_t AdaptiveCropController::MinimumCoverage() const
	{
		return kCoverageBuckets[std::min(minimumBucket_, static_cast<std::uint32_t>(kCoverageBuckets.size() - 1))];
	}

	float AdaptiveCropController::HandoffAlpha() const
	{
		if (transitionFrameCount_ == 0)
			return 1.0f;
		return std::clamp(static_cast<float>(transitionFrame_ + 1) /
			static_cast<float>(transitionFrameCount_), 0.05f, 1.0f);
	}
}
