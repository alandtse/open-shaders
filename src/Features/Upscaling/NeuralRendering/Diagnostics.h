#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

struct ID3D11Resource;
struct ID3D11ShaderResourceView;

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
			BypassWriteback = 64,
			BypassEvaluation = 128,
			CopyInputToOutput = 256,
			InteropRoundTrip = 512,
			BypassMask = 1024,
			ForceMaskZero = 2048,
			ForceMaskOne = 4096,
			VisualizeMask = 8192,
			DisableTone = 16384,
			DisableStructure = 32768,
			DisableSkin = 65536,
			DisableExposure = 131072,
			DisableColorTransform = 262144,
			VisualizeSkinMask = 524288,
			VisualizeAutoMask = 1048576
		};
		enum class ColorConversion : uint32_t
		{
			Raw,
			LinearToSRGB,
			SRGBToLinear,
			LinearToGamma22,
			Gamma22ToLinear,
			SRGBToGamma22,
			SkyrimGammaToGamma22,
			LinearToSkyrimGamma,
			LinearToLinear,
			Production
		};
		enum class ExposureMode : uint32_t
		{
			Production,
			Ignore,
			ForceOne,
			Game,
			Manual,
			DeExposeReExpose,
			PassOnly,
			DoNotPass
		};
		enum class CompositeMode : uint32_t
		{
			Production,
			Replacement,
			MaskedLerp,
			HalfMaskedLerp,
			PreserveLuminance,
			PreserveRatio,
			Residual,
			Ratio
		};
		enum class VisualMode : uint32_t
		{
			None,
			Input,
			Output,
			Difference,
			Ratio,
			Original,
			PostComposite,
			LuminanceDifference,
			ChromaDifference,
			Mask,
			Exposure,
			SplitOriginalOutput,
			SplitOriginalComposite,
			SplitInputOutput,
			SplitPrePost,
			LogRatio,
			ToneDelta,
			ToneLow,
			ToneHigh,
			ToneLowGain,
			FinalLuminanceRatio
		};
		/** @brief Returns the live, session-only isolation options. */
		uint32_t Options() const { return options.load(); }
		ColorConversion ConversionMode() const { return static_cast<ColorConversion>(conversionMode.load()); }
		ExposureMode ExposureSetting() const { return static_cast<ExposureMode>(exposureMode.load()); }
		CompositeMode CompositionMode() const { return static_cast<CompositeMode>(compositeMode.load()); }
		VisualMode ViewMode() const { return static_cast<VisualMode>(visualMode.load()); }
		float ManualExposure() const { return manualExposure.load(); }
		float DifferenceStrength() const { return differenceStrength.load(); }
		float SplitPosition() const { return splitPosition.load(); }
		float ShadowProtect() const { return shadowProtect.load(); }
		float HighlightProtect() const { return highlightProtect.load(); }
		float ToneRadius() const { return toneRadius.load(); }
		/** @brief Requests one lossless DDS capture of every stage in the next NR frame. */
		void RequestCapture() { captureRequested = true; }
		bool BeginCapture(uint32_t frame) { return captureRequested.exchange(false) ? (captureFrame = frame, true) : false; }
		bool CaptureActive(uint32_t frame) const { return captureFrame.load() == frame; }
		void FinishCapture(uint32_t frame) { captureFrame.compare_exchange_strong(frame, UINT32_MAX); }
		void CaptureStage(const char* stage, ID3D11Resource* resource, uint32_t frame);
		void CaptureView(const char* stage, ID3D11ShaderResourceView* view, uint32_t frame);
		/** @brief Writes a developer-requested texture dump and logs its resource description. */
		void DumpTexture(const char* stage, ID3D11Resource* resource, uint32_t frame);
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
			uint32_t width = 0, height = 0, format = 0, proxyFormat = 0, eyeCount = 0, evaluated = 0, copied = 0, created = 0;
			std::array<uint32_t, 2> reset{}, result{};
			uintptr_t source = 0;
			uint64_t submittedFence = 0, completedFence = 0;
			uint32_t conversion = 0, exposureMode = 0, compositeMode = 0, visualMode = 0;
			float manualExposure = 1.0f, differenceStrength = 1.0f, splitPosition = 0.5f;
			float intensity = 0.0f, localTone = 0.0f, localStructure = 0.0f, skinStructure = 0.0f;
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
		std::atomic<uint32_t> conversionMode = static_cast<uint32_t>(ColorConversion::Production), exposureMode = 0, compositeMode = 0, visualMode = 0;
		std::atomic<float> manualExposure = 1.0f, differenceStrength = 4.0f, splitPosition = 0.5f;
		std::atomic<float> shadowProtect = 0.0f, highlightProtect = 0.0f, toneRadius = 1.5f;
		std::atomic_bool captureRequested = false;
		std::atomic<uint32_t> captureFrame = UINT32_MAX;
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
