#pragma once

#include <cstdint>

namespace NR
{
	/** @brief What one NR hook call must do, in the order the checks settle it. */
	enum class FrameAction
	{
		ReleasePassResources,  ///< Disabled: free the pass resources, keep the device and NGX instance.
		SkipNoWorld,           ///< No world rendered yet; the runtime starts on the first world frame.
		RebuildThenRun,        ///< A retry was requested after a latched failure: rebuild, then run.
		SkipLatched,           ///< Failed, with no retry pending.
		SkipDuplicate,         ///< A second hook call for a frame NR already handled.
		InitializeThenRun,     ///< First world frame: initialize the runtime, then run.
		Run                    ///< Evaluate normally.
	};

	/** @brief The hook's scheduling inputs, mirroring the per-runtime state it reads. */
	struct FrameInputs
	{
		bool enabled = false;
		bool worldRendered = false;
		bool failed = false;
		bool retryRequested = false;
		bool ready = false;
		uint32_t lastFrame = UINT32_MAX;
		uint32_t frameCount = 0;
	};

	/** @brief Settles what this hook call must do. */
	inline FrameAction DecideFrame(const FrameInputs& inputs)
	{
		if (!inputs.enabled)
			return FrameAction::ReleasePassResources;
		if (!inputs.worldRendered)
			return FrameAction::SkipNoWorld;
		if (inputs.failed && inputs.retryRequested)
			return FrameAction::RebuildThenRun;
		if (inputs.failed)
			return FrameAction::SkipLatched;
		if (inputs.lastFrame == inputs.frameCount)
			return FrameAction::SkipDuplicate;
		if (!inputs.ready)
			return FrameAction::InitializeThenRun;
		return FrameAction::Run;
	}

	/** @brief Whether a failure drops the runtime before latching, so a retry rebuilds it. */
	enum class FailureAction
	{
		Latch,             ///< Keep the runtime; a retry can reuse it.
		TeardownThenLatch  ///< Device removed, so the runtime cannot be reused.
	};

	/** @brief A removed device can never be reused; every other failure keeps the runtime. */
	inline FailureAction OnFailure(bool deviceRemoved)
	{
		return deviceRemoved ? FailureAction::TeardownThenLatch : FailureAction::Latch;
	}
}
