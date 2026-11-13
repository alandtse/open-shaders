#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace DX12Native
{
	/** @brief Persistent, per-scene CPU progress. Never holds GPU or Skyrim pointers.
     *
     * This tracks attempts rather than only successes, so absent archive assets
     * cannot repeatedly consume every subsequent batch. A new scene, revision,
     * or source-mode selection must reset it.
     */
	class RuntimeTextureProgress
	{
	public:
		void Reset()
		{
			attempted.clear();
			batches = 0;
			successful = 0;
			failed = 0;
		}

		void Record(const std::vector<std::string>& a_attempts,
			const std::unordered_set<std::string>& a_resident)
		{
			if (a_attempts.empty())
				return;
			++batches;
			for (const auto& path : a_attempts) {
				if (!attempted.insert(path).second)
					continue;
				if (a_resident.contains(path))
					++successful;
				else
					++failed;
			}
		}

		const std::unordered_set<std::string>& Attempted() const { return attempted; }
		size_t AttemptedCount() const { return attempted.size(); }
		uint32_t Batches() const { return batches; }
		uint32_t Successful() const { return successful; }
		uint32_t Failed() const { return failed; }

	private:
		std::unordered_set<std::string> attempted;
		uint32_t batches = 0, successful = 0, failed = 0;
	};
}
