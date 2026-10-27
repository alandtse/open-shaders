#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.pluginapi. {brand} is expanded at registration. */
	inline constexpr const char* kPluginApiTool =
		R"({"description":")"
		R"(Calls a method on the actual )"
		R"(SKSE ICSInterface001 plugin ABI (see API.md) -- the same in-process interface a third-party plugin gets from the CSAP message handshake, not a re-implementation. Exercises the real vtable, )"
		R"(including the internal staging/apply path (StagePatch -> ProcessStagedSettings, )"
		R"(applied on the render thread next frame) that openshaders.feature's set action bypasses by writing settings directly. method (top-level, not nested under params): one of getBuildNumber, )"
		R"(GetSSSEnabled, )"
		R"(SetSSSEnabled, GetSSGIEnabled, )"
		R"(SetSSGIEnabled, GetVolumetricLightingExteriorEnabled, )"
		R"(SetVolumetricLightingExteriorEnabled, GetUpscalePreset, )"
		R"(SetUpscalePreset, GetLightLimitFixContactShadowsEnabled, )"
		R"(SetLightLimitFixContactShadowsEnabled, GetDLSSProfile, )"
		R"(SetDLSSProfile, GetRenderAtUpscaleResEnabled, )"
		R"(SetRenderAtUpscaleResEnabled, GetRenderAtUpscaleResActive, )"
		R"(SetVRUpscalingTransitionProfile, GetUpscaleMethod, )"
		R"(SetUpscaleMethod, )"
		R"(SetVRUpscalingTransitionProfileForMethod, GetVRUpscalingApplyBlockReasons, IsVRUpscalingProfileApplyAllowed, )"
		R"(SampleWind, )"
		R"(SampleWindExcludingHavokImpulses. params: named args for setters (enabled: bool; preset/profile/method: the ABI enum's underlying uint value; renderScaleModeEnabled: bool); wind sampling takes x, y, z world coordinates and returns the decomposed wind sample; )"
		R"(SampleWindExcludingHavokImpulses also returns windExcludingHavokImpulses, which excludes native physics effects. Omitted args default to 0/false, so always pass every arg a setter takes. Getters return {result}. )"
		R"(Setters return {staged:true,enqueued_at_frame} -- staged means the call was dispatched to the ABI, )"
		R"(NOT that it was accepted: the ABI validates enum arguments internally and silently ignores unsupported values (kHoshipa/kUltraQuality presets, DLSS profile kF, out-of-range methods, )"
		R"(or -- for the two VR transition methods -- any single invalid argument, which aborts the whole call), )"
		R"(only logging a warning. Confirm the real outcome with openshaders.feature action=get once frame_count (inspect kind=openshaders) advances past enqueued_at_frame; )"
		R"(an unsupported value leaves the setting unchanged. Every method is safe from this listener thread by the same contract API.md documents for a real plugin thread.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"method": {"type":"string"},
		"params": {"type":"object"}
	},
	"required": ["method"]
})"
		R"(})";
}
