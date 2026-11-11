#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace NR::Cadence
{
	/** @brief Frame marker for an eye that has not run the model yet. */
	inline constexpr uint64_t kNoModelFrame = UINT64_MAX;

	/** @brief What the stagger applies to this frame. */
	struct Inputs
	{
		/** @brief The developer setting is on; the caller folds developer mode and the VR check in. */
		bool enabled = false;
		/** @brief Two eyes are rendered this frame. */
		bool vr = false;
		/** @brief Eyes the pass evaluates; only a two-eye frame can stagger. */
		uint32_t eyeCount = 0;
		/** @brief Engine frame; its parity assigns the model to one eye. */
		uint64_t frame = 0;
		/**
		 * @brief A gain stored now may not be reprojected: both eyes evaluate and the reuse state restarts.
		 *        Set for a history reset, a resource or crop change, a material-strength change, a frame
		 *        generation transition and the diagnostics that isolate history.
		 */
		bool invalidate = false;
		/** @brief A one-view frame runs the model once per this many frames; 1 runs it every frame. */
		uint32_t interval = 1;
		/** @brief The camera moved or turned past the flat limits this frame, so a carried gain would be stale. */
		bool highMotion = false;
	};

	/** @brief Per-eye memory the cadence keeps between frames. */
	struct State
	{
		/** @brief Frame each eye last ran the model on. */
		std::array<uint64_t, 2> lastModelFrame{ kNoModelFrame, kNoModelFrame };
		/**
		 * @brief Whether the eye has a stored gain a later frame may reuse.
		 *        The caller sets it after the gain store succeeds; Decide clears it whenever it assigns
		 *        that eye an evaluation, so a gain is never reused before it has been stored.
		 */
		std::array<bool, 2> valid{};
	};

	/** @brief Which eyes run the model this frame; the others reuse their stored gain. */
	struct Decision
	{
		std::array<bool, 2> evaluate{ true, true };
	};

	/**
	 * @brief Assigns one frame's model work to one eye.
	 *        A one-view frame with an interval above 1 runs the model on every interval-th frame, so a reused
	 *        gain is at most interval - 1 frames old, and a high-motion frame runs the model off its turn. Without a two-eye frame every eye evaluates every frame. Otherwise the even frames evaluate
	 *        eye 0 and the odd frames eye 1, and an eye whose last gain is missing or invalidated still
	 *        evaluates off its turn, so a reused gain is never older than one frame.
	 */
	inline Decision Decide(const Inputs& inputs, State& state)
	{
		Decision decision;
		if (inputs.enabled && !inputs.vr && inputs.eyeCount == 1 && inputs.interval > 1 && !inputs.invalidate) {
			const bool due = inputs.frame % inputs.interval == 0;
			decision.evaluate[0] = due || !state.valid[0] || inputs.highMotion;
			if (decision.evaluate[0]) {
				state.lastModelFrame[0] = inputs.frame;
				state.valid[0] = false;
			}
			return decision;
		}
		const bool stagger = inputs.enabled && inputs.vr && inputs.eyeCount == 2;
		if (!stagger || inputs.invalidate) {
			for (uint32_t eye = 0; eye < 2; ++eye) {
				decision.evaluate[eye] = true;
				state.lastModelFrame[eye] = inputs.frame;
				state.valid[eye] = false;
			}
			return decision;
		}
		for (uint32_t eye = 0; eye < 2; ++eye) {
			const bool due = (inputs.frame & 1u) == eye;
			const bool evaluate = due || !state.valid[eye];
			decision.evaluate[eye] = evaluate;
			if (evaluate) {
				state.lastModelFrame[eye] = inputs.frame;
				state.valid[eye] = false;
			}
		}
		return decision;
	}

	/** @brief Frames since the eye's last model run; the model is told to expect this much motion and time. */
	inline float GapScale(uint64_t frame, uint64_t lastModelFrame)
	{
		return lastModelFrame == kNoModelFrame || frame <= lastModelFrame ? 1.0f : static_cast<float>(frame - lastModelFrame);
	}

	/**
	 * @brief Whether the camera turned or moved far enough since the last frame that a carried gain would be stale.
	 *        A limit of 0 disables that test, and a non-finite sample counts as high motion so the model runs.
	 * @param directionDot Dot product of the camera's forward direction this frame and last frame.
	 * @param distance World units the camera moved since last frame.
	 */
	inline bool IsHighMotion(float directionDot, float distance, float maxTurnDegrees, float maxMoveUnits)
	{
		if (!std::isfinite(directionDot) || !std::isfinite(distance))
			return true;
		constexpr float kRadiansToDegrees = 57.2957795f;
		const float turnDegrees = std::acos(std::clamp(directionDot, -1.0f, 1.0f)) * kRadiansToDegrees;
		return (maxTurnDegrees > 0.0f && turnDegrees > maxTurnDegrees) || (maxMoveUnits > 0.0f && distance > maxMoveUnits);
	}
}
