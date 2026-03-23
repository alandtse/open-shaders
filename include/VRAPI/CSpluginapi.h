#pragma once

#include "Features/Upscaling.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/VolumetricLighting.h"
#include "Globals.h"
#include "VRAPI/CSinterface001.h"

#include <algorithm>

inline constexpr unsigned int CSBuildNumber = 2;

namespace CSPluginAPI
{
	void* GetApi(unsigned int revisionNumber);

	void ModMessageHandler(SKSE::MessagingInterface::Message* message);

	// This object provides access to Community Shaders' mod support API version 1.
	struct CSInterface001 : ICSInterface001
	{
		virtual unsigned int getBuildNumber() override;

		virtual bool GetSSSEnabled() override;
		virtual void SetSSSEnabled(bool enabled) override;

		virtual bool GetSSGIEnabled() override;
		virtual void SetSSGIEnabled(bool enabled) override;

		virtual bool GetVolumetricLightingExteriorEnabled() override;
		virtual void SetVolumetricLightingExteriorEnabled(bool enabled) override;

		virtual DLSSMode GetDLSSMode() override;
		virtual void SetDLSSMode(DLSSMode mode) override;
	};

	namespace detail
	{
		inline bool IsValidInterfaceRequest(const SKSE::MessagingInterface::Message* message)
		{
			return message &&
			       message->type == CSMessage::kMessage_GetInterface &&
			       message->data &&
			       message->dataLen >= sizeof(CSMessage);
		}

		template <class TFlag>
		constexpr TFlag BoolToFlag(bool enabled)
		{
			return enabled ? static_cast<TFlag>(1) : static_cast<TFlag>(0);
		}

		inline bool IsValidDLSSMode(DLSSMode mode)
		{
			switch (mode) {
			case DLSSMode::kDLAA:
			case DLSSMode::kQuality:
			case DLSSMode::kBalanced:
			case DLSSMode::kPerformance:
			case DLSSMode::kUltraPerformance:
				return true;
			default:
				return false;
			}
		}
	}

	inline CSInterface001 g_interface001;

	// Constructs and returns an API of the revision number requested.
	inline void* GetApi(unsigned int revisionNumber)
	{
		// Accept revision 0 as "latest" in addition to explicit revision 1.
		if (revisionNumber != 0 && revisionNumber != CSInterfaceRevision) {
			return nullptr;
		}

		return &g_interface001;
	}

	// Handles SKSE mod messages requesting to fetch API functions from Community Shaders.
	inline void ModMessageHandler(SKSE::MessagingInterface::Message* message)
	{
		if (!detail::IsValidInterfaceRequest(message)) {
			return;
		}

		auto* csMessage = static_cast<CSMessage*>(message->data);
		csMessage->GetApiFunction = GetApi;
		logger::info("Provided Community Shaders plugin interface to {}", message->sender ? message->sender : "<unknown>");
	}

	// Fetches the version number.
	inline unsigned int CSInterface001::getBuildNumber()
	{
		return CSBuildNumber;
	}

	inline bool CSInterface001::GetSSSEnabled()
	{
		return globals::features::screenSpaceShadows.bendSettings.Enable != 0;
	}

	inline void CSInterface001::SetSSSEnabled(bool enabled)
	{
		using EnableFlag = decltype(globals::features::screenSpaceShadows.bendSettings.Enable);
		globals::features::screenSpaceShadows.bendSettings.Enable = detail::BoolToFlag<EnableFlag>(enabled);
	}

	inline bool CSInterface001::GetSSGIEnabled()
	{
		return globals::features::screenSpaceGI.settings.Enabled;
	}

	inline void CSInterface001::SetSSGIEnabled(bool enabled)
	{
		globals::features::screenSpaceGI.settings.Enabled = enabled;
	}

	inline bool CSInterface001::GetVolumetricLightingExteriorEnabled()
	{
		return globals::features::volumetricLighting.IsExteriorEnabled();
	}

	inline void CSInterface001::SetVolumetricLightingExteriorEnabled(bool enabled)
	{
		globals::features::volumetricLighting.SetExteriorEnabled(enabled);
	}

	inline DLSSMode CSInterface001::GetDLSSMode()
	{
		const uint32_t clampedMode = std::min(globals::features::upscaling.settings.qualityMode, 4u);
		return static_cast<DLSSMode>(clampedMode);
	}

	inline void CSInterface001::SetDLSSMode(DLSSMode mode)
	{
		if (!detail::IsValidDLSSMode(mode)) {
			logger::warn("[CS API] Ignoring invalid DLSS mode value {}", static_cast<uint32_t>(mode));
			return;
		}

		globals::features::upscaling.settings.qualityMode = static_cast<uint32_t>(mode);
	}
}  // namespace CSPluginAPI
