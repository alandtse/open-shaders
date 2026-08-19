#include "CloudRelight.h"

#include "../I18n/I18n.h"
#include "CloudShadows.h"

#define I18N_KEY_PREFIX "feature.cloud_relight."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudRelight::Settings,
	enabled,
	cloudRelightMix,
	cloudOriginalMix,
	silverLiningMix,
	silverLiningSpread)

namespace
{
	void ClampSettings(CloudRelight::Settings& settings)
	{
		settings.cloudRelightMix = std::clamp(settings.cloudRelightMix, 0.0f, 2.0f);
		settings.cloudOriginalMix = std::clamp(settings.cloudOriginalMix, 0.0f, 2.0f);
		settings.silverLiningMix = std::clamp(settings.silverLiningMix, 0.0f, 1.0f);
		settings.silverLiningSpread = std::clamp(settings.silverLiningSpread, -0.99f, 0.99f);
	}
}

void CloudRelight::DrawSettings()
{
	bool enable = settings.enabled != 0;
	if (ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &enable))
		settings.enabled = enable;

	ImGui::SeparatorText(T(TKEY("cloud_relighting"), "Cloud Relighting"));

	ImGui::SliderFloat(T(TKEY("vanilla_mix"), "Vanilla Mix"), &settings.cloudOriginalMix, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("vanilla_mix_tooltip"), "Multiplier on the original vanilla cloud color before relighting is applied."));

	ImGui::SliderFloat(T(TKEY("relight_mix"), "Relight Mix"), &settings.cloudRelightMix, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("relight_mix_tooltip"), "Multiplier on the directional light contribution added to clouds."));

	ImGui::SeparatorText(T(TKEY("silver_lining"), "Silver Lining"));

	ImGui::SliderFloat(T(TKEY("silver_lining_accent"), "Silver Lining Accent"), &settings.silverLiningMix, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("silver_lining_accent_tooltip"), "Blend between flat isotropic phase and sharp silver-lining phase lighting."));

	ImGui::SliderFloat(T(TKEY("silver_lining_spread"), "Silver Lining Spread"), &settings.silverLiningSpread, -0.99f, 0.99f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("silver_lining_spread_tooltip"),
							  "Positive: silver lining spreads into thicker cloud areas.\n"
							  "Negative: silver lining is confined to thinner cloud edges."));

	if (!globals::features::cloudShadows.loaded) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", T(TKEY("cloud_shadows_required"), "Cloud self-shadowing requires Cloud Shadows to be installed and enabled."));
	}
}

#undef I18N_KEY_PREFIX

void CloudRelight::LoadSettings(json& o_json)
{
	settings = o_json;
	ClampSettings(settings);
}

void CloudRelight::SaveSettings(json& o_json)
{
	o_json = settings;
}

void CloudRelight::RestoreDefaultSettings()
{
	settings = {};
}

CloudRelight::Settings CloudRelight::GetCommonBufferData() const
{
	return settings;
}
