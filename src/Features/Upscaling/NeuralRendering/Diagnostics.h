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
	/** @brief Camera-cut detection thresholds; the detection and the trace legend must agree. */
	inline constexpr float kCameraCutDistance = 256.0f;
	inline constexpr float kCameraCutDirectionDot = 0.5f;
	inline constexpr float kProjectionCutThreshold = 0.1f;

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
			Suspended,
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
			FeatureCreated = 64,
			RegionChanged = 128
		};
		/** @brief Which history resets a frame needs: the request, the first frame, or a frame gap. */
		static uint32_t FrameResetReasons(bool requested, uint32_t lastFrame, uint32_t frame)
		{
			uint32_t reasons = requested ? Requested : 0;
			if (lastFrame == UINT32_MAX)
				reasons |= FirstFrame;
			else if (lastFrame + 1 != frame)
				reasons |= FrameGap;
			return reasons;
		}
		enum TestOption : uint32_t
		{
			IgnorePosition = 1,
			ApplyCameraCuts = 2,
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
			VisualizeAutoMask = 1048576,
			FeedCameraData = 2097152,
			DilateMotion = 4194304
		};
		/**
		 * @brief Options under which NR's history is deliberately isolated, so the eye-stagger must
		 *        evaluate both eyes instead of reusing a gain: they bypass the model, force a reset every
		 *        frame or zero the motion a reuse would reproject by.
		 */
		static constexpr uint32_t kHistoryIsolatingOptions = BypassEvaluation | CopyInputToOutput | InteropRoundTrip | ForceReset | ZeroMotion;
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
			FinalLuminanceRatio,
			Category
		};
		/** @brief Mask mode ColorTransferCS.hlsl reads to substitute a constant mask. */
		enum class MaskMode : uint32_t
		{
			Automatic,
			ForceZero,
			ForceOne
		};
		// ColorTransferCS.hlsl reads these numbers through ModeValues.hlsli: changing one here
		// without that file silently renders the wrong mode.
		static_assert(static_cast<uint32_t>(ColorConversion::Raw) == 0);
		static_assert(static_cast<uint32_t>(ColorConversion::LinearToSRGB) == 1);
		static_assert(static_cast<uint32_t>(ColorConversion::SRGBToLinear) == 2);
		static_assert(static_cast<uint32_t>(ColorConversion::LinearToGamma22) == 3);
		static_assert(static_cast<uint32_t>(ColorConversion::Gamma22ToLinear) == 4);
		static_assert(static_cast<uint32_t>(ColorConversion::SRGBToGamma22) == 5);
		static_assert(static_cast<uint32_t>(ColorConversion::SkyrimGammaToGamma22) == 6);
		static_assert(static_cast<uint32_t>(ColorConversion::LinearToSkyrimGamma) == 7);
		static_assert(static_cast<uint32_t>(ColorConversion::LinearToLinear) == 8);
		static_assert(static_cast<uint32_t>(ColorConversion::Production) == 9);
		static_assert(static_cast<uint32_t>(ExposureMode::Production) == 0);
		static_assert(static_cast<uint32_t>(ExposureMode::Ignore) == 1);
		static_assert(static_cast<uint32_t>(ExposureMode::ForceOne) == 2);
		static_assert(static_cast<uint32_t>(ExposureMode::Game) == 3);
		static_assert(static_cast<uint32_t>(ExposureMode::Manual) == 4);
		static_assert(static_cast<uint32_t>(ExposureMode::DeExposeReExpose) == 5);
		static_assert(static_cast<uint32_t>(ExposureMode::PassOnly) == 6);
		static_assert(static_cast<uint32_t>(ExposureMode::DoNotPass) == 7);
		static_assert(static_cast<uint32_t>(CompositeMode::Production) == 0);
		static_assert(static_cast<uint32_t>(CompositeMode::Replacement) == 1);
		static_assert(static_cast<uint32_t>(CompositeMode::MaskedLerp) == 2);
		static_assert(static_cast<uint32_t>(CompositeMode::HalfMaskedLerp) == 3);
		static_assert(static_cast<uint32_t>(CompositeMode::PreserveLuminance) == 4);
		static_assert(static_cast<uint32_t>(CompositeMode::PreserveRatio) == 5);
		static_assert(static_cast<uint32_t>(CompositeMode::Residual) == 6);
		static_assert(static_cast<uint32_t>(CompositeMode::Ratio) == 7);
		static_assert(static_cast<uint32_t>(MaskMode::Automatic) == 0);
		static_assert(static_cast<uint32_t>(MaskMode::ForceZero) == 1);
		static_assert(static_cast<uint32_t>(MaskMode::ForceOne) == 2);
		static_assert(static_cast<uint32_t>(VisualMode::None) == 0);
		static_assert(static_cast<uint32_t>(VisualMode::Input) == 1);
		static_assert(static_cast<uint32_t>(VisualMode::Output) == 2);
		static_assert(static_cast<uint32_t>(VisualMode::Difference) == 3);
		static_assert(static_cast<uint32_t>(VisualMode::Ratio) == 4);
		static_assert(static_cast<uint32_t>(VisualMode::Original) == 5);
		static_assert(static_cast<uint32_t>(VisualMode::PostComposite) == 6);
		static_assert(static_cast<uint32_t>(VisualMode::LuminanceDifference) == 7);
		static_assert(static_cast<uint32_t>(VisualMode::ChromaDifference) == 8);
		static_assert(static_cast<uint32_t>(VisualMode::Mask) == 9);
		static_assert(static_cast<uint32_t>(VisualMode::Exposure) == 10);
		static_assert(static_cast<uint32_t>(VisualMode::SplitOriginalOutput) == 11);
		static_assert(static_cast<uint32_t>(VisualMode::SplitOriginalComposite) == 12);
		static_assert(static_cast<uint32_t>(VisualMode::SplitInputOutput) == 13);
		static_assert(static_cast<uint32_t>(VisualMode::SplitPrePost) == 14);
		static_assert(static_cast<uint32_t>(VisualMode::LogRatio) == 15);
		static_assert(static_cast<uint32_t>(VisualMode::ToneDelta) == 16);
		static_assert(static_cast<uint32_t>(VisualMode::ToneLow) == 17);
		static_assert(static_cast<uint32_t>(VisualMode::ToneHigh) == 18);
		static_assert(static_cast<uint32_t>(VisualMode::ToneLowGain) == 19);
		static_assert(static_cast<uint32_t>(VisualMode::FinalLuminanceRatio) == 20);
		static_assert(static_cast<uint32_t>(VisualMode::Category) == 21);
		/** @brief Production defaults the diagnostic value knobs fall back to. */
		static constexpr float kManualExposure = 1.0f, kDifferenceStrength = 4.0f, kSplitPosition = 0.5f;
		static constexpr float kShadowProtect = 0.0f, kHighlightProtect = 0.0f, kToneRadius = 1.0f;
		/** @brief Enables the diagnostic options, UI, overlay and logging; off outside dev mode. */
		void SetDeveloperMode(bool enabled) { developerMode.store(enabled, std::memory_order_relaxed); }
		/** @brief True while developer mode has the NR diagnostics switched on. */
		bool DeveloperMode() const { return developerMode.load(std::memory_order_relaxed); }
		/** @brief Every diagnostic value a render may use, resolved for the current mode. */
		struct Selection
		{
			ColorConversion conversion = ColorConversion::Production;
			ExposureMode exposure = ExposureMode::Production;
			CompositeMode composite = CompositeMode::Production;
			VisualMode visual = VisualMode::None;
			uint32_t options = 0;
			float manualExposure = kManualExposure;
			float differenceStrength = kDifferenceStrength;
			float splitPosition = kSplitPosition;
			float shadowProtect = kShadowProtect;
			float highlightProtect = kHighlightProtect;
			float toneRadius = kToneRadius;
		};
		/** @brief Resolves every option; outside developer mode a render reads the production defaults. */
		Selection Selected() const;
		/**
		 * @brief Requests one lossless DDS capture of every stage in the next NR frame.
		 * @param verifyReuse On a staggered frame, also run the model on the eye that reuses its gain and dump its
		 *        input, model output and reused output, so the reuse error can be measured offline.
		 */
		void RequestCapture(bool verifyReuse = false)
		{
			verifyReuseRequested = verifyReuse;
			captureRequested = true;
		}
		/** @brief True once for a capture that asked to verify the reuse; call only on the capture frame. */
		bool ConsumeVerifyReuse() { return verifyReuseRequested.exchange(false); }
		/** @brief A queued capture wants the verify dumps, which only the NR pass itself can write. */
		bool VerifyReusePending() const { return verifyReuseRequested.load(); }
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
			bool world = false, paused = false, recreated = false, afterUpscale = false, afterPost = false, mainChanged = false;
			/** @brief evaluated, reused and copied are per-eye bitmasks over the eyes this frame handled. */
			uint32_t width = 0, height = 0, format = 0, proxyFormat = 0, eyeCount = 0, evaluated = 0, reused = 0, copied = 0, created = 0;
			std::array<uint32_t, 2> reset{}, result{};
			uintptr_t source = 0;
			uint64_t submittedFence = 0, completedFence = 0;
			/** @brief CPU milliseconds this frame blocked in the reset drain; zero when nothing reset. */
			float resetDrainMs = 0.0f;
			uint32_t conversion = 0, exposureMode = 0, compositeMode = 0, visualMode = 0;
			float manualExposure = 1.0f, differenceStrength = 1.0f, splitPosition = 0.5f;
			float intensity = 0.0f, localTone = 0.0f, localStructure = 0.0f, skinStructure = 0.0f;
		};
		/** @brief Short names of the eight history-reset reasons, indexed by bit position. Matches ResetReason. */
		static constexpr std::array<const char*, 8> kResetReasonNames{
			"request", "first", "gap", "position", "direction", "projection", "creation", "region"
		};
		/** @brief Cumulative NR counters since startup; every field is monotonic so a caller may difference two reads. */
		struct Counters
		{
			std::array<uint64_t, kResetReasonNames.size()> resets{};  ///< Frames that carried each reset reason.
			double drainMs = 0.0;                                     ///< Cumulative CPU time blocked in the reset drain.
		};
		/** @brief Adds one applied frame's reset reasons and drain cost to the cumulative counters. */
		void RecordFrame(uint32_t resetReasons, double drainMs);
		/** @brief Snapshot of the cumulative counters, safe from any thread. */
		Counters GetCounters() const;
		/** @brief Records entry without overwriting a successful result on duplicate calls. */
		Frame& BeginHook(uint32_t frame, uint32_t target);
		/** @brief Records how far the existing post-processing chain reached. */
		void Stage(uint32_t frame, bool finishedPost, uintptr_t main);
		/** @brief Publishes one record per engine frame, including frames with no NR hook. */
		void EndFrame(uint32_t frame, bool world, bool paused);
		/** @brief Draws diagnostic settings in Upscaling's existing settings panel. */
		void DrawSettings();
		/** @brief Draws the latest completed-frame outcome and recent scheduling history. */
		void DrawOverlay(const std::string& status);
		/** @brief True while the developer has switched the overlay on; off by default. */
		bool OverlayVisible() const { return DeveloperMode() && showOverlay.load(std::memory_order_relaxed); }

	private:
		static constexpr size_t kHistorySize = 120;
		Frame current;
		std::atomic<uint32_t> options = 0;
		std::atomic_bool developerMode = false;
		std::atomic<uint32_t> conversionMode = static_cast<uint32_t>(ColorConversion::Production), exposureMode = 0, compositeMode = 0, visualMode = 0;
		std::atomic<float> manualExposure = kManualExposure, differenceStrength = kDifferenceStrength, splitPosition = kSplitPosition;
		std::atomic<float> shadowProtect = kShadowProtect, highlightProtect = kHighlightProtect, toneRadius = kToneRadius;
		std::atomic_bool captureRequested = false;
		std::atomic_bool verifyReuseRequested = false;
		std::atomic<uint32_t> captureFrame = UINT32_MAX;
		std::atomic_bool startSuite = false;
		std::atomic_bool stopSuite = false;
		bool suite = false;
		uint32_t suiteStep = 0, suiteFrames = 0, savedOptions = 0;
		std::ofstream traceFile;
		std::string tracePath;
		static constexpr uint32_t kSuiteFrames = 600;
		static constexpr std::array<uint32_t, 8> kSuiteOptions{ 0, ApplyCameraCuts, ApplyCameraCuts | IgnorePosition, ForceReset, ZeroMotion, ZeroJitter, SerializeGPU, BypassWriteback };
		static constexpr std::array<const char*, kSuiteOptions.size()> kSuiteNames{ "Baseline", "Apply inferred camera cuts", "Apply direction/projection cuts", "Reset every frame", "Zero motion", "Zero NR jitter", "Serialize GPU", "Bypass NR writeback" };
		void OpenTrace();
		void WriteCameraTrace(const Frame& frame);
		/** @brief Draws the isolation-test options, which a running suite disables. */
		void DrawSuiteOptions();
		std::mutex mutex;
		std::array<Frame, kHistorySize> history{};
		size_t next = 0, count = 0;
		std::atomic_bool showOverlay = false;
		std::array<std::atomic<uint64_t>, kResetReasonNames.size()> resetCounts{};
		std::atomic<uint64_t> resetDrainMicros = 0;
		uint32_t framesSinceSummary = 0;
		static const char* Name(Outcome outcome);
		static char Code(Outcome outcome);
		/** @brief "A=copyQueued,N=noHook,..." legend for Code(), the single copy both call sites share. */
		static std::string Legend();
		static void LogFrame(const Frame& frame);
	};
}
