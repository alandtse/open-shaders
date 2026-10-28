#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Scheduling and statistics for the in-game settings benchmark.
 *
 * The engine is pure: it never touches settings or the renderer. Each world frame the caller passes
 * the frame's timing sample, and the engine answers with the settings change to make. Every variant is
 * measured between two baseline blocks, so slow drift in the scene or GPU shows up as uncertainty on the
 * result instead of being credited to the setting.
 */
namespace SettingsBenchmark
{
	/** @brief Marks a block that runs the unchanged baseline settings. */
	inline constexpr int kBaseline = -1;

	/** @brief Frames of a block that must be valid before its mean is trusted. */
	inline constexpr uint32_t kMinimumValidFrames = 10;

	/** @brief Frame times at or above this are discarded as hitches (shader compiles, loads). */
	inline constexpr float kMaxFrameMs = 100.0f;

	struct Config
	{
		uint32_t warmupFrames = 45;     ///< Frames discarded after each settings change.
		uint32_t measureFrames = 90;    ///< Valid frames averaged per block.
		uint32_t repeats = 1;           ///< Times the whole variant sequence is run.
		float countdownSeconds = 8.0f;  ///< Delay before the first change, so a menu can be closed.
	};

	/** @brief Timing of one rendered frame. */
	struct Sample
	{
		float gpuMs = 0.0f;
		float cpuMs = 0.0f;
		float frameMs = 0.0f;  ///< Wall-clock frame interval; 0 when the source has none.
	};

	struct VariantInfo
	{
		std::string id;
		std::string label;
		std::string group;
	};

	/** @brief Measured cost of one variant against its bracketing baselines. */
	struct Result
	{
		std::string id;
		std::string label;
		std::string group;
		std::string error;  ///< Non-empty when the variant could not be measured.
		float gpuDeltaMs = 0.0f;
		float gpuUncertaintyMs = 0.0f;
		float gpuVariantMs = 0.0f;
		float gpuBaselineMs = 0.0f;
		float cpuDeltaMs = 0.0f;
		float frameDeltaMs = 0.0f;  ///< Frame-interval change; capped by vsync or a frame limiter.
		float frameBaselineMs = 0.0f;
		uint32_t repeats = 0;
	};

	enum class Phase
	{
		Idle,
		Countdown,
		Switching,
		Warmup,
		Measuring,
		Finished,
		Cancelled,
		Failed,
	};

	/** @brief What the caller must do to the live settings this frame. */
	struct Directive
	{
		bool apply = false;  ///< Apply `variant` (kBaseline means the original settings).
		int variant = kBaseline;
		bool restore = false;  ///< The run ended: put the original settings back.
	};

	struct Progress
	{
		Phase phase = Phase::Idle;
		uint32_t block = 0;
		uint32_t blockCount = 0;
		uint32_t framesInBlock = 0;
		uint32_t framesNeeded = 0;
		float countdownRemaining = 0.0f;
		int currentVariant = kBaseline;
	};

	class Engine
	{
	public:
		/**
		 * @brief Plans a run: baseline, variant, baseline, ... for every repeat.
		 * @return False when there is nothing to run or a run is already active.
		 */
		bool Start(std::vector<VariantInfo> variants, const Config& config);

		/**
		 * @brief Advances one frame.
		 * @param sample This frame's timing; ignored unless `hasSample` is true.
		 * @param hasSample False when the platform reported no timing for the frame.
		 * @param deltaSeconds Wall time since the previous call.
		 * @param canMeasure False while a menu or loading screen would distort the frame.
		 */
		Directive Tick(const Sample& sample, bool hasSample, float deltaSeconds, bool canMeasure);

		/** @brief Ends the run early; the caller must restore the original settings. */
		Directive Cancel();

		/** @brief Ends the run as failed; the caller must restore the original settings. */
		Directive Fail(std::string reason);

		bool IsRunning() const;
		Phase GetPhase() const { return phase; }
		Progress GetProgress() const;
		const std::string& FailureReason() const { return failure; }

		/** @brief Results for every variant whose bracketing blocks completed. */
		std::vector<Result> Results() const;

	private:
		struct Block
		{
			int variant = kBaseline;
			uint32_t validFrames = 0;
			uint32_t rejectedFrames = 0;
			double sumGpu = 0.0;
			double sumGpuSquares = 0.0;
			double sumCpu = 0.0;
			double sumFrame = 0.0;
			bool complete = false;
		};

		Directive Finish(Phase endPhase);

		Config config;
		std::vector<VariantInfo> variants;
		std::vector<Block> blocks;
		Phase phase = Phase::Idle;
		size_t current = 0;
		uint32_t framesInPhase = 0;
		float countdown = 0.0f;
		std::string failure;
	};

	/** @brief Display name for a phase. */
	const char* PhaseName(Phase phase);
}
