#pragma once

#include <cstdint>

namespace NR
{
	/** @brief What one NR hook call must do, in the order the checks settle it. */
	enum class FrameAction
	{
		ReleasePassResources,  ///< Disabled: free the pass resources, keep the device and NGX instance.
		Suspend,               ///< Gated off this frame: skip dispatch and composite, keep resources, runtime and NGX features.
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
		/** @brief The user has NR switched on. */
		bool enabled = false;
		/** @brief Dialogue-only gating holds this frame: the pass is suspended but stays initialized. */
		bool suspended = false;
		/** @brief The engine drew a world this frame. */
		bool worldRendered = false;
		/** @brief A failure is latched on the current runtime. */
		bool failed = false;
		/** @brief A retry was queued while that failure was latched. */
		bool retryRequested = false;
		/** @brief The NR runtime is initialized. */
		bool ready = false;
		/** @brief Engine frame this hook last handled; UINT32_MAX before the first. */
		uint32_t lastFrame = UINT32_MAX;
		/** @brief Engine frame of this call. */
		uint32_t frameCount = 0;
	};

	/** @brief Settles what this hook call must do. */
	inline FrameAction DecideFrame(const FrameInputs& inputs)
	{
		if (!inputs.enabled)
			return FrameAction::ReleasePassResources;
		if (inputs.suspended)
			return FrameAction::Suspend;
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

	/**
	 * @brief Whether dialogue-only gating holds the pass this frame: with it on, NR evaluates only
	 *        while dialogue is open, so a closed menu suspends the pass without tearing it down.
	 */
	inline bool DialogueGate(bool dialogueOnly, bool dialogueOpen)
	{
		return dialogueOnly && !dialogueOpen;
	}

	/**
	 * @brief Whether this frame leaves a suspension, the one transition that must invalidate history;
	 *        the caller keeps the previous frame's suspended state in the reference, which is updated here.
	 */
	inline bool ResumesFromSuspend(bool suspended, bool& wasSuspended)
	{
		const bool resumes = wasSuspended && !suspended;
		wasSuspended = suspended;
		return resumes;
	}

	/** @brief A removed device can never be reused; every other failure keeps the runtime. */
	inline FailureAction OnFailure(bool deviceRemoved)
	{
		return deviceRemoved ? FailureAction::TeardownThenLatch : FailureAction::Latch;
	}
}
