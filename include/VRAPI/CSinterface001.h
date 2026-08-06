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
	inline constexpr unsigned int CSInterfaceRevision001 = 1;
	inline constexpr unsigned int CSInterfaceRevision002 = 2;
	inline constexpr unsigned int CSInterfaceRevision003 = 3;
	inline constexpr unsigned int CSInterfaceRevision = CSInterfaceRevision003;
	// Guidance for VR transition controllers that hide render-scale relatches
	// behind a game fade. These constants are advisory only and do not change
	// the ABI; Community Shaders does not drive Game.FadeOutGame itself.
	inline constexpr float CSVRRenderScaleTransitionFadeOutSeconds = 1.0f;
	inline constexpr float CSVRRenderScaleTransitionBlackHoldAfterProfileSeconds = 6.0f;
	inline constexpr float CSVRRenderScaleTransitionFadeInSeconds = 1.0f;

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

	// Shared upscaler render-scale presets for DLSS, FSR 3.1.5, and runtime FSR4.
	enum class UpscalePreset : uint32_t
	{
		// Values 0-4 are kept stable for existing compiled API users.
		kNativeAA = 0,      // Native render scale: DLAA for DLSS, Native AA for FSR/FSR4.
		kDLAA = kNativeAA,  // Legacy DLSS-specific name for the native-scale preset.
		kQuality = 1,
		kBalanced = 2,
		kPerformance = 3,
		kUltraPerformance = 4,
		kHoshipa = 5,
		kUltraQuality = 6
	};

	// Legacy type name kept for source compatibility. It is the same enum type.
	using DLSSMode = UpscalePreset;

	enum class DLSSProfile : uint32_t
	{
		kJ = 0,
		kK = 1,
		kL = 2,
		kM = 3,
		kF = 4
	};

	enum class UpscaleMethod : uint32_t
	{
		kNone = 0,
		kTAA = 1,
		kFSR = 2,
		kDLSS = 3
	};

	enum class VRUpscalingApplyBlockReason : uint32_t
	{
		kNone = 0,
		kRaceSexMenu = 1u << 0,
		kRaceSexStartupTail = 1u << 1,
		kLoadingMenu = 1u << 2,
		kRelatchPending = 1u << 3,
		kTransitionPending = 1u << 4
	};

	// This object provides access to Community Shaders' mod support API.
	struct ICSInterface001
	{
		// ABI note: keep virtual methods append-only. Inserting new virtuals before
		// existing entries changes vtable slots for already-compiled consumers.
		virtual unsigned int getBuildNumber() = 0;

		// SSS here means Screen Space Shadows.
		virtual bool GetSSSEnabled() = 0;
		virtual void SetSSSEnabled(bool enabled) = 0;

		virtual bool GetSSGIEnabled() = 0;
		virtual void SetSSGIEnabled(bool enabled) = 0;

		virtual bool GetVolumetricLightingExteriorEnabled() = 0;
		virtual void SetVolumetricLightingExteriorEnabled(bool enabled) = 0;

		// Controls the shared DLSS/FSR/FSR4 upscaler preset.
		virtual UpscalePreset GetUpscalePreset() = 0;
		virtual void SetUpscalePreset(UpscalePreset preset) = 0;

		virtual bool GetLightLimitFixContactShadowsEnabled() = 0;
		virtual void SetLightLimitFixContactShadowsEnabled(bool enabled) = 0;

		// DLSS profile selection only. This does not affect FSR 3.1.5 or FSR4.
		virtual DLSSProfile GetDLSSProfile() = 0;
		virtual void SetDLSSProfile(DLSSProfile profile) = 0;

		// VR only. Legacy compatibility names for Community Shaders' Render Scale Mode request.
		// Runtime activation can require a render-target relatch during a loading transition.
		virtual bool GetRenderAtUpscaleResEnabled() = 0;
		virtual void SetRenderAtUpscaleResEnabled(bool enabled) = 0;
		virtual bool GetRenderAtUpscaleResActive() = 0;

		// Legacy convenience for interior/exterior transition controllers. On
		// DLSS-capable systems this stages DLSS, render-scale path, shared
		// preset, and DLSS profile together. FSR-specific callers should use
		// SetVRUpscalingTransitionProfileForMethod in revision 2.
		virtual void SetVRUpscalingTransitionProfile(bool renderScaleModeEnabled, UpscalePreset preset, DLSSProfile profile) = 0;

		// Revision 2. Explicit upscaler method control for callers that must
		// distinguish DLSS from FSR/FSR4 instead of relying on current CS settings.
		virtual UpscaleMethod GetUpscaleMethod() = 0;
		virtual void SetUpscaleMethod(UpscaleMethod method) = 0;
		virtual void SetVRUpscalingTransitionProfileForMethod(UpscaleMethod method, bool renderScaleModeEnabled, UpscalePreset preset, DLSSProfile profile) = 0;

		// Revision 3. External transition controllers should query this before
		// applying VR upscaling profiles. Non-zero block reasons mean the caller
		// should buffer its latest desired profile and try again later.
		virtual uint32_t GetVRUpscalingApplyBlockReasons() = 0;
		virtual bool IsVRUpscalingProfileApplyAllowed() = 0;
	};
}  // namespace CSPluginAPI

extern CSPluginAPI::ICSInterface001* g_CSInterface;
