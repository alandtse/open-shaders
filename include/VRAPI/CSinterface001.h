#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <cstdint>

namespace CSPluginAPI
{
	// Returns an ICSInterface001 object compatible with the API shown below.
	// This should only be called after SKSE sends kMessage_PostLoad to your plugin.
	constexpr const auto CSPluginName = "CommunityShaders";
	inline constexpr uint32_t CSInterfaceMessageType = 0x43534150;  // "CSAP"
	inline constexpr unsigned int CSInterfaceRevision = 1;

	// A message used to fetch Community Shaders' interface.
	struct CSMessage
	{
		enum : uint32_t
		{
			kMessage_GetInterface = CSInterfaceMessageType
		};
		void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
	};

	struct ICSInterface001;
	ICSInterface001* GetCSInterface001();

	enum class DLSSMode : uint32_t
	{
		kDLAA = 0,
		kQuality = 1,
		kBalanced = 2,
		kPerformance = 3,
		kUltraPerformance = 4
	};

	// This object provides access to Community Shaders' mod support API.
	struct ICSInterface001
	{
		virtual unsigned int getBuildNumber() = 0;

		// SSS here means Screen Space Shadows.
		virtual bool GetSSSEnabled() = 0;
		virtual void SetSSSEnabled(bool enabled) = 0;

		virtual bool GetSSGIEnabled() = 0;
		virtual void SetSSGIEnabled(bool enabled) = 0;

		virtual bool GetVolumetricLightingExteriorEnabled() = 0;
		virtual void SetVolumetricLightingExteriorEnabled(bool enabled) = 0;

		// DLSS quality mode only (does not expose profile/reflex controls).
		virtual DLSSMode GetDLSSMode() = 0;
		virtual void SetDLSSMode(DLSSMode mode) = 0;
	};
}  // namespace CSPluginAPI

extern CSPluginAPI::ICSInterface001* g_CSInterface;
