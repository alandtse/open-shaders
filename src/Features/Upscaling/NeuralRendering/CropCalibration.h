#pragma once

#include "Utils/Region.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace NR
{
	/**
	 * @brief Measures how NR's GPU cost falls with crop size by sweeping centred crops of decreasing
	 *        area, and finds the largest crop that still costs about as little as the cheapest one.
	 *
	 * NR's GPU time flips between a fast and a slow state for seconds at a time with nothing changed,
	 * so the sweep repeats kPasses times, each step shorter than a phase, and each step keeps the fastest low-percentile time it saw;
	 * StabilityRatio reports how far apart the passes were.
	 *
	 * Pure state machine: the caller forces the crop reported by CurrentFraction() each frame and
	 * feeds the frame's NREvaluate GPU time to AddFrame().
	 */
	class CropCalibration
	{
	public:
		/** @brief Number of crop sizes the sweep evaluates. */
		static constexpr size_t kSteps = 7;
		/** @brief Times the whole sweep repeats; a step keeps its best pass. */
		static constexpr uint32_t kPasses = 4;
		/** @brief Share of a step's sorted samples below the value it records, so a slow burst does not set it. */
		static constexpr float kStepPercentile = 0.1f;
		/** @brief Crop area, as a share of the frame, each step evaluates; 1 is the uncropped frame. */
		static constexpr std::array<float, kSteps> kFractions{ 1.0f, 0.8f, 0.6f, 0.45f, 0.3f, 0.2f, 0.1f };
		/** @brief Frames a step discards after the crop changes, so NR's history reset does not skew it. */
		static constexpr uint32_t kSettleFrames = 15;
		/** @brief Frames a step samples after settling. */
		static constexpr uint32_t kSampleFrames = 45;
		/** @brief Fewest timed frames a step needs to be trusted. */
		static constexpr uint32_t kMinSamples = 15;
		/** @brief How far above the cheapest step a crop's cost may sit and still count as free. */
		static constexpr float kKneeTolerance = 0.05f;

		/**
		 * @brief True when a resolved capture is newer than the last one sampled, marking it consumed.
		 *        GetResults() keeps the previous frame's timings until the next capture resolves, so
		 *        sampling every main-thread frame would count one GPU frame many times, across steps.
		 */
		static bool ConsumeCapture(uint32_t a_capturedGpuFrame, uint32_t& a_lastConsumed)
		{
			if (a_capturedGpuFrame == a_lastConsumed)
				return false;
			a_lastConsumed = a_capturedGpuFrame;
			return true;
		}

		/** @brief Where a sweep stands. */
		enum class State : uint8_t
		{
			kIdle,
			kRunning,
			kDone,
			kFailed
		};

		/** @brief Why a sweep failed. */
		enum class Failure : uint8_t
		{
			kNone,
			kFrameGeneration,  ///< Frame generation paces the frame and skews GPU zones.
			kNotActive,        ///< NR was not running.
			kNoSamples         ///< A step never produced enough timed frames.
		};

		/** @brief The sweep's state and, once done, its measurements. */
		struct Result
		{
			State state = State::kIdle;
			Failure failure = Failure::kNone;
			/** @brief Fastest low-percentile NREvaluate GPU time per step over the passes, 0 for a step not measured. */
			std::array<float, kSteps> stepMs{};
			/** @brief Slowest over fastest pass value of the most unstable step; near 1 means steady timing. */
			float stabilityRatio = 1.0f;
			float floorMs = 0.0f;
			/** @brief Largest measured crop area whose cost is within kKneeTolerance of floorMs. */
			float kneeFraction = 1.0f;
		};

		/** @brief Begins a sweep, discarding any earlier result. */
		void Start()
		{
			result = {};
			result.state = State::kRunning;
			step = 0;
			pass = 0;
			frameInStep = 0;
			samples.clear();
			slowestMs.fill(0.0f);
		}

		/** @brief Ends the sweep with a failure. */
		void Fail(Failure a_failure)
		{
			result.state = State::kFailed;
			result.failure = a_failure;
		}

		/** @brief True while a sweep is in progress. */
		[[nodiscard]] bool Running() const { return result.state == State::kRunning; }

		/** @brief Crop area, as a share of the frame, the caller must force this frame. */
		[[nodiscard]] float CurrentFraction() const { return kFractions[std::min<size_t>(step, kSteps - 1)]; }

		/**
		 * @brief Accounts one frame of the current step.
		 * @param a_gpuMs NREvaluate GPU time this frame; zero or less when no fresh sample exists.
		 */
		void AddFrame(float a_gpuMs)
		{
			if (!Running())
				return;
			if (frameInStep >= kSettleFrames && a_gpuMs > 0.0f)
				samples.push_back(a_gpuMs);
			if (++frameInStep < kSettleFrames + kSampleFrames)
				return;
			if (samples.size() < kMinSamples) {
				Fail(Failure::kNoSamples);
				return;
			}
			std::sort(samples.begin(), samples.end());
			const float passMs = samples[static_cast<size_t>(kStepPercentile * static_cast<float>(samples.size()))];
			result.stepMs[step] = pass == 0 ? passMs : std::min(result.stepMs[step], passMs);
			slowestMs[step] = std::max(slowestMs[step], passMs);
			samples.clear();
			frameInStep = 0;
			if (++step < kSteps)
				return;
			step = 0;
			if (++pass >= kPasses)
				Finish();
		}

		/** @brief The current state and measurements. */
		[[nodiscard]] const Result& GetResult() const { return result; }

	private:
		void Finish()
		{
			result.floorMs = *std::min_element(result.stepMs.begin(), result.stepMs.end());
			for (size_t index = 0; index < kSteps; ++index)
				result.stabilityRatio = std::max(result.stabilityRatio, slowestMs[index] / result.stepMs[index]);
			result.kneeFraction = kFractions.back();
			for (size_t index = 0; index < kSteps; ++index) {
				if (result.stepMs[index] <= result.floorMs * (1.0f + kKneeTolerance)) {
					result.kneeFraction = kFractions[index];
					break;
				}
			}
			result.state = State::kDone;
		}

		Result result;
		std::array<float, kSteps> slowestMs{};
		size_t step = 0;
		uint32_t pass = 0;
		uint32_t frameInStep = 0;
		std::vector<float> samples;
	};

	/** @brief Screen bounds of a frame-centred rect covering a_areaFraction of the frame, square in proportion to it. */
	inline Util::Region::ScreenBounds CenteredBounds(float a_areaFraction)
	{
		const float half = 0.5f * std::sqrt(std::clamp(a_areaFraction, 0.0f, 1.0f));
		return { 0.5f - half, 0.5f - half, 0.5f + half, 0.5f + half };
	}
}
