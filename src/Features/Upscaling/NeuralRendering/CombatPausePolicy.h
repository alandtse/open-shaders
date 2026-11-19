#pragma once

#include <cstdint>
#include <optional>

namespace NR
{
	/** @brief Continuous peaceful gameplay required before NR resumes. */
	inline constexpr std::uint64_t kCombatResumeDelayMs = 3000;

	/** @brief Combat admission state, independent of NR's other runtime blockers. */
	enum class CombatPausePhase : std::uint8_t
	{
		Disabled,
		Ready,
		Combat,
		Cooldown,
		Unavailable
	};

	/** @brief One immutable combat decision shared by every hook and eye in a frame. */
	struct CombatPauseSnapshot
	{
		CombatPausePhase phase = CombatPausePhase::Disabled;
		bool sampled = false;
		std::uint32_t frame = 0;
		std::uint64_t remainingMs = 0;

		/** @brief Whether NR must leave the ordinary rendering path in control. */
		[[nodiscard]] constexpr bool IsPaused() const noexcept
		{
			return phase != CombatPausePhase::Disabled && phase != CombatPausePhase::Ready;
		}
	};

	/** @brief Stable phase identifier for devbench and diagnostics. */
	[[nodiscard]] constexpr const char* CombatPausePhaseName(CombatPausePhase phase) noexcept
	{
		switch (phase) {
		case CombatPausePhase::Disabled:
			return "disabled";
		case CombatPausePhase::Ready:
			return "ready";
		case CombatPausePhase::Combat:
			return "combat";
		case CombatPausePhase::Cooldown:
			return "cooldown";
		case CombatPausePhase::Unavailable:
			return "unavailable";
		default:
			return "unknown";
		}
	}

	/** @brief Frame-latched combat admission; the caller serializes access. */
	class CombatPausePolicy
	{
	public:
		/** @brief Whether this frame already has its shared decision. */
		[[nodiscard]] constexpr bool HasFrame(std::uint32_t frame) const noexcept
		{
			return snapshot.sampled && snapshot.frame == frame;
		}

		/** @brief The most recently sampled decision. */
		[[nodiscard]] constexpr CombatPauseSnapshot GetSnapshot() const noexcept { return snapshot; }

		/** @brief Paused gameplay interrupts recovery without starting a new combat pause. */
		constexpr CombatPauseSnapshot Update(std::uint32_t frame, bool enabled,
			std::uint64_t nowMs, std::optional<bool> inCombat, bool gamePaused = false) noexcept
		{
			if (HasFrame(frame))
				return snapshot;
			if (snapshot.sampled && (frame != snapshot.frame + 1u || nowMs < lastSampleMs))
				snapshot.phase = CombatPausePhase::Unavailable;
			snapshot.sampled = true;
			snapshot.frame = frame;
			snapshot.remainingMs = 0;
			lastSampleMs = nowMs;
			if (!enabled) {
				snapshot.phase = CombatPausePhase::Disabled;
			} else if (!inCombat) {
				snapshot.phase = CombatPausePhase::Unavailable;
			} else if (*inCombat) {
				snapshot.phase = CombatPausePhase::Combat;
			} else if (gamePaused && snapshot.IsPaused()) {
				snapshot.phase = CombatPausePhase::Unavailable;
			} else if (snapshot.IsPaused()) {
				if (snapshot.phase != CombatPausePhase::Cooldown)
					cooldownStartedMs = nowMs;
				const auto elapsed = nowMs - cooldownStartedMs;
				if (elapsed >= kCombatResumeDelayMs) {
					snapshot.phase = CombatPausePhase::Ready;
				} else {
					snapshot.phase = CombatPausePhase::Cooldown;
					snapshot.remainingMs = kCombatResumeDelayMs - elapsed;
				}
			} else {
				snapshot.phase = CombatPausePhase::Ready;
			}
			return snapshot;
		}

	private:
		CombatPauseSnapshot snapshot;
		std::uint64_t lastSampleMs = 0;
		std::uint64_t cooldownStartedMs = 0;
	};
}
