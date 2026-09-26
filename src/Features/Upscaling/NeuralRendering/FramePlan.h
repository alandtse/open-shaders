#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace NR
{
	/** @brief What the frame path does this frame; the pure decision DrawBeforeUpscaling applies. */
	enum class FrameAction : uint8_t
	{
		kSkipDisabled,     ///< Neural rendering is switched off.
		kSkipNoWorld,      ///< No world was rendered this frame (main menu or loading screen).
		kSkipUnsupported,  ///< The renderer's adapter is not an NVIDIA one, so Feature 18 has no device.
		kSkipLatched,      ///< A failure latched the pass off and no retry is queued.
		kRetry,            ///< A retry is queued: rebuild the pass before running it.
		kRun               ///< Run the pass.
	};

	/** @brief Per-frame facts the decision reads; all are known before any GPU work starts. */
	struct FrameInputs
	{
		bool enabled = false;
		bool worldRendered = false;
		bool adapterSupported = false;
		bool failed = false;          ///< The pass latched a failure; only an explicit retry clears it.
		bool retryRequested = false;  ///< A retry is queued; it overrides the latch.
	};

	/** @brief What the last frame did, reported by the status query. */
	enum class Outcome : uint8_t
	{
		kDisabled,            ///< Switched off (the shipped default).
		kNoWorld,             ///< A menu or loading frame: the color there is not the scene.
		kUnsupportedAdapter,  ///< No NVIDIA adapter, so no NR device can exist.
		kInitializing,        ///< Enabled, but the runtime layer has not finished initializing yet.
		kFailed,              ///< The pass latched a failure and the color is untouched.
		kApplied              ///< The pass ran this frame and composited into kMAIN.
	};

	/** @brief Diagnostics label for the frame decision. */
	[[nodiscard]] constexpr std::string_view FrameActionName(FrameAction a_action)
	{
		switch (a_action) {
		case FrameAction::kSkipDisabled:
			return "SkipDisabled";
		case FrameAction::kSkipNoWorld:
			return "SkipNoWorld";
		case FrameAction::kSkipUnsupported:
			return "SkipUnsupportedAdapter";
		case FrameAction::kSkipLatched:
			return "SkipLatched";
		case FrameAction::kRetry:
			return "Retry";
		case FrameAction::kRun:
			return "Run";
		}
		return "Unknown";
	}

	/** @brief Diagnostics label for a frame outcome. */
	[[nodiscard]] constexpr std::string_view OutcomeName(Outcome a_outcome)
	{
		switch (a_outcome) {
		case Outcome::kDisabled:
			return "Disabled";
		case Outcome::kNoWorld:
			return "NoWorld";
		case Outcome::kUnsupportedAdapter:
			return "UnsupportedAdapter";
		case Outcome::kInitializing:
			return "Initializing";
		case Outcome::kFailed:
			return "Failed";
		case Outcome::kApplied:
			return "Applied";
		}
		return "Unknown";
	}

	/**
	 * @brief The frame decision, in the order that keeps the status meaningful: a queued retry is
	 *        checked before the latch it clears, so a retry is never silently swallowed.
	 */
	[[nodiscard]] constexpr FrameAction DecideFrameAction(const FrameInputs& a_inputs)
	{
		if (!a_inputs.enabled)
			return FrameAction::kSkipDisabled;
		if (!a_inputs.worldRendered)
			return FrameAction::kSkipNoWorld;
		if (!a_inputs.adapterSupported)
			return FrameAction::kSkipUnsupported;
		if (a_inputs.retryRequested)
			return FrameAction::kRetry;
		if (a_inputs.failed)
			return FrameAction::kSkipLatched;
		return FrameAction::kRun;
	}

	/** @brief Where the Prepare pass takes the exposure it multiplies the linear source by. */
	enum class ExposureSource : uint32_t
	{
		kScene = 0,   ///< The scene exposure a loaded feature publishes this frame; the shipped default.
		kLocal = 1,   ///< The pass's own GPU estimate over both eyes, so left and right share one value.
		kScalar = 2,  ///< A fixed 1.0, leaving Feature 18's own auto-exposure to normalize the proxy.
		kCount
	};

	/**
	 * @brief The arm a frame actually uses. A requested scene exposure falls back to the local
	 *        estimate when no loaded feature publishes one (PostProcessing absent, bypassed, or
	 *        Effects11 owns the tonemap), which is the ruling's required fallback.
	 */
	[[nodiscard]] constexpr ExposureSource ResolveExposureSource(ExposureSource a_requested, bool a_sceneAvailable)
	{
		if (a_requested == ExposureSource::kScene && !a_sceneAvailable)
			return ExposureSource::kLocal;
		return a_requested;
	}

	/**
	 * @brief Whether this frame must start a new temporal history.
	 *        A requested reset, the pass's first frame and a frame the pass skipped all do; every
	 *        other frame continues the history the previous one left.
	 */
	[[nodiscard]] constexpr bool NeedsHistoryReset(bool a_requested, uint32_t a_lastFrame, uint32_t a_frameCount)
	{
		return a_requested || a_lastFrame == UINT32_MAX || a_lastFrame + 1 != a_frameCount;
	}

	/** @brief Group grid of the exposure reduction, bounded so its partial buffer has a fixed size. */
	struct ExposureGrid
	{
		uint32_t columns = 0, rows = 0;

		[[nodiscard]] constexpr uint32_t Count() const { return columns * rows; }
		[[nodiscard]] constexpr bool Empty() const { return columns == 0 || rows == 0; }
	};

	/** @brief Pixels one exposure group covers per axis, so an 8x8 group strides a 64x64 tile. */
	inline constexpr uint32_t kExposureTilePixels = 64;
	/** @brief Axis cap of the exposure grid. Both axes at the cap is the partial-buffer capacity. */
	inline constexpr uint32_t kMaxExposureAxis = 64;
	/** @brief Partials the exposure reduction may write; the grid caps below it by construction. */
	inline constexpr uint32_t kMaxExposureGroups = kMaxExposureAxis * kMaxExposureAxis;

	/**
	 * @brief Grid for a source extent; empty when the extent is, so the frame falls back to a
	 *        scalar exposure instead of dispatching an empty reduction.
	 */
	[[nodiscard]] constexpr ExposureGrid ComputeExposureGrid(uint32_t a_sourceWidth, uint32_t a_sourceHeight)
	{
		if (!a_sourceWidth || !a_sourceHeight)
			return {};
		ExposureGrid grid;
		grid.columns = std::clamp((a_sourceWidth + kExposureTilePixels - 1) / kExposureTilePixels, 1u, kMaxExposureAxis);
		grid.rows = std::clamp((a_sourceHeight + kExposureTilePixels - 1) / kExposureTilePixels, 1u, kMaxExposureAxis);
		return grid;
	}

	static_assert(kMaxExposureAxis * kMaxExposureAxis == kMaxExposureGroups);

	/**
	 * @brief Per-eye width of the render rect. One expression for both the NR call site and the
	 *        shared-encoder site in Upscale(), which would otherwise drift apart.
	 */
	[[nodiscard]] constexpr uint32_t EyeRenderWidth(float a_renderWidth, uint32_t a_eyeCount)
	{
		return a_eyeCount ? static_cast<uint32_t>(a_renderWidth / static_cast<float>(a_eyeCount)) : 0u;
	}
}
