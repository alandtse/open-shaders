#pragma once

#include <cstdint>
#include <string_view>

namespace NR
{
	/** @brief The immutable feature dimensions shared by startup creation and the first evaluation. */
	struct PrewarmExtent
	{
		uint32_t width = 0, height = 0, eyes = 0;
		bool operator==(const PrewarmExtent&) const = default;
		[[nodiscard]] bool Valid() const { return width && height && (eyes == 1 || eyes == 2); }
	};

	/** @brief One startup frame's qualified inputs; no world guide resources are needed. */
	struct PrewarmInputs
	{
		bool requested = false, enabled = false, startupMenu = false, worldRendered = false;
		bool deviceReady = false, runtimeReady = false, runtimeFailed = false, passResources = false;
		bool materialStrength = false;
		bool configuredExtentReady = true;
		uint32_t frame = 0;
	};

	/** @brief At most one synchronous startup operation is permitted per presented frame. */
	enum class PrewarmStep : uint8_t
	{
		None,
		Initialize,
		Eye0,
		Eye1
	};

	/** @brief Startup-only scheduler; a failed attempt never disables ordinary lazy initialization. */
	class StartupPrewarm
	{
	public:
		PrewarmExtent extent;
		uint32_t preparedEyes = 0;
		bool failed = false, usedInWorld = false, reused = false;
		std::string_view reason = "disabled";

		/** @brief Starts a fresh scheduling attempt, without releasing runtime resources. */
		void Reset() { *this = StartupPrewarm{}; }

		/** @brief Invalidates partial progress when the configured extent changes. */
		void SetExtent(PrewarmExtent value)
		{
			if (extent == value)
				return;
			extent = value;
			preparedEyes = 0;
			reused = false;
		}

		/** @brief True if startup created at least one eye for this exact unprotected configuration. */
		[[nodiscard]] bool Matches(PrewarmExtent value) const { return !failed && preparedEyes && extent == value; }

		/** @brief Selects the next operation, preserving partial progress through temporary gate failures. */
		PrewarmStep Next(const PrewarmInputs& input)
		{
			if (!input.requested)
				return Block("disabled");
			if (!input.enabled)
				return Block("nr-disabled");
			if (failed)
				return Block("failed");
			if (input.runtimeFailed)
				return Block("runtime-failed");
			if (usedInWorld || input.passResources)
				return Block("already-running");
			if (input.worldRendered)
				return Block("world-rendered");
			if (!input.startupMenu)
				return Block("outside-startup-menu");
			if (!input.deviceReady)
				return Block("device-unavailable");
			if (hasStepFrame && lastStepFrame == input.frame)
				return Block("frame-step-complete");
			if (!input.runtimeReady)
				return Take(PrewarmStep::Initialize, input.frame);
			if (!input.configuredExtentReady || !extent.Valid())
				return Block("extent-unavailable");
			if (input.materialStrength)
				return Block("material-guides-required");
			if (preparedEyes >= extent.eyes)
				return Block("ready");
			return Take(preparedEyes == 0 ? PrewarmStep::Eye0 : PrewarmStep::Eye1, input.frame);
		}

		/** @brief Advances only after the selected operation completed successfully. */
		void Complete(PrewarmStep step)
		{
			if (step == PrewarmStep::Eye0 || step == PrewarmStep::Eye1)
				++preparedEyes;
			reason = extent.Valid() && preparedEyes == extent.eyes ? "ready" : "staged";
		}

	private:
		uint32_t lastStepFrame = UINT32_MAX;
		bool hasStepFrame = false;
		PrewarmStep Block(std::string_view value)
		{
			reason = value;
			return PrewarmStep::None;
		}
		PrewarmStep Take(PrewarmStep step, uint32_t frame)
		{
			lastStepFrame = frame;
			hasStepFrame = true;
			reason = step == PrewarmStep::Initialize ? "initializing" : "preparing-eye";
			return step;
		}
	};
}
