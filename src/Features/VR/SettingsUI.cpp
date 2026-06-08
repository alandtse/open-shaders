#include "FeatureConstraints.h"
#include "Features/DynamicCubemaps.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/Upscaling.h"
#include "Features/VR.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/Fonts.h"
#include "RE/B/BSOpenVR.h"
#include "RE/P/PlayerCharacter.h"
#include "State.h"
#include "Utils/PerfUtils.h"
#include "Utils/UI.h"
#include "Utils/VRUtils.h"

#include <openvr.h>

#define I18N_KEY_PREFIX "feature.vr."

using AttachMode = VR::Settings::OverlayAttachMode;

namespace
{
	bool BeginTabItemWithFont(const char* label, Menu::FontRole role, ImGuiTabItemFlags flags = ImGuiTabItemFlags_None)
	{
		return MenuFonts::BeginTabItemWithFont(label, role, flags);
	}
}

//=============================================================================
// COMBO RECORDING HELPERS
//=============================================================================

void VR::ResetComboRecording()
{
	isCapturingCombo = false;
	currentComboType = ComboType::None;
	currentComboName = nullptr;
	recordedCombo.clear();
	comboStartTime = 0.0;
	recordingButtonControllers.clear();
}

void VR::ApplyRecordedCombo()
{
	if (recordedCombo.empty())
		return;

	switch (currentComboType) {
	case ComboType::MenuOpen:
		settings.VRMenuOpenKeys = recordedCombo;
		break;
	case ComboType::MenuClose:
		settings.VRMenuCloseKeys = recordedCombo;
		break;
	case ComboType::OverlayOpen:
		settings.VROverlayOpenKeys = recordedCombo;
		break;
	case ComboType::OverlayClose:
		settings.VROverlayCloseKeys = recordedCombo;
		break;
	default:
		break;
	}
}

//=============================================================================
// OVERLAY (WELCOME MESSAGE)
//=============================================================================

void VR::DrawOverlay()
{
	auto& vr = globals::features::vr;
	if (!vr.IsOpenVRCompatible())
		return;
	static LARGE_INTEGER overlayShowStart = { 0 };
	static LARGE_INTEGER freq = { 0 };

	bool shouldShow = IsWelcomeOverlayVisible();

	if (!shouldShow) {
		overlayShowStart.QuadPart = 0;
		return;
	}

	if (freq.QuadPart == 0) {
		QueryPerformanceFrequency(&freq);
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	if (overlayShowStart.QuadPart == 0) {
		overlayShowStart = now;
	}

	double elapsed = double(now.QuadPart - overlayShowStart.QuadPart) / double(freq.QuadPart);
	const double autoHideSeconds = static_cast<double>(settings.kAutoHideSeconds);
	if (elapsed >= autoHideSeconds) {
		return;
	}
	int secondsLeft = int(std::ceil(autoHideSeconds - elapsed));

	ImGuiIO& io = ImGui::GetIO();
	const float scale = Util::GetUIScale();
	ImVec2 overlaySize(520 * scale, 0);
	ImVec2 overlayPos = ImVec2((io.DisplaySize.x - overlaySize.x) * 0.5f, (io.DisplaySize.y * 0.35f));
	ImGui::SetNextWindowPos(overlayPos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(overlaySize, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.95f);

	ImGui::Begin("HowToUseOverlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

	ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 500.0f * scale);
	ImGui::TextWrapped("%s", T(TKEY("overlay_how_to_title"), "How to Use VR Open Shaders Menu:"));
	ImGui::Separator();
	ImGui::TextWrapped("%s", T(TKEY("overlay_open_menu_first"), "You must open the Main Menu or Tween Menu before VR controls work."));
	ImGui::Spacing();
	ImGui::PopTextWrapPos();

	ImGui::Text("%s", T(TKEY("overlay_open_menu_label"), "Open Menu: "));
	ImGui::SameLine();
	Util::DrawButtonCombo(settings.VRMenuOpenKeys, true);

	ImGui::Text("%s", T(TKEY("overlay_close_menu_label"), "Close Menu: "));
	ImGui::SameLine();
	Util::DrawButtonCombo(settings.VRMenuCloseKeys, true);

	ImGui::Spacing();
	ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 500.0f * scale);
	ImGui::TextWrapped("%s", T(TKEY("overlay_grip_thumbstick_depth"), "Grip + Thumbstick: Adjust overlay depth (closer/farther)"));
	ImGui::Spacing();
	ImGui::TextWrapped("%s", T(TKEY("overlay_disable_tip"), "Tip: Disable this VR overlay by setting Attach Mode to 'None' in VR settings."));
	ImGui::Spacing();
	ImGui::TextWrapped(T(TKEY("overlay_auto_hide_countdown"), "(This welcome message will auto-hide in %d seconds)"), secondsLeft);
	ImGui::TextWrapped("%s", T(TKEY("overlay_disable_location"), "(Disable in: VR settings > Controller Input Instructions)"));
	ImGui::PopTextWrapPos();

	ImGui::End();
}

//=============================================================================
// ANONYMOUS NAMESPACE: SETTINGS PANEL DRAW FUNCTIONS
//=============================================================================

namespace
{
	void DrawControllerInputInstructions()
	{
		auto& vr = globals::features::vr;
		auto& settings = vr.settings;
		if (!vr.IsOpenVRCompatible())
			return;
		if (ImGui::CollapsingHeader(T(TKEY("controller_input_header"), "Controller Input Instructions"), ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderInt(T(TKEY("auto_hide_timeout"), "Auto-hide Welcome overlay timeout"), &settings.kAutoHideSeconds, 0, VR::Config::kMaxAutoHideSeconds,
				settings.kAutoHideSeconds <= 0 ? "Hidden" : "%d seconds");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("auto_hide_timeout_tooltip"), "Set to 0 to hide the overlay, or a positive value to show it for that many seconds"));
			}
			ImGui::TextWrapped("%s", T(TKEY("instructions_menu_section"), "Menu (while in the main menu or tween menu):"));
			if (ImGui::BeginTable("MenuInstructionsTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", T(TKEY("instructions_open_menu"), "Open the Open Shaders Menu:"));
				ImGui::TableSetColumnIndex(1);
				Util::DrawButtonCombo(settings.VRMenuOpenKeys, true);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", T(TKEY("instructions_close_menu"), "Close the Open Shaders Menu:"));
				ImGui::TableSetColumnIndex(1);
				Util::DrawButtonCombo(settings.VRMenuCloseKeys, true);
				ImGui::EndTable();
			}
			ImGui::TextWrapped("%s", T(TKEY("instructions_overlay_section"), "Overlay (while in the main menu or tween menu):"));
			if (ImGui::BeginTable("OverlayInstructionsTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", T(TKEY("instructions_open_overlay"), "Open Overlay:"));
				ImGui::TableSetColumnIndex(1);
				Util::DrawButtonCombo(settings.VROverlayOpenKeys, true);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", T(TKEY("instructions_close_overlay"), "Close Overlay:"));
				ImGui::TableSetColumnIndex(1);
				Util::DrawButtonCombo(settings.VROverlayCloseKeys, true);
				ImGui::EndTable();
			}
			ImGui::TextWrapped("%s", T(TKEY("instructions_controller_input_section"), "Menu Controller Input:"));
			if (ImGui::BeginTable("ControllerInputTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(Util::GetControllerBothColor(), "%s", T(TKEY("input_trigger_both"), "Trigger (Both Controllers)"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", T(TKEY("input_left_mouse_button"), "Left mouse button"));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(Util::GetControllerBothColor(), "%s", T(TKEY("input_grip_both"), "Grip (Both Controllers)"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", T(TKEY("input_right_mouse_button"), "Right mouse button"));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(Util::GetControllerBothColor(), "%s", T(TKEY("input_touchpad_both"), "Touchpad Click (Both Controllers)"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", T(TKEY("input_middle_mouse_button"), "Middle mouse button"));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(Util::GetControllerBothColor(), "%s", T(TKEY("input_stick_click_both"), "Stick Click (Both Controllers)"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", T(TKEY("input_middle_mouse_button_2"), "Middle mouse button"));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(Util::GetControllerBothColor(), "%s", T(TKEY("input_ax_both"), "A/X (Both Controllers)"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", T(TKEY("input_enter"), "Enter"));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(Util::GetControllerPrimaryColor(), "%s", T(TKEY("input_by_primary"), "B/Y (Primary Controller)"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", T(TKEY("input_tab"), "Tab"));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(Util::GetControllerSecondaryColor(), "%s", T(TKEY("input_by_secondary"), "B/Y (Secondary Controller)"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", T(TKEY("input_shift_tab"), "Shift+Tab"));
				ImGui::EndTable();
			}
			bool useAttachedControllerForCursor = (settings.attachMode == AttachMode::ControllerOnly || settings.attachMode == AttachMode::Both);
			if (ImGui::BeginTable("ThumbstickInstructionsTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				if (useAttachedControllerForCursor) {
					if (settings.VRMenuAttachController == ControllerDevice::Primary) {
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::TextColored(Util::GetControllerPrimaryColor(), "%s", T(TKEY("thumbstick_primary"), "Primary Controller Thumbstick"));
						ImGui::TableSetColumnIndex(1);
						ImGui::Text("%s", T(TKEY("thumbstick_mouse_movement_attached"), "Mouse movement (attached controller)"));
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::TextColored(Util::GetControllerSecondaryColor(), "%s", T(TKEY("thumbstick_secondary"), "Secondary Controller Thumbstick"));
						ImGui::TableSetColumnIndex(1);
						ImGui::Text("%s", T(TKEY("thumbstick_scroll"), "Scroll"));
					} else {
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::TextColored(Util::GetControllerPrimaryColor(), "%s", T(TKEY("thumbstick_primary"), "Primary Controller Thumbstick"));
						ImGui::TableSetColumnIndex(1);
						ImGui::Text("%s", T(TKEY("thumbstick_scroll"), "Scroll"));
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::TextColored(Util::GetControllerSecondaryColor(), "%s", T(TKEY("thumbstick_secondary"), "Secondary Controller Thumbstick"));
						ImGui::TableSetColumnIndex(1);
						ImGui::Text("%s", T(TKEY("thumbstick_mouse_movement_attached"), "Mouse movement (attached controller)"));
					}
				} else {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextColored(Util::GetControllerPrimaryColor(), "%s", T(TKEY("thumbstick_primary"), "Primary Controller Thumbstick"));
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%s", T(TKEY("thumbstick_mouse_movement_hmd"), "Mouse movement (HMD mode)"));
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextColored(Util::GetControllerSecondaryColor(), "%s", T(TKEY("thumbstick_secondary"), "Secondary Controller Thumbstick"));
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%s", T(TKEY("thumbstick_scroll"), "Scroll"));
				}
				ImGui::EndTable();
			}
		}
	}

	void DrawStereoSettings()
	{
		auto& vr = globals::features::vr;
		VR::Settings& settings = vr.settings;

		if (ImGui::CollapsingHeader(T(TKEY("stereo_reprojection_header"), "Stereo Reprojection"), ImGuiTreeNodeFlags_DefaultOpen))
			vr.stereoOpt.DrawSettings();

		bool hasEffects = VR::AnyScreenSpaceEffectLoaded();
		bool isDev = globals::state && globals::state->IsDeveloperMode();

		if (ImGui::CollapsingHeader(T(TKEY("stereo_blend_header"), "Stereo Blend"), ImGuiTreeNodeFlags_DefaultOpen)) {
			if (!hasEffects && !isDev) {
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", T(TKEY("stereo_blend_requires_effect"), "Requires an active screen-space effect (SSGI, SS Shadows, SSR)."));
			} else {
				if (!hasEffects)
					ImGui::TextColored(ImVec4(0.6f, 0.6f, 1.0f, 1.0f), "%s", T(TKEY("stereo_blend_dev_mode"), "Developer mode: no screen-space effects active."));

				ImGui::Checkbox(T(TKEY("stereo_blend_enable"), "Enable Stereo Blend"), &settings.EnableStereoBlend);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s",
						T(TKEY("stereo_blend_enable_tooltip"),
							"Post-composite depth-aware bilateral blend between eyes.\n"
							"Reduces stereo inconsistencies from screen-space effects (SSGI, SSR, etc.).\n"
							"Each pixel is reprojected to the other eye; blending is applied only where\n"
							"depth agrees (same surface). Full-screen pass in VR."));
				}

				ImGui::BeginDisabled(!settings.EnableStereoBlend);

				ImGui::SliderFloat(T(TKEY("stereo_blend_depth_sigma"), "Depth Sigma"), &settings.StereoBlendDepthSigma, 0.001f, 0.1f, "%.4f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s",
						T(TKEY("stereo_blend_depth_sigma_tooltip"),
							"Depth sensitivity for the bilateral weight.\n"
							"Lower values are stricter -- only blend when depths match very closely.\n"
							"Higher values allow blending across slight depth differences.\n"
							"Default: 0.01"));
				}

				ImGui::SliderFloat(T(TKEY("stereo_blend_max_factor"), "Max Blend Factor"), &settings.StereoBlendMaxFactor, 0.0f, 0.5f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s",
						T(TKEY("stereo_blend_max_factor_tooltip"),
							"Maximum blend strength between the two eyes.\n"
							"Higher values reduce screen-space effect flicker but destroy stereo depth.\n"
							"Keep below ~0.15 to preserve 3D parallax.\n"
							"Default: 0.1"));
				}

				ImGui::SliderFloat(T(TKEY("stereo_blend_color_threshold"), "Color Difference Threshold"), &settings.StereoBlendColorThreshold, 0.0f, 0.2f, "%.3f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s",
						T(TKEY("stereo_blend_color_threshold_tooltip"),
							"Minimum luminance difference between eyes to trigger blending.\n"
							"Set to 0 to blend everywhere. Higher = more selective.\n"
							"Default: 0.02"));
				}

				ImGui::EndDisabled();
			}
		}

		if (hasEffects || isDev) {
			ImGui::Separator();

			// Auto-enable required feature when a debug mode is selected; restore on Off.
			// Tracks what we toggled so user-initiated changes aren't clobbered.
			static bool s_weEnabledStereoBlend = false;
			static bool s_weEnabledReproj = false;

			const char* debugModes[] = {
				T(TKEY("stereo_debug_off"), "Off"),
				T(TKEY("stereo_debug_back_check"), "Back-Check"),
				T(TKEY("stereo_debug_blend_weight"), "Blend Weight"),
				T(TKEY("stereo_debug_edge_detection"), "Edge Detection"),
				T(TKEY("stereo_debug_overwrite"), "Overwrite"),
				T(TKEY("stereo_debug_overwrite_eye1"), "Overwrite Eye1")
			};
			if (ImGui::Combo(T(TKEY("stereo_debug_view"), "Debug View"), &settings.StereoBlendDebugMode, debugModes, IM_ARRAYSIZE(debugModes))) {
				int newMode = settings.StereoBlendDebugMode;
				bool needsBlend = (newMode >= 1 && newMode <= 3);
				bool needsReproj = (newMode == 4 || newMode == 5);

				// Auto-enable Stereo Blend for modes 1-3 (runtime-toggleable)
				if (needsBlend && !settings.EnableStereoBlend) {
					settings.EnableStereoBlend = true;
					s_weEnabledStereoBlend = true;
				} else if (!needsBlend && s_weEnabledStereoBlend) {
					settings.EnableStereoBlend = false;
					s_weEnabledStereoBlend = false;
				}

				// Auto-enable Reprojection for modes 4-5 (note: takes effect after restart)
				auto& sm = vr.stereoOpt.settings.stereoMode;
				if (needsReproj && sm == VRStereoOptimizations::StereoMode::Off) {
					sm = VRStereoOptimizations::StereoMode::Enable;
					s_weEnabledReproj = true;
				} else if (!needsReproj && s_weEnabledReproj) {
					sm = VRStereoOptimizations::StereoMode::Off;
					s_weEnabledReproj = false;
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s",
					T(TKEY("stereo_debug_view_tooltip"),
						"Selecting a debug mode auto-enables the required feature; setting back to Off restores it.\n\n"
						"Off: Normal rendering\n"
						"Back-Check: Round-trip reprojection validation (auto-enables Stereo Blend)\n"
						"Blend Weight: Heatmap of bilateral blend intensity (auto-enables Stereo Blend)\n"
						"Edge Detection: Highlights depth discontinuities (auto-enables Stereo Blend)\n"
						"Overwrite: Mode texture classification (auto-enables Reprojection -- restart required)\n"
						"  Green=edge  Pink=edge neighbour  Blue=disoccluded  Orange=full blend\n"
						"Overwrite Eye1: POM depth heatmap for Eye 1 (auto-enables Reprojection -- restart required)"));
			}
		}

		if (ImGui::CollapsingHeader(T(TKEY("foveated_effects_header"), "Foveated Effects"))) {
			auto& upscaling = globals::features::upscaling;
			auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			const bool foveatedDLSSActive = upscaling.foveatedRender.IsActive();
			const bool ssrEnabled = dynamicCubemaps.loaded && dynamicCubemaps.settings.EnabledSSR;

			if (!foveatedDLSSActive)
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", T(TKEY("foveated_requires_dlss"), "Requires Foveated DLSS to be active (Upscaling settings)."));
			if (!ssrEnabled)
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", T(TKEY("foveated_requires_ssr"), "Requires Screen Space Reflections (Dynamic Cubemaps)."));

			ImGui::BeginDisabled(!foveatedDLSSActive || !ssrEnabled);
			ImGui::Checkbox(T(TKEY("foveated_ssr_raymarching"), "Foveate SSR Raymarching"), &settings.EnableSSRFoveation);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s",
					T(TKEY("foveated_ssr_raymarching_tooltip"),
						"Reduces screen-space reflection raymarching toward the periphery, using the\n"
						"active Foveated DLSS region. Central reflections stay full quality; peripheral\n"
						"pixels fall back to the cubemap / water reflection. VR only."));
			}

			ImGui::BeginDisabled(!settings.EnableSSRFoveation);
			ImGui::Checkbox("Hard Cutoff Outside Center##SSRFoveation", &settings.EnableSSRFoveationHardCutoff);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s",
					T(TKEY("foveated_hard_cutoff_tooltip"),
						"Hard-skip SSR outside the center region instead of a feathered falloff.\n"
						"Cheaper, but the transition edge may be visible. Default off (feathered)."));
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
		}
	}

	void DrawGeneralVRSettings()
	{
		auto& vr = globals::features::vr;
		VR::Settings& settings = vr.settings;
		if (ImGui::CollapsingHeader(T(TKEY("general_settings_header"), "General Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
			bool exteriorChanged = ImGui::Checkbox(T(TKEY("depth_culling_exteriors"), "Enable Depth Buffer Culling in Exteriors"), &settings.EnableDepthBufferCullingExterior);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("depth_culling_exteriors_tooltip"), "Improves performance in exteriors, recommended ON."));
			}

			bool interiorChanged = ImGui::Checkbox(T(TKEY("depth_culling_interiors"), "Enable Depth Buffer Culling in Interiors"), &settings.EnableDepthBufferCullingInterior);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("depth_culling_interiors_tooltip"), "Improves performance in interiors, recommended ON."));
			}

			if (exteriorChanged || interiorChanged) {
				vr.UpdateDepthBufferCulling();
			}

			if (ImGui::SliderFloat(T(TKEY("min_occludee_box_extent"), "Min Occludee Box Extent"), &settings.MinOccludeeBoxExtent, 0.0f, 1000.0f, "%.1f")) {
				if (vr.gMinOccludeeBoxExtent) {
					*vr.gMinOccludeeBoxExtent = settings.MinOccludeeBoxExtent;
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("min_occludee_box_extent_tooltip"), "Minimum bounding box dimensions for object occlusion culling. Lower values improve performance but may result in visual artifacts."));
			}
		}
	}

	void DrawMenuSettings()
	{
		auto& vr = globals::features::vr;
		auto& settings = vr.settings;
		if (!vr.IsOpenVRCompatible())
			return;
		if (ImGui::CollapsingHeader(T(TKEY("menu_settings_header"), "Menu Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
			float maxScale = VR::Config::kMaxMenuScale;
			ImGui::SliderFloat(T(TKEY("menu_scale"), "Menu Scale"), &settings.VRMenuScale, VR::Config::kMinMenuScale, maxScale, "%.2f");
			const char* positioningMethods[] = {
				T(TKEY("menu_pos_hmd_relative"), "HMD Relative"),
				T(TKEY("menu_pos_fixed_world"), "Fixed World Position")
			};
			int prevMethod = settings.VRMenuPositioningMethod;
			if (ImGui::Combo(T(TKEY("menu_positioning_method"), "Menu Positioning Method"), &settings.VRMenuPositioningMethod, positioningMethods, IM_ARRAYSIZE(positioningMethods))) {
				if (prevMethod != 1 && settings.VRMenuPositioningMethod == 1) {
					vr.SetFixedOverlayToCurrentHMD();
					auto player = RE::PlayerCharacter::GetSingleton();
					if (player)
						vr.savedPlayerWorldPos = player->GetPosition();
				}
			}
			const char* attachModes[] = {
				T(TKEY("attach_mode_hmd_only"), "HMD Only"),
				T(TKEY("attach_mode_controller_only"), "Controller Only"),
				T(TKEY("attach_mode_both"), "Both"),
				T(TKEY("attach_mode_none"), "None (Disabled)")
			};
			int attachModeInt = static_cast<int>(settings.attachMode);
			if (ImGui::Combo(T(TKEY("attach_mode"), "Attach Mode"), &attachModeInt, attachModes, IM_ARRAYSIZE(attachModes))) {
				settings.attachMode = static_cast<AttachMode>(attachModeInt);
			}

			if (settings.attachMode == AttachMode::ControllerOnly ||
				settings.attachMode == AttachMode::Both) {
				const char* attachControllers[] = {
					T(TKEY("attach_controller_primary"), "Primary Controller"),
					T(TKEY("attach_controller_secondary"), "Secondary Controller")
				};
				int attachControllerInt = static_cast<int>(settings.VRMenuAttachController);
				if (ImGui::Combo(T(TKEY("attach_to_controller"), "Attach to Controller"), &attachControllerInt, attachControllers, IM_ARRAYSIZE(attachControllers))) {
					settings.VRMenuAttachController = static_cast<ControllerDevice>(attachControllerInt);
				}

				ImGui::Separator();
				ImGui::Text("%s", T(TKEY("controller_offset_settings"), "Controller Offset Settings"));
				ImGui::SliderFloat(T(TKEY("controller_offset_x"), "Controller Offset X"), &settings.VRMenuControllerOffsetX, -2.0f, 2.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("controller_offset_y"), "Controller Offset Y"), &settings.VRMenuControllerOffsetY, -2.0f, 2.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("controller_offset_z"), "Controller Offset Z"), &settings.VRMenuControllerOffsetZ, -2.0f, 2.0f, "%.2f");
			}

			if (settings.attachMode == AttachMode::HMDOnly ||
				settings.attachMode == AttachMode::Both) {
				ImGui::Separator();
				ImGui::Text("%s", T(TKEY("hmd_offset_settings"), "HMD Offset Settings"));
				ImGui::SliderFloat(T(TKEY("hmd_offset_x"), "HMD Offset X"), &settings.VRMenuOffsetX, -2.0f, 2.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("hmd_offset_y"), "HMD Offset Y"), &settings.VRMenuOffsetY, -2.0f, 2.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("hmd_offset_z"), "HMD Offset Z"), &settings.VRMenuOffsetZ, -4.0f, 1.0f, "%.2f");
			}

			if (settings.VRMenuPositioningMethod == 1) {
				ImGui::Separator();
				ImGui::Text("%s", T(TKEY("fixed_world_pos_settings"), "Fixed World Position Settings"));
				ImGui::SliderFloat(T(TKEY("auto_reset_distance"), "Auto Reset Distance (game units)"), &settings.VRMenuAutoResetDistance, 100.0f, 5000.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text(T(TKEY("auto_reset_distance_tooltip"), "If you move farther than this distance from the menu, it will automatically reset to your HMD position. %s"), Util::Units::FormatDistance(settings.VRMenuAutoResetDistance).c_str());
				}
				if (ImGui::Button(T(TKEY("reset_menu_to_hmd"), "Reset Menu to HMD Position"))) {
					vr.SetFixedOverlayToCurrentHMD();
				}
			}
		}
	}

	void DrawMouseSettings()
	{
		auto& vr = globals::features::vr;
		if (!vr.IsOpenVRCompatible())
			return;
		VR::Settings& settings = vr.settings;
		if (ImGui::CollapsingHeader(T(TKEY("input_settings_header"), "Input Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::Checkbox(T(TKEY("enable_wand_pointing"), "Enable Wand Pointing"), &settings.EnableWandPointing)) {
				vr.wandState.isIntersecting = false;
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("enable_wand_pointing_tooltip"), "Use controller ray-casting to point at UI elements"));
			}
			ImGui::Separator();
			ImGui::Text("%s", T(TKEY("joystick_settings"), "Joystick Settings"));
			ImGui::SliderFloat(T(TKEY("mouse_deadzone"), "Mouse Deadzone"), &settings.mouseDeadzone, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("mouse_deadzone_tooltip"), "Thumbstick deadzone for joystick cursor movement"));
			}
			ImGui::SliderFloat(T(TKEY("mouse_speed"), "Mouse Speed"), &settings.mouseSpeed, 0.1f, 50.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("mouse_speed_tooltip"), "Speed multiplier for joystick cursor movement"));
			}
		}
	}

	void DrawDragSettings()
	{
		auto& vr = globals::features::vr;
		if (!vr.IsOpenVRCompatible())
			return;
		VR::Settings& settings = vr.settings;
		if (ImGui::CollapsingHeader(T(TKEY("drag_settings_header"), "Drag Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::CollapsingHeader(T(TKEY("drag_instructions_header"), "Drag Instructions"), ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::TextWrapped("%s", T(TKEY("drag_overlay_positioning"), "Overlay Positioning (Grip + Drag):"));
				ImGui::BulletText("%s", T(TKEY("drag_fixed_world"), "Fixed World Position: Any controller can drag (HMD-only mode) or attached controller only (Both modes)"));
				ImGui::BulletText("%s", T(TKEY("drag_hmd_relative"), "HMD Relative: Any controller can drag (HMD-only mode) or attached controller only (Both modes)"));
				ImGui::BulletText("%s", T(TKEY("drag_controller_attached"), "Controller Attached: Only the opposite hand can drag the controller overlay"));
				ImGui::Spacing();
				ImGui::TextWrapped("%s", T(TKEY("drag_depth_adjustment"), "Depth Adjustment (Grip + Thumbstick):"));
				ImGui::BulletText("%s", T(TKEY("drag_depth_thumbstick"), "While gripping to drag, use the thumbstick on the same hand to adjust depth"));
				ImGui::BulletText("%s", T(TKEY("drag_thumbstick_forward"), "Thumbstick forward: Push overlay farther away"));
				ImGui::BulletText("%s", T(TKEY("drag_thumbstick_back"), "Thumbstick back: Pull overlay closer"));
			}
			ImGui::Checkbox(T(TKEY("enable_drag_reposition"), "Enable drag to reposition overlays"), &settings.EnableDragToReposition);
			ImGui::BeginDisabled(!settings.EnableDragToReposition);
			ImGui::ColorEdit4(T(TKEY("drag_highlight_color"), "Drag Highlight Color"), settings.dragHighlightColor.data());
			ImGui::EndDisabled();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("drag_highlight_color_tooltip"), "Color used to highlight draggable overlays in VR."));
			}
		}
	}

	void DrawKeyBindings()
	{
		auto& vr = globals::features::vr;
		auto& settings = vr.settings;

		if (ImGui::CollapsingHeader(T(TKEY("combo_settings_header"), "Combo Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat(T(TKEY("combo_timeout"), "Combo Timeout"), &settings.comboTimeout, 1.0f, 10.0f, "%.1f seconds");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("combo_timeout_tooltip"), "Time limit for recording button combinations."));
			}
		}
		ImGui::Separator();
		const char* comboTypes[] = {
			T(TKEY("combo_type_open_menu"), "Open the Open Shaders Menu"),
			T(TKEY("combo_type_close_menu"), "Close the Open Shaders Menu"),
			T(TKEY("combo_type_open_overlay"), "Open VR Overlay"),
			T(TKEY("combo_type_close_overlay"), "Close VR Overlay")
		};
		static int selectedComboIndex = 0;
		ImGui::Text("%s", T(TKEY("select_combo_to_record"), "Select Combo to Record:"));
		ImGui::SameLine();
		if (ImGui::Combo("##ComboSelector", &selectedComboIndex, comboTypes, IM_ARRAYSIZE(comboTypes))) {
			vr.isCapturingCombo = false;
			vr.currentComboType = VR::ComboType::None;
			vr.recordedCombo.clear();
		}
		if (ImGui::Button(T(TKEY("record_selected_combo"), "Record Selected Combo"))) {
			vr.isCapturingCombo = true;
			vr.currentComboType = static_cast<VR::ComboType>(selectedComboIndex + 1);
			vr.currentComboName = comboTypes[selectedComboIndex];
			vr.recordedCombo.clear();
			vr.comboStartTime = Util::GetNowSecs();
			vr.recordingButtonControllers.clear();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(T(TKEY("clear_button"), "Clear"))) {
			switch (selectedComboIndex) {
			case 0:
				settings.VRMenuOpenKeys.clear();
				break;
			case 1:
				settings.VRMenuCloseKeys.clear();
				break;
			case 2:
				settings.VROverlayOpenKeys.clear();
				break;
			case 3:
				settings.VROverlayCloseKeys.clear();
				break;
			}
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("record_combo_tooltip"), "Click to start recording a new button combination for the selected action."));
		}
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		if (ImGui::BeginTable("##VRBindingsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn(T(TKEY("bindings_col_action"), "Action"));
			ImGui::TableSetupColumn(T(TKEY("bindings_col_current"), "Current Binding"));
			ImGui::TableSetupColumn(T(TKEY("bindings_col_description"), "Description"));
			ImGui::TableHeadersRow();
			struct VRKeyBindingConfig
			{
				const char* label;
				std::vector<InputCombo>& combos;
				const char* description;
				const char* controllerRequirement;
			};
			std::vector<VRKeyBindingConfig> keyBindingConfigs = {
				{ T(TKEY("combo_type_open_menu"), "Open the Open Shaders Menu"), settings.VRMenuOpenKeys, T(TKEY("binding_desc_open_menu"), "Button combination to open the Open Shaders menu"), "Primary" },
				{ T(TKEY("combo_type_close_menu"), "Close the Open Shaders Menu"), settings.VRMenuCloseKeys, T(TKEY("binding_desc_close_menu"), "Button combination to close the Open Shaders menu"), "Both" },
				{ T(TKEY("combo_type_open_overlay"), "Open VR Overlay"), settings.VROverlayOpenKeys, T(TKEY("binding_desc_open_overlay"), "Button combination to open the VR overlay"), "Primary" },
				{ T(TKEY("combo_type_close_overlay"), "Close VR Overlay"), settings.VROverlayCloseKeys, T(TKEY("binding_desc_close_overlay"), "Button combination to close the VR overlay"), "Secondary" }
			};
			for (size_t row = 0; row < keyBindingConfigs.size(); ++row) {
				const auto& config = keyBindingConfigs[row];
				ImGui::TableNextRow();
				if (row == static_cast<size_t>(selectedComboIndex)) {
					ImU32 highlight = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 0.0f, 0.15f));
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, highlight);
				}
				ImGui::TableSetColumnIndex(0);
				char selectableId[64];
				snprintf(selectableId, sizeof(selectableId), "##combo_row_%zu", row);
				bool rowSelected = (row == static_cast<size_t>(selectedComboIndex));
				if (ImGui::Selectable(selectableId, rowSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 0))) {
					selectedComboIndex = static_cast<int>(row);
				}
				ImGui::SameLine(0, 0);
				ImGui::Text("%s", config.label);
				ImGui::TableSetColumnIndex(1);
				Util::DrawButtonCombo(config.combos, false);
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%s", config.description);
			}
			ImGui::EndTable();
		}
		ImGui::Spacing();
		if (ImGui::Button(T(TKEY("reset_to_defaults"), "Reset to Defaults"))) {
			VR::Settings defaults;
			settings.VRMenuOpenKeys = defaults.VRMenuOpenKeys;
			settings.VRMenuCloseKeys = defaults.VRMenuCloseKeys;
			settings.VROverlayOpenKeys = defaults.VROverlayOpenKeys;
			settings.VROverlayCloseKeys = defaults.VROverlayCloseKeys;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("reset_to_defaults_tooltip"), "Reset all VR key bindings to their default values."));
		}
	}

	void DrawThumbstickColumn(VR& vr, bool showPrimary, ImU32 highlightCol)
	{
		auto& state = showPrimary ? vr.primaryControllerState : vr.secondaryControllerState;
		auto role = showPrimary ? RE::ControllerRole::Primary : RE::ControllerRole::Secondary;
		float x = state.thumbsticks[static_cast<size_t>(role)].x;
		float y = state.thumbsticks[static_cast<size_t>(role)].y;

		ImVec2 padSize = ImVec2(80, 80);
		ImVec2 cursor = ImGui::GetCursorScreenPos();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 center = ImVec2(cursor.x + padSize.x / 2, cursor.y + padSize.y / 2);
		float radius = padSize.x / 2 - 4;
		ImU32 borderCol = ImGui::GetColorU32(ImGuiCol_Border);
		ImU32 axisCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		ImU32 dotCol = ImGui::GetColorU32(ImGuiCol_Text);

		drawList->AddRectFilled(cursor, ImVec2(cursor.x + padSize.x, cursor.y + padSize.y), ImGui::GetColorU32(ImGuiCol_FrameBg));
		drawList->AddRect(cursor, ImVec2(cursor.x + padSize.x, cursor.y + padSize.y), borderCol, 4.0f, 0, 2.0f);
		drawList->AddLine(ImVec2(center.x, cursor.y + 4), ImVec2(center.x, cursor.y + padSize.y - 4), axisCol, 1.0f);
		drawList->AddLine(ImVec2(cursor.x + 4, center.y), ImVec2(cursor.x + padSize.x - 4, center.y), axisCol, 1.0f);

		int quad = 0;
		if (x > 0 && y > 0)
			quad = 1;
		else if (x < 0 && y > 0)
			quad = 2;
		else if (x < 0 && y < 0)
			quad = 3;
		else if (x > 0 && y < 0)
			quad = 4;

		if (quad != 0) {
			ImVec2 q0 = center, q1 = center, q2 = center, q3 = center;
			if (quad == 1) {
				q1 = { center.x + radius, center.y - radius };
				q2 = { center.x + radius, center.y };
				q3 = { center.x, center.y - radius };
			} else if (quad == 2) {
				q1 = { center.x - radius, center.y - radius };
				q2 = { center.x - radius, center.y };
				q3 = { center.x, center.y - radius };
			} else if (quad == 3) {
				q1 = { center.x - radius, center.y + radius };
				q2 = { center.x - radius, center.y };
				q3 = { center.x, center.y + radius };
			} else if (quad == 4) {
				q1 = { center.x + radius, center.y + radius };
				q2 = { center.x + radius, center.y };
				q3 = { center.x, center.y + radius };
			}
			ImVec2 poly[4] = { center, q1, q2, q3 };
			drawList->AddConvexPolyFilled(poly, 4, highlightCol);
		}

		ImVec2 dot = ImVec2(center.x + x * radius, center.y - y * radius);
		drawList->AddCircleFilled(dot, 5.0f, dotCol);

		ImGui::Dummy(padSize);
		ImGui::SetNextItemWidth(160.0f);
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetTextLineHeight());
		ImGui::Text(T(TKEY("thumbstick_xy_quadrant"), "X: %+1.3f  Y: %+1.3f  [%s]"), x, y, RE::GetQuadrantName(x, y));
	}

	void DrawDebugSection()
	{
		auto& vr = globals::features::vr;
		auto& settings = vr.settings;
		auto menu = globals::menu;

		if (ImGui::CollapsingHeader(T(TKEY("openvr_info_header"), "OpenVR Information"), ImGuiTreeNodeFlags_DefaultOpen)) {
			auto& info = vr.openVRInfo;
			if (info.isAvailable) {
				if (vr.IsOpenVRCompatible()) {
					ImGui::Text("%s", T(TKEY("openvr_active_compatible"), "OpenVR System: Active & Compatible"));
				} else {
					ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", T(TKEY("openvr_active_incompatible"), "OpenVR System: Active but INCOMPATIBLE"));
					ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", T(TKEY("openvr_menus_disabled"), "VR overlay menus disabled."));
				}

				ImGui::Text(T(TKEY("openvr_runtime"), "Runtime: %s"), VRDetection::RuntimeTypeToString(info.runtimeType));
				ImGui::Text(T(TKEY("openvr_dll_path"), "DLL Path: %s"), info.dllPath.c_str());
				ImGui::Text(T(TKEY("openvr_dll_version"), "DLL Version: %s"), info.version.c_str());
				ImGui::Text(T(TKEY("openvr_dll_size"), "DLL Size: %llu bytes"), info.fileSize);
				ImGui::Text(T(TKEY("openvr_modified"), "Modified: %s"), info.modificationTime.c_str());

				ImGui::Separator();
				ImGui::Text("%s", T(TKEY("openvr_detection_method"), "Detection Method:"));
				ImGui::Text(T(TKEY("openvr_interface_probing"), "  Interface Probing: %s"), info.probingSucceeded ? T(TKEY("openvr_passed"), "Passed") : T(TKEY("openvr_failed"), "Failed"));
				ImGui::Text(T(TKEY("openvr_ivroverlay"), "    IVROverlay_016: %s"), info.hasOverlayInterface ? T(TKEY("openvr_ok"), "OK") : T(TKEY("openvr_missing"), "Missing"));
				ImGui::Text(T(TKEY("openvr_ivrsystem"), "    IVRSystem_017: %s"), info.hasSystemInterface ? T(TKEY("openvr_ok"), "OK") : T(TKEY("openvr_missing"), "Missing"));
				ImGui::Text(T(TKEY("openvr_ivrcompositor"), "    IVRCompositor_021: %s"), info.hasCompositorInterface ? T(TKEY("openvr_ok"), "OK") : T(TKEY("openvr_missing"), "Missing"));
				ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", T(TKEY("openvr_rendering"), "  Rendering: In-scene overlay (submit hook)"));

			} else {
				ImGui::Text("%s", T(TKEY("openvr_not_available"), "OpenVR system not available"));
			}
		}

		if (ImGui::CollapsingHeader(T(TKEY("controller_diagnostics_header"), "Controller Diagnostics"), ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::Checkbox(T(TKEY("diagnostics_test_mode"), "Test Mode: Disable controller menu input (except scroll controller and triggers)"), &settings.VRMenuControllerDiagnosticsTestMode)) {
				ImGui::SetScrollHereY(0.0f);
			}
			ImGui::SeparatorText(T(TKEY("diagnostics_button_state"), "Button State"));
			double nowSecs = Util::GetNowSecs();
			ImVec4 highlightColor = menu->GetTheme().StatusPalette.InfoColor;
			ImU32 highlightColorU32 = ImGui::ColorConvertFloat4ToU32(highlightColor);

			bool isLeftHanded = vr.lastKnownLeftHandedMode;

			if (ImGui::BeginTable("vr_input_state_table", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn(T(TKEY("diag_col_button"), "Button"));
				if (isLeftHanded) {
					ImGui::TableSetupColumn(T(TKEY("diag_col_primary_state"), "Primary State"));
					ImGui::TableSetupColumn(T(TKEY("diag_col_primary_held"), "Primary Held (s)"));
					ImGui::TableSetupColumn(T(TKEY("diag_col_primary_type"), "Primary Type"));
					ImGui::TableSetupColumn(T(TKEY("diag_col_secondary_state"), "Secondary State"));
					ImGui::TableSetupColumn(T(TKEY("diag_col_secondary_held"), "Secondary Held (s)"));
					ImGui::TableSetupColumn(T(TKEY("diag_col_secondary_type"), "Secondary Type"));
				} else {
					ImGui::TableSetupColumn(T(TKEY("diag_col_secondary_state"), "Secondary State"));
					ImGui::TableSetupColumn(T(TKEY("diag_col_secondary_held"), "Secondary Held (s)"));
					ImGui::TableSetupColumn(T(TKEY("diag_col_secondary_type"), "Secondary Type"));
					ImGui::TableSetupColumn(T(TKEY("diag_col_primary_state"), "Primary State"));
					ImGui::TableSetupColumn(T(TKEY("diag_col_primary_held"), "Primary Held (s)"));
					ImGui::TableSetupColumn(T(TKEY("diag_col_primary_type"), "Primary Type"));
				}
				ImGui::TableHeadersRow();

				auto DrawButtonType = [](const RE::ButtonState& state) {
					if (!state.isPressed) {
						if (state.IsClick())
							ImGui::TextUnformatted(T(TKEY("diag_type_click"), "Click"));
						else if (state.IsHold())
							ImGui::TextUnformatted(T(TKEY("diag_type_hold"), "Hold"));
						else
							ImGui::TextUnformatted(T(TKEY("diag_type_none"), "-"));
					} else {
						ImGui::TextUnformatted(T(TKEY("diag_type_held"), "Held"));
					}
				};

				auto printRow = [&](const char* label, const RE::ButtonState& left, const RE::ButtonState& right) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(label);
					ImGui::TableSetColumnIndex(1);
					if (left.isPressed)
						ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, highlightColorU32);
					ImGui::TextUnformatted(left.isPressed ? T(TKEY("diag_pressed"), "Pressed") : T(TKEY("diag_released"), "Released"));
					ImGui::TableSetColumnIndex(2);
					if (left.isPressed)
						ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, highlightColorU32);
					ImGui::Text("%.2f", left.GetCurrentHeldTime(nowSecs));
					ImGui::TableSetColumnIndex(3);
					if (left.isPressed)
						ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, highlightColorU32);
					DrawButtonType(left);
					ImGui::TableSetColumnIndex(4);
					if (right.isPressed)
						ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, highlightColorU32);
					ImGui::TextUnformatted(right.isPressed ? T(TKEY("diag_pressed"), "Pressed") : T(TKEY("diag_released"), "Released"));
					ImGui::TableSetColumnIndex(5);
					if (right.isPressed)
						ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, highlightColorU32);
					ImGui::Text("%.2f", right.GetCurrentHeldTime(nowSecs));
					ImGui::TableSetColumnIndex(6);
					if (right.isPressed)
						ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, highlightColorU32);
					DrawButtonType(right);
				};

				auto printRowWithHandedness = [&](const char* label, auto key) {
					auto& primary = vr.primaryControllerState[key];
					auto& secondary = vr.secondaryControllerState[key];
					if (isLeftHanded) {
						printRow(label, primary, secondary);
					} else {
						printRow(label, secondary, primary);
					}
				};

				printRowWithHandedness(T(TKEY("diag_btn_trigger"), "Trigger"), RE::BSOpenVRControllerDevice::Keys::kTrigger);
				printRowWithHandedness(T(TKEY("diag_btn_grip"), "Grip"), RE::BSOpenVRControllerDevice::Keys::kGrip);
				printRowWithHandedness(T(TKEY("diag_btn_grip_alt"), "GripAlt"), RE::BSOpenVRControllerDevice::Keys::kGripAlt);
				printRowWithHandedness(T(TKEY("diag_btn_stick_click"), "Stick Click"), RE::BSOpenVRControllerDevice::Keys::kJoystickTrigger);
				printRowWithHandedness(T(TKEY("diag_btn_touchpad_click"), "Touchpad Click"), RE::BSOpenVRControllerDevice::Keys::kTouchpadClick);
				printRowWithHandedness(T(TKEY("diag_btn_touchpad_alt"), "Touchpad Alt"), RE::BSOpenVRControllerDevice::Keys::kTouchpadAlt);
				printRowWithHandedness(T(TKEY("diag_btn_by"), "B/Y"), RE::BSOpenVRControllerDevice::Keys::kBY);
				printRowWithHandedness(T(TKEY("diag_btn_ax"), "A/X"), RE::BSOpenVRControllerDevice::Keys::kXA);
				ImGui::EndTable();
			}

			ImGui::SeparatorText(T(TKEY("thumbstick_state_section"), "VR Thumbstick State"));
			ImU32 highlightCol = ImGui::ColorConvertFloat4ToU32(menu->GetTheme().StatusPalette.InfoColor);
			if (ImGui::BeginTable("##VRThumbstickTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
				if (isLeftHanded) {
					ImGui::TableSetupColumn(T(TKEY("thumbstick_col_primary"), "Primary Controller"), ImGuiTableColumnFlags_WidthFixed, 200.0f);
					ImGui::TableSetupColumn(T(TKEY("thumbstick_col_secondary"), "Secondary Controller"), ImGuiTableColumnFlags_WidthFixed, 200.0f);
				} else {
					ImGui::TableSetupColumn(T(TKEY("thumbstick_col_secondary"), "Secondary Controller"), ImGuiTableColumnFlags_WidthFixed, 200.0f);
					ImGui::TableSetupColumn(T(TKEY("thumbstick_col_primary"), "Primary Controller"), ImGuiTableColumnFlags_WidthFixed, 200.0f);
				}
				ImGui::TableHeadersRow();

				// Left column
				ImGui::TableSetColumnIndex(0);
				ImGui::BeginGroup();
				DrawThumbstickColumn(vr, isLeftHanded, highlightCol);
				ImGui::EndGroup();

				// Right column
				ImGui::TableSetColumnIndex(1);
				ImGui::BeginGroup();
				DrawThumbstickColumn(vr, !isLeftHanded, highlightCol);
				ImGui::EndGroup();
				ImGui::EndTable();
			}

			ImGui::SeparatorText(T(TKEY("events_section"), "Recent VR Controller Events"));
			ImGui::TextDisabled("%s", T(TKEY("events_note"), "Note: For thumbstick events, KeyCode/Value columns show X/Y floats."));
			if (ImGui::BeginTable("eventlog", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
				ImGui::TableSetupColumn(T(TKEY("events_col_device"), "Device"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
				ImGui::TableSetupColumn(T(TKEY("events_col_keycode_x"), "KeyCode/X"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn(T(TKEY("events_col_value_y"), "Value/Y"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn(T(TKEY("events_col_pressed"), "Pressed"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableSetupColumn(T(TKEY("events_col_known_mapping"), "Known Mapping"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
				ImGui::TableSetupColumn(T(TKEY("events_col_event_type"), "Event Type"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
				ImGui::TableHeadersRow();
				for (const auto& e : vr.vrControllerEventLog) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::Text("%d", e.device);
					ImGui::TableSetColumnIndex(1);
					if (e.heldSource == "thumbstick") {
						ImGui::Text("%.3f", e.thumbstickX);
					} else {
						ImGui::Text("%d", e.keyCode);
					}
					ImGui::TableSetColumnIndex(2);
					if (e.heldSource == "thumbstick") {
						ImGui::Text("%.3f", e.thumbstickY);
					} else {
						ImGui::Text("%d", e.value);
					}
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%s", e.pressed ? T(TKEY("diag_pressed"), "Pressed") : T(TKEY("diag_released"), "Released"));
					ImGui::TableSetColumnIndex(4);
					if (e.heldSource == "thumbstick") {
						ImGui::TextUnformatted(e.controllerRole.c_str());
					} else {
						ImGui::TextUnformatted(RE::GetOpenVRButtonName(e.keyCode));
					}
					ImGui::TableSetColumnIndex(5);
					if (e.heldSource == "thumbstick") {
						ImGui::TextUnformatted(T(TKEY("events_type_none"), "-"));
					} else {
						if (!e.pressed) {
							if (e.heldTime > 0.0) {
								if (e.heldTime < 0.5) {
									ImGui::Text(T(TKEY("events_type_click"), "Click (%.2fs)"), e.heldTime);
								} else {
									ImGui::Text(T(TKEY("events_type_hold"), "Hold (%.2fs)"), e.heldTime);
								}
							} else {
								ImGui::Text("%s", T(TKEY("events_type_release"), "Release"));
							}
						} else if (e.pressed) {
							if (e.heldTime > 0.0) {
								ImGui::Text(T(TKEY("events_type_held_for"), "Held for %.2fs"), e.heldTime);
							} else {
								ImGui::Text("%s", T(TKEY("events_type_press"), "Press"));
							}
						}
					}
				}
				ImGui::EndTable();
			}

			ImGui::SeparatorText(T(TKEY("wand_state_section"), "Wand Pointing State"));
			if (ImGui::BeginTable("##WandPointingState", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn(T(TKEY("wand_col_property"), "Property"), ImGuiTableColumnFlags_WidthFixed, 200.0f);
				ImGui::TableSetupColumn(T(TKEY("wand_col_value"), "Value"), ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableHeadersRow();

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", T(TKEY("wand_pointing_enabled"), "Wand Pointing Enabled"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", settings.EnableWandPointing ? T(TKEY("wand_yes"), "Yes") : T(TKEY("wand_no"), "No"));

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", T(TKEY("wand_intersecting_overlay"), "Intersecting Overlay"));
				ImGui::TableSetColumnIndex(1);
				if (vr.wandState.isIntersecting) {
					ImGui::TextColored(menu->GetTheme().StatusPalette.InfoColor, "%s", T(TKEY("wand_yes_upper"), "YES"));
				} else {
					ImGui::Text("%s", T(TKEY("wand_no"), "No"));
				}

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", T(TKEY("wand_uv_coordinates"), "UV Coordinates"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("(%.3f, %.3f)", vr.wandState.uvCoordinates.x, vr.wandState.uvCoordinates.y);

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", T(TKEY("wand_controller_index"), "Controller Index"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%u", vr.wandState.controllerIndex);

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", T(TKEY("wand_ray_origin"), "Ray Origin"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("(%.2f, %.2f, %.2f)", vr.wandState.rayOrigin.x, vr.wandState.rayOrigin.y, vr.wandState.rayOrigin.z);

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", T(TKEY("wand_ray_direction"), "Ray Direction"));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("(%.2f, %.2f, %.2f)", vr.wandState.rayDirection.x, vr.wandState.rayDirection.y, vr.wandState.rayDirection.z);

				ImGui::EndTable();
			}
		}

		if (ImGui::CollapsingHeader(T(TKEY("openvr_addresses_header"), "OpenVR Addresses"))) {
			auto openvr = RE::BSOpenVR::GetSingleton();
			auto overlay = openvr ? RE::BSOpenVR::GetIVROverlayFromContext(&openvr->vrContext) : nullptr;
			auto vrSystem = openvr ? openvr->vrSystem : nullptr;
			ADDRESS_NODE(openvr)
			ADDRESS_NODE(overlay)
			ADDRESS_NODE(vrSystem)
		}
	}
}  // namespace

//=============================================================================
// DRAW SETTINGS (main entry point)
//=============================================================================

void VR::DrawSettings()
{
	auto menu = globals::menu;
	if (!menu)
		return;
	if (ImGui::BeginTabBar("##VRTabs", ImGuiTabBarFlags_None)) {
		if (BeginTabItemWithFont(T(TKEY("tab_general"), "General"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##VRGeneralFrame", { 0, 0 }, true)) {
				DrawGeneralVRSettings();
				DrawControllerInputInstructions();
				DrawMenuSettings();
				DrawMouseSettings();
				DrawDragSettings();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (BeginTabItemWithFont(T(TKEY("tab_stereo"), "Stereo"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##VRStereoFrame", { 0, 0 }, true)) {
				DrawStereoSettings();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (IsOpenVRCompatible()) {
			if (BeginTabItemWithFont(T(TKEY("tab_bindings"), "Bindings"), Menu::FontRole::Subheading)) {
				if (ImGui::BeginChild("##VRBindingsFrame", { 0, 0 }, true)) {
					DrawKeyBindings();
				}
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
		}

		if (BeginTabItemWithFont(T(TKEY("tab_debug"), "Debug"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##VRDebugFrame", { 0, 0 }, true)) {
				DrawDebugSection();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}

	// Combo recording popup
	if (this->isCapturingCombo) {
		ImGui::OpenPopup("Record Combo");
		if (auto popup = Util::CenteredPopupModal("Record Combo")) {
			auto GetButtonName = [](uint32_t key) -> const char* {
				switch (key) {
				case static_cast<uint32_t>(RE::BSOpenVRControllerDevice::Keys::kTrigger):
					return "Trigger";
				case static_cast<uint32_t>(RE::BSOpenVRControllerDevice::Keys::kGrip):
					return "Grip";
				case static_cast<uint32_t>(RE::BSOpenVRControllerDevice::Keys::kTouchpadClick):
					return "Touchpad";
				case static_cast<uint32_t>(RE::BSOpenVRControllerDevice::Keys::kJoystickTrigger):
					return "Stick Click";
				case static_cast<uint32_t>(RE::BSOpenVRControllerDevice::Keys::kXA):
					return "A/X";
				case static_cast<uint32_t>(RE::BSOpenVRControllerDevice::Keys::kBY):
					return "B/Y";
				default:
					return "Unknown";
				}
			};

			ImGui::Text(T(TKEY("popup_recording_for"), "Recording combo for: %s"), this->currentComboName ? this->currentComboName : T(TKEY("popup_unknown"), "Unknown"));
			ImGui::Spacing();

			ImGui::TextDisabled("%s", T(TKEY("popup_recording_note"), "(During recording, any controller's buttons can be used. Requirement is only enforced during use.)"));

			ImGui::Spacing();

			double remainingTime = settings.comboTimeout - (Util::GetNowSecs() - this->comboStartTime);
			ImVec4 timerColor = remainingTime > 2.0 ? Util::Colors::GetTimerGood() :
			                    remainingTime > 1.0 ? Util::Colors::GetTimerWarning() :
			                                          Util::Colors::GetTimerCritical();
			ImGui::TextColored(timerColor, T(TKEY("popup_time_remaining"), "Time remaining: %.1f seconds"), remainingTime);

			ImGui::Spacing();

			if (this->recordedCombo.empty()) {
				ImGui::Text("%s", T(TKEY("popup_press_buttons"), "Press buttons to record combo..."));
			} else {
				ImGui::Text("%s", T(TKEY("popup_recorded_buttons"), "Recorded buttons:"));
				std::vector<ButtonCombo> sortedRecordedCombos;
				for (size_t i = 0; i < this->recordedCombo.size(); ++i) {
					sortedRecordedCombos.push_back(this->recordedCombo[i]);
				}
				std::sort(sortedRecordedCombos.begin(), sortedRecordedCombos.end(),
					[](const ButtonCombo& a, const ButtonCombo& b) {
						return a.GetKey() < b.GetKey();
					});

				Util::DrawButtonCombo(sortedRecordedCombos, false);
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			ImGui::Text("%s", T(TKEY("popup_enter_esc"), "Press ENTER to accept, ESC to cancel"));

			// Handle button recording
			bool buttonPressed = false;
			uint32_t pressedKey = 0;
			ControllerDevice pressedDevice = ControllerDevice::Both;

			for (const auto& [keyCode, buttonState] : primaryControllerState.GetActiveButtons()) {
				if (buttonState->isPressed) {
					pressedKey = keyCode;
					buttonPressed = true;
					pressedDevice = ControllerDevice::Primary;
					break;
				}
			}

			if (!buttonPressed) {
				for (const auto& [keyCode, buttonState] : secondaryControllerState.GetActiveButtons()) {
					if (buttonState->isPressed) {
						pressedKey = keyCode;
						buttonPressed = true;
						pressedDevice = ControllerDevice::Secondary;
						break;
					}
				}
			}

			if (buttonPressed) {
				auto it = recordingButtonControllers.find(pressedKey);
				if (it == recordingButtonControllers.end()) {
					recordingButtonControllers[pressedKey] = pressedDevice;
				} else {
					if (it->second != pressedDevice && it->second != ControllerDevice::Both) {
						it->second = ControllerDevice::Both;
					}
				}
				this->recordedCombo.clear();
				for (const auto& [key, device] : recordingButtonControllers) {
					this->recordedCombo.push_back(ButtonCombo(device, key));
				}
			}

			if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
				ApplyRecordedCombo();
				ResetComboRecording();
				ImGui::CloseCurrentPopup();
			}

			if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				ResetComboRecording();
				ImGui::CloseCurrentPopup();
			}

			if (remainingTime <= 0.0) {
				ApplyRecordedCombo();
				ResetComboRecording();
				ImGui::CloseCurrentPopup();
			}
		}
	}
}

#undef I18N_KEY_PREFIX
