#pragma once

#include "VRAPI/CSinterface001.h"

// Build compatibility level reported by getBuildNumber(); tracks the sibling
// fork's revision-3 interface so already-compiled consumers gate correctly.
inline constexpr unsigned int CSBuildNumber = 8;

namespace CSPluginAPI
{
	/// @brief Returns the API object for a supported revision (0 = latest), else nullptr.
	void* GetApi(unsigned int revisionNumber);

	/// @brief Handles SKSE mod messages requesting the Community Shaders API.
	void ModMessageHandler(SKSE::MessagingInterface::Message* message);

	/// @brief Applies API setter writes staged from other plugins' threads.
	/// Render thread only, once per frame, so writes land exactly like menu edits.
	void ProcessStagedSettings();

	// This object provides access to Community Shaders' mod support API.
	struct CSInterface001 : ICSInterface001
	{
		// Must mirror ICSInterface001 virtual order exactly.
		virtual unsigned int getBuildNumber() override;

		virtual bool GetSSSEnabled() override;
		virtual void SetSSSEnabled(bool enabled) override;

		virtual bool GetSSGIEnabled() override;
		virtual void SetSSGIEnabled(bool enabled) override;

		virtual bool GetVolumetricLightingExteriorEnabled() override;
		virtual void SetVolumetricLightingExteriorEnabled(bool enabled) override;

		virtual UpscalePreset GetUpscalePreset() override;
		virtual void SetUpscalePreset(UpscalePreset preset) override;

		virtual bool GetLightLimitFixContactShadowsEnabled() override;
		virtual void SetLightLimitFixContactShadowsEnabled(bool enabled) override;

		virtual DLSSProfile GetDLSSProfile() override;
		virtual void SetDLSSProfile(DLSSProfile profile) override;

		virtual bool GetRenderAtUpscaleResEnabled() override;
		virtual void SetRenderAtUpscaleResEnabled(bool enabled) override;
		virtual bool GetRenderAtUpscaleResActive() override;
		virtual void SetVRUpscalingTransitionProfile(bool renderScaleModeEnabled, UpscalePreset preset, DLSSProfile profile) override;

		virtual UpscaleMethod GetUpscaleMethod() override;
		virtual void SetUpscaleMethod(UpscaleMethod method) override;
		virtual void SetVRUpscalingTransitionProfileForMethod(UpscaleMethod method, bool renderScaleModeEnabled, UpscalePreset preset, DLSSProfile profile) override;

		virtual uint32_t GetVRUpscalingApplyBlockReasons() override;
		virtual bool IsVRUpscalingProfileApplyAllowed() override;
	};
}  // namespace CSPluginAPI
