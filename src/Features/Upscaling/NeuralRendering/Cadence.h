#pragma once

#include <array>
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
	 *        Without a two-eye frame every eye evaluates every frame. Otherwise the even frames evaluate
	 *        eye 0 and the odd frames eye 1, and an eye whose last gain is missing or invalidated still
	 *        evaluates off its turn, so a reused gain is never older than one frame.
	 */
	inline Decision Decide(const Inputs& inputs, State& state)
	{
		Decision decision;
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
}
