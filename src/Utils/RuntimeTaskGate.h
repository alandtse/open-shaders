#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace Util
{
	/** @brief Lifetime-safe access to the existing runtime task dispatcher.
	 * Fence callbacks may outlive the feature, but cannot call a closed dispatcher. */
	class RuntimeTaskGate
	{
	public:
		using Task = std::function<void()>;
		using Submitter = std::function<bool(Task)>;
		using PrioritySubmitter = std::function<bool(Task, bool)>;

		explicit RuntimeTaskGate(Submitter submit) : submitter([submit = std::move(submit)](Task task, bool) { return submit && submit(std::move(task)); }) {}
		explicit RuntimeTaskGate(PrioritySubmitter submit) : submitter(std::move(submit)) {}

		bool TrySubmit(Task task, bool menuPriority = false)
		{
			std::scoped_lock lock(mutex);
			return submitter && task && submitter(std::move(task), menuPriority);
		}

		void Close()
		{
			std::scoped_lock lock(mutex);
			submitter = {};
		}

	private:
		std::mutex mutex;
		PrioritySubmitter submitter;
	};

	// Render-demanded shaders win during gameplay. Menu preparation may compete for a free slot.
	// One runtime task may occupy
	// the shared budget, including across a scene reset or feature re-enable.
	constexpr bool CanAdmitRuntimeTask(bool closed, bool runtimeInFlight, bool demandedShaders,
		uint32_t inFlight, int32_t limit, bool menuPriority = false)
	{
		return !closed && !runtimeInFlight && (!demandedShaders || menuPriority) && limit > 0 &&
		       inFlight < static_cast<uint32_t>(limit);
	}
}
