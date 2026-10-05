#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

/// How much shader compiling turning a feature on or off puts back, read from the table the release build ships.
namespace Util::FeatureFlipImpact
{
	enum class Tier
	{
		Low,
		Medium,
		High
	};

	inline constexpr int kSchemaVersion = 1;

	/// Share of compile time below which a flip is Low, and from which it is High.
	inline constexpr double kLowBelow = 0.10;
	inline constexpr double kHighFrom = 0.50;

	struct Impact
	{
		double share = 0.0;  ///< Fraction of the full compile time a flip recompiles, 0 to 1.
		Tier tier = Tier::Low;
	};

	inline Tier TierFor(double a_share)
	{
		if (a_share < kLowBelow)
			return Tier::Low;
		return a_share < kHighFrom ? Tier::Medium : Tier::High;
	}

	/// Impact of a shader define for one runtime ("SE" or "VR"); empty when the table, runtime or define is missing or malformed.
	inline std::optional<Impact> Lookup(const nlohmann::json& a_table, std::string_view a_runtime, std::string_view a_define)
	{
		if (a_define.empty() || !a_table.is_object() || a_table.value("schemaVersion", 0) != kSchemaVersion)
			return std::nullopt;
		const auto runtimes = a_table.find("runtimes");
		if (runtimes == a_table.end() || !runtimes->is_object())
			return std::nullopt;
		const auto runtime = runtimes->find(std::string(a_runtime));
		if (runtime == runtimes->end() || !runtime->is_object())
			return std::nullopt;
		const auto entry = runtime->find(std::string(a_define));
		if (entry == runtime->end() || !entry->is_object())
			return std::nullopt;
		const auto share = entry->find("share");
		if (share == entry->end() || !share->is_number())
			return std::nullopt;
		const double clamped = std::clamp(share->get<double>(), 0.0, 1.0);
		return Impact{ clamped, TierFor(clamped) };
	}
}
