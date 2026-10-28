#include "SettingsBenchmarkEngine.h"

#include <algorithm>
#include <cmath>

namespace SettingsBenchmark
{
	namespace
	{
		/** @brief A block that never reaches its frame target fails the run after this many attempts per wanted frame. */
		constexpr uint32_t kAttemptsPerWantedFrame = 20;

		struct BlockMean
		{
			double gpu = 0.0;
			double cpu = 0.0;
			double frame = 0.0;
			double standardError = 0.0;
		};

		template <typename Block>
		BlockMean Mean(const Block& block)
		{
			const double n = block.validFrames;
			BlockMean mean;
			mean.gpu = block.sumGpu / n;
			mean.cpu = block.sumCpu / n;
			mean.frame = block.sumFrame / n;
			const double variance = std::max(0.0, block.sumGpuSquares / n - mean.gpu * mean.gpu);
			mean.standardError = std::sqrt(variance / n);
			return mean;
		}
	}

	const char* PhaseName(Phase phase)
	{
		switch (phase) {
		case Phase::Idle:
			return "Idle";
		case Phase::Countdown:
			return "Countdown";
		case Phase::Switching:
			return "Switching";
		case Phase::Warmup:
			return "Warmup";
		case Phase::Measuring:
			return "Measuring";
		case Phase::Finished:
			return "Finished";
		case Phase::Cancelled:
			return "Cancelled";
		case Phase::Failed:
			return "Failed";
		}
		return "Unknown";
	}

	bool Engine::Start(std::vector<VariantInfo> a_variants, const Config& a_config)
	{
		if (IsRunning() || a_variants.empty())
			return false;

		config = a_config;
		config.measureFrames = std::max(config.measureFrames, kMinimumValidFrames);
		config.repeats = std::max<uint32_t>(config.repeats, 1);
		config.countdownSeconds = std::max(config.countdownSeconds, 0.0f);
		variants = std::move(a_variants);

		blocks.clear();
		for (uint32_t repeat = 0; repeat < config.repeats; ++repeat) {
			blocks.push_back({ .variant = kBaseline });
			for (size_t index = 0; index < variants.size(); ++index) {
				blocks.push_back({ .variant = static_cast<int>(index) });
				blocks.push_back({ .variant = kBaseline });
			}
		}

		current = 0;
		framesInPhase = 0;
		countdown = config.countdownSeconds;
		failure.clear();
		phase = countdown > 0.0f ? Phase::Countdown : Phase::Switching;
		return true;
	}

	Directive Engine::Tick(const Sample& sample, bool hasSample, float deltaSeconds, bool canMeasure)
	{
		switch (phase) {
		case Phase::Countdown:
			countdown -= deltaSeconds;
			if (countdown <= 0.0f)
				phase = Phase::Switching;
			return {};

		case Phase::Switching:
			if (!canMeasure)
				return {};
			phase = config.warmupFrames > 0 ? Phase::Warmup : Phase::Measuring;
			framesInPhase = 0;
			return { .apply = true, .variant = blocks[current].variant };

		case Phase::Warmup:
			if (canMeasure && ++framesInPhase >= config.warmupFrames) {
				phase = Phase::Measuring;
				framesInPhase = 0;
			}
			return {};

		case Phase::Measuring:
			{
				if (!canMeasure)
					return {};
				auto& block = blocks[current];
				if (++framesInPhase > config.measureFrames * kAttemptsPerWantedFrame)
					return Fail("The frame timing source reported too few valid frames.");

				const bool hitch = sample.frameMs >= kMaxFrameMs;
				if (hasSample && sample.gpuMs > 0.0f && sample.gpuMs < kMaxFrameMs && !hitch) {
					++block.validFrames;
					block.sumGpu += sample.gpuMs;
					block.sumGpuSquares += static_cast<double>(sample.gpuMs) * sample.gpuMs;
					block.sumCpu += sample.cpuMs;
					block.sumFrame += sample.frameMs;
				} else {
					++block.rejectedFrames;
				}

				if (block.validFrames >= config.measureFrames) {
					block.complete = true;
					if (++current >= blocks.size())
						return Finish(Phase::Finished);
					phase = Phase::Switching;
				}
				return {};
			}

		case Phase::Idle:
		case Phase::Finished:
		case Phase::Cancelled:
		case Phase::Failed:
			break;
		}
		return {};
	}

	Directive Engine::Cancel()
	{
		return IsRunning() ? Finish(Phase::Cancelled) : Directive{};
	}

	Directive Engine::Fail(std::string reason)
	{
		if (!IsRunning())
			return {};
		failure = std::move(reason);
		return Finish(Phase::Failed);
	}

	Directive Engine::Finish(Phase endPhase)
	{
		phase = endPhase;
		return { .restore = true };
	}

	bool Engine::IsRunning() const
	{
		return phase == Phase::Countdown || phase == Phase::Switching || phase == Phase::Warmup || phase == Phase::Measuring;
	}

	Progress Engine::GetProgress() const
	{
		Progress progress;
		progress.phase = phase;
		progress.blockCount = static_cast<uint32_t>(blocks.size());
		progress.block = static_cast<uint32_t>(std::min(current + 1, blocks.size()));
		progress.countdownRemaining = std::max(countdown, 0.0f);
		if (current < blocks.size()) {
			progress.currentVariant = blocks[current].variant;
			progress.framesInBlock = phase == Phase::Measuring ? blocks[current].validFrames : framesInPhase;
		}
		progress.framesNeeded = phase == Phase::Warmup ? config.warmupFrames : config.measureFrames;
		return progress;
	}

	std::vector<Result> Engine::Results() const
	{
		std::vector<Result> results;
		const size_t perRepeat = variants.size() * 2 + 1;
		if (variants.empty() || blocks.size() != perRepeat * config.repeats)
			return results;

		for (size_t variantIndex = 0; variantIndex < variants.size(); ++variantIndex) {
			Result result{ .id = variants[variantIndex].id, .label = variants[variantIndex].label, .group = variants[variantIndex].group };
			std::vector<double> deltas;
			double cpuDeltaSum = 0.0, frameDeltaSum = 0.0, frameBaselineSum = 0.0, uncertaintySum = 0.0, variantSum = 0.0, baselineSum = 0.0;

			for (uint32_t repeat = 0; repeat < config.repeats; ++repeat) {
				const size_t first = repeat * perRepeat + variantIndex * 2;
				const auto& before = blocks[first];
				const auto& measured = blocks[first + 1];
				const auto& after = blocks[first + 2];
				if (!before.complete || !measured.complete || !after.complete)
					continue;

				const auto baselineBefore = Mean(before);
				const auto baselineAfter = Mean(after);
				const auto variantMean = Mean(measured);
				const double baseline = (baselineBefore.gpu + baselineAfter.gpu) * 0.5;
				const double drift = std::abs(baselineBefore.gpu - baselineAfter.gpu) * 0.5;

				deltas.push_back(variantMean.gpu - baseline);
				cpuDeltaSum += variantMean.cpu - (baselineBefore.cpu + baselineAfter.cpu) * 0.5;
				const double frameBaseline = (baselineBefore.frame + baselineAfter.frame) * 0.5;
				frameDeltaSum += variantMean.frame - frameBaseline;
				frameBaselineSum += frameBaseline;
				uncertaintySum += std::max(drift, variantMean.standardError);
				variantSum += variantMean.gpu;
				baselineSum += baseline;
			}

			if (deltas.empty()) {
				result.error = "Not measured";
				results.push_back(std::move(result));
				continue;
			}

			const double n = static_cast<double>(deltas.size());
			double meanDelta = 0.0;
			for (const double delta : deltas)
				meanDelta += delta;
			meanDelta /= n;

			double uncertainty = uncertaintySum / n;
			if (deltas.size() > 1) {
				double squares = 0.0;
				for (const double delta : deltas)
					squares += (delta - meanDelta) * (delta - meanDelta);
				uncertainty = std::max(uncertainty, std::sqrt(squares / (n - 1.0) / n));
			}

			result.gpuDeltaMs = static_cast<float>(meanDelta);
			result.gpuUncertaintyMs = static_cast<float>(uncertainty);
			result.gpuVariantMs = static_cast<float>(variantSum / n);
			result.gpuBaselineMs = static_cast<float>(baselineSum / n);
			result.cpuDeltaMs = static_cast<float>(cpuDeltaSum / n);
			result.frameDeltaMs = static_cast<float>(frameDeltaSum / n);
			result.frameBaselineMs = static_cast<float>(frameBaselineSum / n);
			result.repeats = static_cast<uint32_t>(deltas.size());
			results.push_back(std::move(result));
		}
		return results;
	}
}
