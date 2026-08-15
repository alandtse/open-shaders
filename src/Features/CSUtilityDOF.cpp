#include "CSUtility.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "UnderwaterDepthOfField.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>

#define I18N_KEY_PREFIX "feature.cs_utility."

namespace
{
	constexpr float kDofStrengthMin = 0.0f;
	constexpr float kDofStrengthMax = 1.0f;
	constexpr float kDofDistanceMin = 0.0f;
	constexpr float kDofDistanceMax = 50000.0f;
	constexpr float kDofRangeMin = 0.0f;
	constexpr float kDofRangeMax = 50000.0f;
	constexpr float kDofAutoFocusDepthMax = 10000.0f;
	constexpr float kDofAutoFocusBlurMin = 0.0f;
	constexpr float kDofAutoFocusBlurMax = 1.0f;
	constexpr float kDofAutoFocusBlurMultiplierMin = 0.0f;
	constexpr float kDofAutoFocusBlurMultiplierMax = 1.0f;
	constexpr float kDofLockButtonWidth = 120.0f;
	constexpr uint32_t kDofModeMask = 0x3;
	constexpr uint32_t kDofNoSkyFlag = 0x4;
	constexpr uint32_t kDofBlurRadiusShift = 3;
	constexpr uint32_t kDofBlurRadiusMax = 7;
	constexpr uint32_t kDofAutoFocusFlag = 0x80;
	constexpr uint32_t kDofModeBack = 2;
	constexpr float kDofFloatEpsilon = 0.0001f;

	using DofAutoFocusSettings = CSUtility::DepthOfFieldAutoFocusSettings;
	using DofSettings = CSUtility::DepthOfFieldSettings;
	using DofOverride = CSUtility::DepthOfFieldOverride;
	using DepthOfField = RE::ImageSpaceBaseData::DepthOfField;
	using SkyBlurRadius = DepthOfField::SkyBlurRadius;
	using DofAutoFocusMember = float DofAutoFocusSettings::*;

	struct DofAutoFocusSettingDefinition
	{
		const char* settingName;
		DofAutoFocusMember member;
		float defaultValue;
		float minValue;
		float maxValue;
		REL::VariantOffset valueOffset;
	};

	const std::array<DofAutoFocusSettingDefinition, 7> kDofAutoFocusSettingDefinitions{ {
		{ "fDynamicDOFNearDist:Display", &DofAutoFocusSettings::nearDistance, 0.0f, kDofDistanceMin, kDofAutoFocusDepthMax, REL::VariantOffset(0x323369c, 0x338cacc, 0x3485cd8) },
		{ "fDynamicDOFFarDist:Display", &DofAutoFocusSettings::farDistance, 0.0f, kDofDistanceMin, kDofAutoFocusDepthMax, REL::VariantOffset(0x32336a0, 0x338cad0, 0x3485cdc) },
		{ "fDynamicDOFNearRange:Display", &DofAutoFocusSettings::nearRange, 0.0f, kDofRangeMin, kDofAutoFocusDepthMax, REL::VariantOffset(0x32336a4, 0x338cad4, 0x3485ce0) },
		{ "fDynamicDOFFarRange:Display", &DofAutoFocusSettings::farRange, 0.0f, kDofRangeMin, kDofAutoFocusDepthMax, REL::VariantOffset(0x32336a8, 0x338cad8, 0x3485ce4) },
		{ "fDynamicDOFNearBlur:Display", &DofAutoFocusSettings::nearBlur, 0.0f, kDofAutoFocusBlurMin, kDofAutoFocusBlurMax, REL::VariantOffset(0x32336ac, 0x338cadc, 0x3485ce8) },
		{ "fDynamicDOFFarBlur:Display", &DofAutoFocusSettings::farBlur, 0.0f, kDofAutoFocusBlurMin, kDofAutoFocusBlurMax, REL::VariantOffset(0x32336b0, 0x338cae0, 0x3485cec) },
		{ "fDynamicDOFBlurMultiplier:Display", &DofAutoFocusSettings::blurMultiplier, 1.0f, kDofAutoFocusBlurMultiplierMin, kDofAutoFocusBlurMultiplierMax, REL::VariantOffset(0x32336b4, 0x338cae4, 0x3485cf0) },
	} };

	constexpr std::array<uint16_t, 8> kSkyBlurRadiusValues{
		static_cast<uint16_t>(SkyBlurRadius::kRadius0),
		static_cast<uint16_t>(SkyBlurRadius::kRadius1),
		static_cast<uint16_t>(SkyBlurRadius::kRadius2),
		static_cast<uint16_t>(SkyBlurRadius::kRadius3),
		static_cast<uint16_t>(SkyBlurRadius::kRadius4),
		static_cast<uint16_t>(SkyBlurRadius::kRadius5),
		static_cast<uint16_t>(SkyBlurRadius::kRadius6),
		static_cast<uint16_t>(SkyBlurRadius::kRadius7),
	};

	constexpr std::array<uint16_t, 8> kNoSkyBlurRadiusValues{
		static_cast<uint16_t>(SkyBlurRadius::kNoSky_Radius0),
		static_cast<uint16_t>(SkyBlurRadius::kNoSky_Radius1),
		static_cast<uint16_t>(SkyBlurRadius::kNoSky_Radius2),
		static_cast<uint16_t>(SkyBlurRadius::kNoSky_Radius3),
		static_cast<uint16_t>(SkyBlurRadius::kNoSky_Radius4),
		static_cast<uint16_t>(SkyBlurRadius::kNoSky_Radius5),
		static_cast<uint16_t>(SkyBlurRadius::kNoSky_Radius6),
		static_cast<uint16_t>(SkyBlurRadius::kNoSky_Radius7),
	};

	float ClampFiniteOrDefault(float a_value, float a_min, float a_max, float a_default)
	{
		if (!std::isfinite(a_value))
			return a_default;
		return std::clamp(a_value, a_min, a_max);
	}

	uint32_t ClampDofMode(uint32_t a_mode)
	{
		return std::min(a_mode, kDofModeMask);
	}

	uint32_t ClampDofBlurRadius(uint32_t a_radius)
	{
		return std::min(a_radius, kDofBlurRadiusMax);
	}

	float* GetDofAutoFocusValue(const DofAutoFocusSettingDefinition& a_definition)
	{
		const auto address = a_definition.valueOffset.address();
		return address ? reinterpret_cast<float*>(address) : nullptr;
	}

	void SanitizeDepthOfFieldAutoFocusSettings(DofAutoFocusSettings& a_settings)
	{
		for (const auto& definition : kDofAutoFocusSettingDefinitions) {
			a_settings.*definition.member = ClampFiniteOrDefault(
				a_settings.*definition.member,
				definition.minValue,
				definition.maxValue,
				definition.defaultValue);
		}
	}

	uint32_t EncodeDofPackedValue(const DofSettings& a_settings)
	{
		return ClampDofMode(a_settings.mode) |
		       (a_settings.excludeSky ? kDofNoSkyFlag : 0) |
		       (a_settings.autoFocus ? kDofAutoFocusFlag : 0) |
		       (ClampDofBlurRadius(a_settings.blurRadius) << kDofBlurRadiusShift);
	}

	DofSettings DecodeDofPackedValue(float a_strength, float a_distance, float a_range, float a_packedValue)
	{
		uint32_t packedValue = 0;
		if (std::isfinite(a_packedValue) && a_packedValue > 0.0f) {
			packedValue = static_cast<uint32_t>(a_packedValue);
		}

		DofSettings result{};
		result.strength = a_strength;
		result.distance = a_distance;
		result.range = a_range;
		result.mode = ClampDofMode(packedValue & kDofModeMask);
		result.excludeSky = (packedValue & kDofNoSkyFlag) != 0;
		result.autoFocus = (packedValue & kDofAutoFocusFlag) != 0;
		result.blurRadius = ClampDofBlurRadius((packedValue >> kDofBlurRadiusShift) & 0xF);
		CSUtility::SanitizeDepthOfFieldSettings(result);
		return result;
	}

	DofSettings DecodeUnderwaterDepthOfField(const DepthOfField& a_depthOfField)
	{
		DofSettings result{};
		result.strength = a_depthOfField.strength;
		result.distance = a_depthOfField.distance;
		result.range = a_depthOfField.range;
		result.mode = kDofModeBack;

		const uint16_t rawRadius = a_depthOfField.skyBlurRadius.underlying();
		for (uint32_t index = 0; index < kSkyBlurRadiusValues.size(); ++index) {
			if (rawRadius == kSkyBlurRadiusValues[index]) {
				result.excludeSky = false;
				result.blurRadius = index;
				CSUtility::SanitizeDepthOfFieldSettings(result);
				return result;
			}
			if (rawRadius == kNoSkyBlurRadiusValues[index]) {
				result.excludeSky = true;
				result.blurRadius = index;
				CSUtility::SanitizeDepthOfFieldSettings(result);
				return result;
			}
		}

		CSUtility::SanitizeDepthOfFieldSettings(result);
		return result;
	}

	SkyBlurRadius GetUnderwaterSkyBlurRadius(const DofSettings& a_settings)
	{
		const uint32_t blurRadius = ClampDofBlurRadius(a_settings.blurRadius);
		const uint16_t rawRadius = a_settings.excludeSky ? kNoSkyBlurRadiusValues[blurRadius] : kSkyBlurRadiusValues[blurRadius];
		return static_cast<SkyBlurRadius>(rawRadius);
	}

	void WriteUnderwaterFlagsAndRadius(DepthOfField& a_depthOfField, const DofSettings& a_settings)
	{
		a_depthOfField.flags = 0;
		a_depthOfField.skyBlurRadius = GetUnderwaterSkyBlurRadius(a_settings);
	}

	RE::Setting* FindDofSetting(std::string_view a_name)
	{
		if (auto* collection = globals::game::iniSettingCollection) {
			if (auto* setting = collection->GetSetting(a_name))
				return setting;
		}

		if (auto* collection = globals::game::iniPrefSettingCollection) {
			if (auto* setting = collection->GetSetting(a_name))
				return setting;
		}

		if (auto* collection = globals::game::gameSettingCollection) {
			if (auto* setting = collection->GetSetting(a_name.data()))
				return setting;
		}

		return RE::GetINISetting(a_name.data());
	}

	DofAutoFocusSettings ReadDofAutoFocusSettings()
	{
		DofAutoFocusSettings result{};
		for (const auto& definition : kDofAutoFocusSettingDefinitions) {
			if (auto* value = GetDofAutoFocusValue(definition)) {
				result.*definition.member = *value;
			} else if (auto* setting = FindDofSetting(definition.settingName); setting && setting->GetType() == RE::Setting::Type::kFloat) {
				result.*definition.member = setting->data.f;
			} else {
				result.*definition.member = definition.defaultValue;
			}
		}
		SanitizeDepthOfFieldAutoFocusSettings(result);
		return result;
	}

	bool DofAutoFocusSettingsChanged(const DofAutoFocusSettings& a_lhs, const DofAutoFocusSettings& a_rhs)
	{
		for (const auto& definition : kDofAutoFocusSettingDefinitions) {
			if (std::abs(a_lhs.*definition.member - a_rhs.*definition.member) > kDofFloatEpsilon)
				return true;
		}
		return false;
	}

	bool DofSettingsChanged(const DofSettings& a_lhs, const DofSettings& a_rhs)
	{
		return std::abs(a_lhs.strength - a_rhs.strength) > kDofFloatEpsilon ||
		       std::abs(a_lhs.distance - a_rhs.distance) > kDofFloatEpsilon ||
		       std::abs(a_lhs.range - a_rhs.range) > kDofFloatEpsilon ||
		       ClampDofMode(a_lhs.mode) != ClampDofMode(a_rhs.mode) ||
		       a_lhs.excludeSky != a_rhs.excludeSky ||
		       a_lhs.autoFocus != a_rhs.autoFocus ||
		       DofAutoFocusSettingsChanged(a_lhs.autoFocusSettings, a_rhs.autoFocusSettings) ||
		       ClampDofBlurRadius(a_lhs.blurRadius) != ClampDofBlurRadius(a_rhs.blurRadius);
	}

	// underwaterBaseData has no CommonLib GetXxx() accessor unlike
	// data/currentBaseData/overrideBaseData.
	RE::ImageSpaceBaseData* GetUnderwaterBaseData(RE::ImageSpaceManager* a_imageSpaceManager)
	{
		GET_INSTANCE_MEMBER_VRPTR(underwaterBaseData, a_imageSpaceManager);
		return underwaterBaseData;
	}

	std::optional<DofSettings> ReadSceneDepthOfField()
	{
		auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		if (!imageSpaceManager)
			return std::nullopt;

		auto& data = imageSpaceManager->GetImageSpaceData();
		DofSettings result = DecodeDofPackedValue(
			data.modData.data[RE::ImageSpaceModData::kDOFStrength],
			data.modData.data[RE::ImageSpaceModData::kDOFDistance],
			data.modData.data[RE::ImageSpaceModData::kDOFRange],
			data.modData.data[RE::ImageSpaceModData::kDOFMode]);
		result.autoFocusSettings = ReadDofAutoFocusSettings();
		return result;
	}

	std::optional<DofSettings> ReadUnderwaterDepthOfField()
	{
		auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		if (!imageSpaceManager)
			return std::nullopt;

		auto* underwaterBaseData = GetUnderwaterBaseData(imageSpaceManager);
		if (!underwaterBaseData)
			return std::nullopt;

		DofSettings result = DecodeUnderwaterDepthOfField(underwaterBaseData->depthOfField);
		result.autoFocusSettings = ReadDofAutoFocusSettings();
		return result;
	}

	void WriteSceneDepthOfField(RE::ImageSpaceModData& a_modData, const DofSettings& a_settings)
	{
		DofSettings sanitizedSettings = a_settings;
		CSUtility::SanitizeDepthOfFieldSettings(sanitizedSettings);

		a_modData.data[RE::ImageSpaceModData::kDOFStrength] = sanitizedSettings.strength;
		a_modData.data[RE::ImageSpaceModData::kDOFDistance] = sanitizedSettings.distance;
		a_modData.data[RE::ImageSpaceModData::kDOFRange] = sanitizedSettings.range;
		a_modData.data[RE::ImageSpaceModData::kDOFMode] = static_cast<float>(EncodeDofPackedValue(sanitizedSettings));
	}

	void WriteUnderwaterDepthOfField(DepthOfField& a_depthOfField, const DofSettings& a_settings)
	{
		DofSettings sanitizedSettings = a_settings;
		CSUtility::SanitizeDepthOfFieldSettings(sanitizedSettings);
		sanitizedSettings.mode = kDofModeBack;
		sanitizedSettings.autoFocus = false;

		a_depthOfField.strength = sanitizedSettings.strength;
		a_depthOfField.distance = sanitizedSettings.distance;
		a_depthOfField.range = sanitizedSettings.range;
		WriteUnderwaterFlagsAndRadius(a_depthOfField, sanitizedSettings);
	}

	const char* GetDofModeLabel(uint32_t a_mode)
	{
		switch (ClampDofMode(a_mode)) {
		case 0:
			return T(TKEY("dof_mode_front_back"), "Front and Back");
		case 1:
			return T(TKEY("dof_mode_front"), "Front");
		case 2:
			return T(TKEY("dof_mode_back"), "Back");
		case 3:
			return T(TKEY("dof_mode_none"), "None");
		default:
			return T(TKEY("dof_mode_back"), "Back");
		}
	}

	const char* GetDofBlurRadiusLabel(uint32_t a_blurRadius)
	{
		switch (ClampDofBlurRadius(a_blurRadius)) {
		case 0:
			return T(TKEY("dof_blur_radius_0"), "Radius 0");
		case 1:
			return T(TKEY("dof_blur_radius_1"), "Radius 1");
		case 2:
			return T(TKEY("dof_blur_radius_2"), "Radius 2");
		case 3:
			return T(TKEY("dof_blur_radius_3"), "Radius 3");
		case 4:
			return T(TKEY("dof_blur_radius_4"), "Radius 4");
		case 5:
			return T(TKEY("dof_blur_radius_5"), "Radius 5");
		case 6:
			return T(TKEY("dof_blur_radius_6"), "Radius 6");
		case 7:
			return T(TKEY("dof_blur_radius_7"), "Radius 7");
		default:
			return T(TKEY("dof_blur_radius_2"), "Radius 2");
		}
	}

	void DrawDofTooltip(const char* a_text)
	{
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextWrapped("%s", a_text);
		}
	}

	bool DrawDofModeCombo(uint32_t& a_mode)
	{
		const char* modeLabels[] = {
			GetDofModeLabel(0),
			GetDofModeLabel(1),
			GetDofModeLabel(2),
			GetDofModeLabel(3),
		};

		int mode = static_cast<int>(ClampDofMode(a_mode));
		const bool changed = ImGui::Combo(T(TKEY("dof_mode"), "Mode"), &mode, modeLabels, IM_ARRAYSIZE(modeLabels));

		if (!changed)
			return false;

		a_mode = static_cast<uint32_t>(mode);
		return true;
	}

	bool DrawDofBlurRadiusCombo(uint32_t& a_blurRadius)
	{
		const char* blurRadiusLabels[] = {
			GetDofBlurRadiusLabel(0),
			GetDofBlurRadiusLabel(1),
			GetDofBlurRadiusLabel(2),
			GetDofBlurRadiusLabel(3),
			GetDofBlurRadiusLabel(4),
			GetDofBlurRadiusLabel(5),
			GetDofBlurRadiusLabel(6),
			GetDofBlurRadiusLabel(7),
		};

		int blurRadius = static_cast<int>(ClampDofBlurRadius(a_blurRadius));
		const bool changed = ImGui::Combo(T(TKEY("dof_blur_radius"), "Blur Radius"), &blurRadius, blurRadiusLabels, IM_ARRAYSIZE(blurRadiusLabels));

		if (!changed)
			return false;

		a_blurRadius = static_cast<uint32_t>(blurRadius);
		return true;
	}

	ImVec2 GetDepthOfFieldLockButtonSize()
	{
		const float labelWidth = std::max(
			ImGui::CalcTextSize(T(TKEY("dof_unlock"), "Unlock")).x,
			ImGui::CalcTextSize(T(TKEY("dof_lock"), "Lock")).x);
		const float buttonWidth = std::max(
			kDofLockButtonWidth * Util::GetUIScale(),
			labelWidth + ImGui::GetStyle().FramePadding.x * 4.0f);
		return ImVec2(buttonWidth, 0.0f);
	}

	bool DrawDofAutoFocusControls(DofAutoFocusSettings& a_values)
	{
		bool changed = false;
		ImGui::SeparatorText(T(TKEY("dof_auto_focus_settings"), "Auto Focus Settings"));
		DrawDofTooltip(T(TKEY("dof_auto_focus_settings_tooltip"), "Auto Focus uses Skyrim's cached dynamic DOF values. The vanilla Display Depth of Field slider maps to Game Slider / Multiplier."));

		changed |= ImGui::SliderFloat(T(TKEY("dof_auto_near_distance"), "Near Distance"), &a_values.nearDistance, kDofDistanceMin, kDofAutoFocusDepthMax, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		DrawDofTooltip(T(TKEY("dof_auto_near_distance_tooltip"), "Offset before pixels nearer than the sampled focus depth begin to blur."));
		changed |= ImGui::SliderFloat(T(TKEY("dof_auto_far_distance"), "Far Distance"), &a_values.farDistance, kDofDistanceMin, kDofAutoFocusDepthMax, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		DrawDofTooltip(T(TKEY("dof_auto_far_distance_tooltip"), "Offset before pixels farther than the sampled focus depth begin to blur."));
		changed |= ImGui::SliderFloat(T(TKEY("dof_auto_near_range"), "Near Range"), &a_values.nearRange, kDofRangeMin, kDofAutoFocusDepthMax, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		DrawDofTooltip(T(TKEY("dof_auto_near_range_tooltip"), "Fade width for near-side blur. Higher values make the transition into blur more gradual."));
		changed |= ImGui::SliderFloat(T(TKEY("dof_auto_far_range"), "Far Range"), &a_values.farRange, kDofRangeMin, kDofAutoFocusDepthMax, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		DrawDofTooltip(T(TKEY("dof_auto_far_range_tooltip"), "Fade width for far-side blur. Higher values make the transition into blur more gradual."));

		changed |= ImGui::SliderFloat(T(TKEY("dof_auto_near_blur"), "Near Blur"), &a_values.nearBlur, kDofAutoFocusBlurMin, kDofAutoFocusBlurMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		DrawDofTooltip(T(TKEY("dof_auto_near_blur_tooltip"), "Near-side blur amount before the game slider multiplier is applied."));
		changed |= ImGui::SliderFloat(T(TKEY("dof_auto_far_blur"), "Far Blur"), &a_values.farBlur, kDofAutoFocusBlurMin, kDofAutoFocusBlurMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		DrawDofTooltip(T(TKEY("dof_auto_far_blur_tooltip"), "Far-side blur amount before the game slider multiplier is applied."));

		changed |= ImGui::SliderFloat(T(TKEY("dof_auto_blur_multiplier"), "Game Slider / Multiplier"), &a_values.blurMultiplier, kDofAutoFocusBlurMultiplierMin, kDofAutoFocusBlurMultiplierMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		DrawDofTooltip(T(TKEY("dof_auto_blur_multiplier_tooltip"), "Scales Near Blur and Far Blur. This matches Skyrim's Settings > Display > Depth of Field slider and fDynamicDOFBlurMultiplier."));

		if (changed) {
			SanitizeDepthOfFieldAutoFocusSettings(a_values);
		}
		return changed;
	}

	bool DrawDepthOfFieldControls(DofSettings& a_values, bool a_enabled, bool a_allowMode, bool a_allowAutoFocus)
	{
		bool changed = false;
		Util::DisableGuard disabled(!a_enabled);

		if (a_allowAutoFocus) {
			changed |= ImGui::Checkbox(T(TKEY("dof_auto_focus"), "Auto Focus"), &a_values.autoFocus);
			DrawDofTooltip(T(TKEY("dof_auto_focus_tooltip"), "Uses Skyrim's dynamic DOF path, which shifts focus from the sampled scene depth. bDoDepthOfField can still disable the effect, and fDDOFFocusCenterweightExt changes sample weighting."));
		} else {
			a_values.autoFocus = false;
		}

		if (a_values.autoFocus) {
			changed |= DrawDofAutoFocusControls(a_values.autoFocusSettings);
		} else {
			changed |= ImGui::SliderFloat(T(TKEY("dof_strength"), "Strength"), &a_values.strength, kDofStrengthMin, kDofStrengthMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			changed |= ImGui::SliderFloat(T(TKEY("dof_distance"), "Distance"), &a_values.distance, kDofDistanceMin, kDofDistanceMax, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			DrawDofTooltip(T(TKEY("dof_distance_tooltip"), "Static focus distance. Mode decides whether blur applies in front of it, behind it, both, or neither."));
			changed |= ImGui::SliderFloat(T(TKEY("dof_range"), "Range"), &a_values.range, kDofRangeMin, kDofRangeMax, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			DrawDofTooltip(T(TKEY("dof_range_tooltip"), "Static transition width around Distance. Higher values make blur fade in more gradually."));
		}

		if (a_allowMode) {
			changed |= DrawDofModeCombo(a_values.mode);
		} else {
			uint32_t fixedMode = kDofModeBack;
			ImGui::BeginDisabled();
			DrawDofModeCombo(fixedMode);
			ImGui::EndDisabled();
		}

		changed |= ImGui::Checkbox(T(TKEY("dof_exclude_sky"), "Exclude Sky"), &a_values.excludeSky);
		DrawDofTooltip(T(TKEY("dof_exclude_sky_tooltip"), "Sets the No Sky flag so the sky is excluded from the DOF blur pass."));
		changed |= DrawDofBlurRadiusCombo(a_values.blurRadius);

		if (changed) {
			CSUtility::SanitizeDepthOfFieldSettings(a_values);
		}
		return changed;
	}

	void SetDepthOfFieldPopupText(Util::ConfirmationPopup& a_popup, const char* a_id)
	{
		a_popup.title = T(TKEY("dof_lock_popup_title"), "Lock Depth of Field?");
		a_popup.title += "##";
		a_popup.title += a_id;
		a_popup.message = T(TKEY("dof_lock_popup_message"), "Discard the manual depth of field values and lock controls to live image space values?");
		a_popup.confirmLabel = T(TKEY("dof_lock_popup_confirm"), "Discard and Lock");
		a_popup.cancelLabel = T(TKEY("dof_lock_popup_cancel"), "Cancel");
	}

	void DrawDepthOfFieldLockButton(DofOverride& a_override, const std::optional<DofSettings>& a_liveValues, Util::ConfirmationPopup& a_popup)
	{
		const bool hasLiveValues = a_liveValues.has_value();
		const ImVec2 buttonSize = GetDepthOfFieldLockButtonSize();
		if (a_override.locked) {
			if (Util::LockButton(T(TKEY("dof_lock"), "Lock"), buttonSize)) {
				if (DofSettingsChanged(a_override.values, a_override.baseline)) {
					a_popup.Request();
				} else {
					a_override.locked = false;
				}
			}

			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("dof_lock_tooltip"), "Lock depth of field controls to live image space values."));
			}
			return;
		}

		{
			ImGui::BeginDisabled(!hasLiveValues);
			if (Util::UnlockButton(T(TKEY("dof_unlock"), "Unlock"), buttonSize) && hasLiveValues) {
				a_override.values = *a_liveValues;
				a_override.baseline = *a_liveValues;
				a_override.locked = true;
			}
			ImGui::EndDisabled();
		}

		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", hasLiveValues ?
								  T(TKEY("dof_unlock_tooltip"), "Unlock depth of field for manual editing.") :
								  T(TKEY("dof_unlock_unavailable_tooltip"), "No live image space values are available."));
		}
	}

	void DrawDepthOfFieldSection(
		const char* a_label,
		const char* a_id,
		DofOverride& a_override,
		const std::optional<DofSettings>& a_liveValues,
		Util::ConfirmationPopup& a_popup,
		bool a_allowMode,
		bool a_allowAutoFocus,
		const char* a_missingDataText)
	{
		if (!ImGui::TreeNodeEx(a_label, ImGuiTreeNodeFlags_DefaultOpen))
			return;

		ImGui::PushID(a_id);
		SetDepthOfFieldPopupText(a_popup, a_id);
		if (a_popup.Draw()) {
			a_override.locked = false;
		}

		DrawDepthOfFieldLockButton(a_override, a_liveValues, a_popup);

		if (!a_liveValues) {
			Util::TextUnformattedDisabled(a_missingDataText);
		}

		DofSettings displayValues = a_override.locked ? a_override.values : a_liveValues.value_or(a_override.values);
		const bool controlsEnabled = a_override.locked && a_liveValues.has_value();
		if (controlsEnabled) {
			DrawDepthOfFieldControls(a_override.values, true, a_allowMode, a_allowAutoFocus);
		} else {
			DrawDepthOfFieldControls(displayValues, false, a_allowMode, a_allowAutoFocus);
		}

		ImGui::PopID();
		ImGui::TreePop();
	}

	bool IsCurrentUnderwaterImageSpace(RE::ImageSpaceManager* a_imageSpaceManager)
	{
		auto* currentBaseData = a_imageSpaceManager->GetCurrentBaseData();
		auto* underwaterBaseData = GetUnderwaterBaseData(a_imageSpaceManager);
		return underwaterBaseData && currentBaseData == underwaterBaseData;
	}

	class DepthOfFieldOverrideScope
	{
	public:
		explicit DepthOfFieldOverrideScope(CSUtility& a_csUtility)
		{
			if (!a_csUtility.loaded)
				return;

			auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
			if (!imageSpaceManager)
				return;

			const bool currentUnderwater = IsCurrentUnderwaterImageSpace(imageSpaceManager);
			const DofOverride* autoFocusOverride = nullptr;
			if (!currentUnderwater && a_csUtility.settings.sceneDof.locked) {
				autoFocusOverride = &a_csUtility.settings.sceneDof;
			}
			if (autoFocusOverride && autoFocusOverride->values.autoFocus) {
				ApplyAutoFocusOverride(autoFocusOverride->values.autoFocusSettings);
			}

			if (a_csUtility.settings.sceneDof.locked) {
				auto& data = imageSpaceManager->GetImageSpaceData();
				sceneModData = &data.modData;
				sceneBackup = {
					sceneModData->data[RE::ImageSpaceModData::kDOFStrength],
					sceneModData->data[RE::ImageSpaceModData::kDOFDistance],
					sceneModData->data[RE::ImageSpaceModData::kDOFRange],
					sceneModData->data[RE::ImageSpaceModData::kDOFMode],
				};
				WriteSceneDepthOfField(*sceneModData, a_csUtility.settings.sceneDof.values);
			}

			if (a_csUtility.settings.underwaterDof.locked) {
				auto* underwaterBaseData = GetUnderwaterBaseData(imageSpaceManager);
				if (underwaterBaseData) {
					underwaterDepthOfField = &underwaterBaseData->depthOfField;
					underwaterBackup = *underwaterDepthOfField;
					WriteUnderwaterDepthOfField(*underwaterDepthOfField, a_csUtility.settings.underwaterDof.values);
				}
			}
		}

		~DepthOfFieldOverrideScope()
		{
			if (sceneModData) {
				sceneModData->data[RE::ImageSpaceModData::kDOFStrength] = sceneBackup[0];
				sceneModData->data[RE::ImageSpaceModData::kDOFDistance] = sceneBackup[1];
				sceneModData->data[RE::ImageSpaceModData::kDOFRange] = sceneBackup[2];
				sceneModData->data[RE::ImageSpaceModData::kDOFMode] = sceneBackup[3];
			}

			if (underwaterDepthOfField) {
				*underwaterDepthOfField = underwaterBackup;
			}

			for (std::size_t index = 0; index < autoFocusValueCount; ++index) {
				*autoFocusValues[index] = autoFocusBackup[index];
			}
		}

	private:
		void ApplyAutoFocusOverride(const DofAutoFocusSettings& a_settings)
		{
			DofAutoFocusSettings sanitizedSettings = a_settings;
			SanitizeDepthOfFieldAutoFocusSettings(sanitizedSettings);

			for (const auto& definition : kDofAutoFocusSettingDefinitions) {
				auto* value = GetDofAutoFocusValue(definition);
				if (!value)
					continue;

				BackupAutoFocusValue(value);
				*value = sanitizedSettings.*definition.member;
			}
		}

		void BackupAutoFocusValue(float* a_value)
		{
			for (std::size_t index = 0; index < autoFocusValueCount; ++index) {
				if (autoFocusValues[index] == a_value)
					return;
			}

			if (autoFocusValueCount >= autoFocusValues.size())
				return;

			autoFocusValues[autoFocusValueCount] = a_value;
			autoFocusBackup[autoFocusValueCount] = *a_value;
			++autoFocusValueCount;
		}

		RE::ImageSpaceModData* sceneModData = nullptr;
		std::array<float, 4> sceneBackup{};
		DepthOfField* underwaterDepthOfField = nullptr;
		DepthOfField underwaterBackup{};
		std::array<float*, kDofAutoFocusSettingDefinitions.size()> autoFocusValues{};
		std::array<float, kDofAutoFocusSettingDefinitions.size()> autoFocusBackup{};
		std::size_t autoFocusValueCount = 0;
	};

	struct ImageSpaceEffectDepthOfField_IsActive
	{
		static bool thunk(RE::ImageSpaceEffectDepthOfField* a_effect)
		{
			auto& csUtility = globals::features::csUtility;
			DepthOfFieldOverrideScope overrideScope(csUtility);
			return func(a_effect);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ImageSpaceEffectDepthOfField_Render
	{
		static void thunk(RE::ImageSpaceEffectDepthOfField* a_effect, RE::BSTriShape* a_shape, RE::ImageSpaceEffectParam* a_param)
		{
			UnderwaterDepthOfField::BeginRender();
			const SKSE::stl::scope_exit endRender([]() noexcept {
				UnderwaterDepthOfField::EndRender();
			});
			func(a_effect, a_shape, a_param);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ImageSpaceEffectDepthOfField_UpdateParams
	{
		static bool thunk(RE::ImageSpaceEffectDepthOfField* a_effect, RE::ImageSpaceEffectParam* a_param)
		{
			auto& csUtility = globals::features::csUtility;
			DepthOfFieldOverrideScope overrideScope(csUtility);
			const bool result = func(a_effect, a_param);
			UnderwaterDepthOfField::RecordShaderConstants(a_effect, a_param);
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void CSUtility::SanitizeDepthOfFieldSettings(DepthOfFieldSettings& a_settings)
{
	const DepthOfFieldSettings defaults{};
	a_settings.strength = ClampFiniteOrDefault(a_settings.strength, kDofStrengthMin, kDofStrengthMax, defaults.strength);
	a_settings.distance = ClampFiniteOrDefault(a_settings.distance, kDofDistanceMin, kDofDistanceMax, defaults.distance);
	a_settings.range = ClampFiniteOrDefault(a_settings.range, kDofRangeMin, kDofRangeMax, defaults.range);
	a_settings.mode = ClampDofMode(a_settings.mode);
	SanitizeDepthOfFieldAutoFocusSettings(a_settings.autoFocusSettings);
	a_settings.blurRadius = ClampDofBlurRadius(a_settings.blurRadius);
}

void CSUtility::SanitizeDepthOfFieldOverride(DepthOfFieldOverride& a_override)
{
	SanitizeDepthOfFieldSettings(a_override.values);
	SanitizeDepthOfFieldSettings(a_override.baseline);
}

void CSUtility::DrawDepthOfFieldSettings()
{
	if (ImGui::BeginTabItem(T(TKEY("tab_vanilla_dof"), "Vanilla DOF"))) {
		static Util::ConfirmationPopup sceneLockPopup;
		static Util::ConfirmationPopup underwaterLockPopup;

		DrawDepthOfFieldSection(
			T(TKEY("dof_scene"), "Scene"),
			"Scene",
			settings.sceneDof,
			ReadSceneDepthOfField(),
			sceneLockPopup,
			true,
			true,
			T(TKEY("dof_scene_no_data"), "No live scene image space data is available."));

		ImGui::Separator();

		DrawDepthOfFieldSection(
			T(TKEY("dof_underwater"), "Underwater"),
			"Underwater",
			settings.underwaterDof,
			ReadUnderwaterDepthOfField(),
			underwaterLockPopup,
			false,
			false,
			T(TKEY("dof_underwater_no_data"), "No underwater image space record is currently applied."));

		ImGui::EndTabItem();
	}
}

void CSUtility::InstallDepthOfFieldHooks()
{
	stl::write_vfunc<0x1, ImageSpaceEffectDepthOfField_Render>(RE::VTABLE_ImageSpaceEffectDepthOfField[0]);
	stl::write_vfunc<0x6, ImageSpaceEffectDepthOfField_IsActive>(RE::VTABLE_ImageSpaceEffectDepthOfField[0]);
	stl::write_vfunc<0x7, ImageSpaceEffectDepthOfField_UpdateParams>(RE::VTABLE_ImageSpaceEffectDepthOfField[0]);
	logger::info("[CSUtility] Installed vanilla depth of field hooks");
}

#undef I18N_KEY_PREFIX
