#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace NR
{
	/** @brief Bounded CPU-side tracing of NR scheduling and command submission. */
	class Diagnostics
	{
	public:
		enum class Outcome : uint8_t
		{
			NoHook,
			Disabled,
			NoWorld,
			Paused,
			FailedLatch,
			Error,
			Applied,
			Bypassed,
			Count
		};
		enum ResetReason : uint32_t
		{
			Requested = 1,
			FirstFrame = 2,
			FrameGap = 4,
			CameraPosition = 8,
			CameraDirection = 16,
			Projection = 32,
			FeatureCreated = 64
		};
		enum TestOption : uint32_t
		{
			IgnorePosition = 1,
			IgnoreCameraCuts = 2,
			ForceReset = 4,
			ZeroMotion = 8,
			ZeroJitter = 16,
			SerializeGPU = 32,
			BypassWriteback = 64
		};
		/** @brief Returns the live, session-only isolation options. */
		uint32_t Options() const { return options.load(); }
		struct CameraSample
		{
			std::array<float, 3> position{}, previous{}, enginePrevious{}, viewTranslation{};
			float distance = 0, directionDot = 0, projectionDelta = 0, jitterX = 0, jitterY = 0, frameTimeMs = 0;
			uint32_t detected = 0;
		};
		struct Frame
		{
			uint32_t options = 0;
			std::array<CameraSample, 2> camera{};
			uint32_t number = UINT32_MAX, calls = 0, duplicates = 0, target = 0;
			Outcome outcome = Outcome::NoHook;
			bool enabled = false, world = false, paused = false, recreated = false, afterUpscale = false, afterPost = false, mainChanged = false;
			uint32_t width = 0, height = 0, format = 0, eyeCount = 0, evaluated = 0, copied = 0, created = 0;
			std::array<uint32_t, 2> reset{}, result{};
			uintptr_t source = 0;
			uint64_t submittedFence = 0, completedFence = 0;
		};
		/** @brief Records entry without overwriting a successful result on duplicate calls. */
		Frame& BeginHook(uint32_t frame, uint32_t target);
		/** @brief Records how far the existing post-processing chain reached. */
		void Stage(uint32_t frame, bool finishedPost, uintptr_t main);
		/** @brief Publishes one record per engine frame, including frames with no NR hook. */
		void EndFrame(uint32_t frame, bool enabled, bool world, bool paused);
		/** @brief Draws trace controls in Upscaling's existing settings panel. */
		void DrawSettings();
		/** @brief Draws the latest completed-frame outcome and recent scheduling history. */
		void DrawOverlay(bool enabled, const std::string& status);

	private:
		static constexpr size_t kHistorySize = 120;
		static constexpr uint32_t kTraceFrames = 300;
		Frame current;
		std::atomic<uint32_t> options = 0;
		std::atomic_bool startSuite = false, startTrace = false, markFlicker = false;
		std::atomic_bool stopSuite = false;
		bool suite = false;
		uint32_t suiteStep = 0, suiteFrames = 0, savedOptions = 0;
		std::ofstream traceFile;
		std::string tracePath;
		static constexpr uint32_t kSuiteFrames = 600;
		static constexpr std::array<uint32_t, 8> kSuiteOptions{ 0, IgnorePosition, IgnoreCameraCuts, IgnorePosition | ForceReset, IgnorePosition | ZeroMotion, IgnorePosition | ZeroJitter, IgnorePosition | SerializeGPU, IgnorePosition | BypassWriteback };
		static constexpr std::array<const char*, 8> kSuiteNames{ "Baseline", "Ignore position resets", "Ignore all camera cuts", "Reset every frame", "Zero motion", "Zero NR jitter", "Serialize GPU", "Bypass NR writeback" };
		void OpenTrace();
		void WriteCameraTrace(const Frame& frame);
		std::mutex mutex;
		std::array<Frame, kHistorySize> history{};
		size_t next = 0, count = 0;
		std::atomic<uint32_t> traceRemaining = 0;
		std::atomic_bool clearRequested = false;
		bool showOverlay = true;
		uint32_t framesSinceSummary = 0;
		static const char* Name(Outcome outcome);
		static char Code(Outcome outcome);
		static void LogFrame(const Frame& frame);
	};
}
