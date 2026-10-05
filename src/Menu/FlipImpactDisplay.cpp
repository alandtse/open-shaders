#include "FlipImpactDisplay.h"

#include "Feature.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Utils/UI.h"

#include <fstream>

namespace FlipImpactDisplay
{
	namespace
	{
		constexpr auto kTablePath = "Data/SKSE/Plugins/CommunityShaders/FeatureFlipImpact.json";

		const nlohmann::json& Table()
		{
			static const nlohmann::json table = [] {
				std::ifstream file(kTablePath);
				return file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
			}();
			return table;
		}

		const char* Label(Util::FeatureFlipImpact::Tier a_tier)
		{
			using Tier = Util::FeatureFlipImpact::Tier;
			switch (a_tier) {
			case Tier::Low:
				return T("menu.features.compile_impact.low", "Low");
			case Tier::Medium:
				return T("menu.features.compile_impact.medium", "Medium");
			default:
				return T("menu.features.compile_impact.high", "High");
			}
		}
	}

	std::optional<Util::FeatureFlipImpact::Impact> Of(Feature& a_feature)
	{
		return Util::FeatureFlipImpact::Lookup(Table(), globals::game::isVR ? "VR" : "SE", a_feature.GetShaderDefineName());
	}

	ImVec4 Color(Util::FeatureFlipImpact::Tier a_tier)
	{
		using Tier = Util::FeatureFlipImpact::Tier;
		switch (a_tier) {
		case Tier::Low:
			return Util::Colors::GetSuccess();
		case Tier::Medium:
			return Util::Colors::GetWarning();
		default:
			return Util::Colors::GetError();
		}
	}

	std::string Tooltip(const Util::FeatureFlipImpact::Impact& a_impact)
	{
		return I18n::GetSingleton()->Format("menu.features.compile_impact",
			{ { "tier", Label(a_impact.tier) },
				{ "percent", std::to_string(static_cast<int>(a_impact.share * 100.0 + 0.5)) } },
			"Shader compile impact: {tier}. Changing this recompiles about {percent}% of shader compile time.");
	}
}
