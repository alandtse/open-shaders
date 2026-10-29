#pragma once

#include <cstdint>

namespace OpenWind
{
	inline constexpr uint32_t kUIRevision = 1;

	/** Consumes UTF-8 JSON during the call without transferring allocation ownership. */
	using UIResponse = void (*)(void* context, const char* response) noexcept;

	/** Menu-thread settings, control descriptions, and commands, separate from wind sampling and rendering. */
	struct UIAPI
	{
		uint32_t structSize = sizeof(UIAPI);
		uint32_t revision = kUIRevision;
		/** Copies settings JSON; a null buffer queries the required size including its terminator. */
		bool (*GetSettings)(char* buffer, uint32_t* inoutSize) noexcept {};
		/** Applies a settings document through the same handler used by saved configurations. */
		bool (*LoadSettings)(const char* json) noexcept {};
		/** Imports renderer settings only when Open Wind has no standalone configuration. */
		bool (*ImportSettings)(const char* json) noexcept {};
		/** Restores the default wind and effect settings. */
		void (*RestoreDefaults)() noexcept {};
		/** Resets an effect, queries availability, or spawns a debug transient. */
		bool (*RunAction)(const char* json) noexcept {};
		/** Executes one authoring or integration request and delivers its JSON response once. */
		bool (*Command)(const char* request, void* context, UIResponse response) noexcept {};
		/** Copies a section's control-description JSON; size includes the terminating null. */
		bool (*GetControls)(const char* section, char* buffer, uint32_t* inoutSize) noexcept {};
	};

	/** Returns the current UI interface regardless of the requested revision. */
	using RequestUIAPI = const UIAPI* (*)(uint32_t revision) noexcept;
}
