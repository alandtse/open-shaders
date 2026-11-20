#pragma once

#include <mutex>
#include <optional>

namespace NR
{
	/** @brief Main-thread player and menu state, without retained engine pointers. */
	struct CombatStateSnapshot
	{
		std::optional<bool> inCombat;
		bool gamePaused = true;
	};

	/** @brief Publishes one coherent main-thread sample for rendering to consume. */
	class CombatState
	{
	public:
		/** @brief Replaces the sample, including unavailable player or world state. */
		void Publish(std::optional<bool> inCombat, bool gamePaused)
		{
			std::scoped_lock lock(mutex);
			snapshot = { inCombat, gamePaused };
		}

		/** @brief Returns a value copy that remains stable across later publications. */
		[[nodiscard]] CombatStateSnapshot GetSnapshot() const
		{
			std::scoped_lock lock(mutex);
			return snapshot;
		}

	private:
		mutable std::mutex mutex;
		CombatStateSnapshot snapshot;
	};
}
