#pragma once

#include "Utils/FeatureFlipImpact.h"

#include <imgui.h>

#include <optional>
#include <string>

struct Feature;

/// How the feature toggles present the compile cost of a flip (tier color and tooltip text).
namespace FlipImpactDisplay
{
	/// Impact for the running runtime; empty when the table or the feature's define is missing.
	std::optional<Util::FeatureFlipImpact::Impact> Of(Feature& a_feature);

	/// Theme color for a tier: success for Low, warning for Medium, error for High.
	ImVec4 Color(Util::FeatureFlipImpact::Tier a_tier);

	/// One-line, localized explanation of the tier and the share of compile time a flip recompiles.
	std::string Tooltip(const Util::FeatureFlipImpact::Impact& a_impact);
}
