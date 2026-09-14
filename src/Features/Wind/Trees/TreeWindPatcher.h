#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RE
{
	class BSGeometry;
}

namespace TreeWindPatcher
{
	struct Sensitivities
	{
		float bend = 1.0f;
		float leafAmbient = 1.0f;
		float upperBendRange = 100.0f;
		float maximumDisplacementPercent = 3.0f;
		float trunkGustInfluence = 0.5f;
		float leafGustInfluence = 0.99f;
		float transientWindInfluence = 2.01f;
		float leafTransientWindInfluence = 5.0f;
		float leafTransientFlutterMaximum = 20.0f;
		float transientMaximumBendMultiplier = 2.5f;
		float boundMinimumZ = 0.0f;
		float boundHeight = 0.0f;
		float3 probeBase{};
		float3 probeTop{};
		bool hasBounds = false;
	};

	struct RuleSnapshot
	{
		std::uint32_t id = 0;
		std::string_view mesh;
		float bend = 1.0f;
		float leafAmbient = 1.0f;
		float upperBendRange = 100.0f;
		float maximumDisplacementPercent = 3.0f;
		float trunkGustInfluence = 0.5f;
		float leafGustInfluence = 0.99f;
		float transientWindInfluence = 2.01f;
		float leafTransientWindInfluence = 5.0f;
		float leafTransientFlutterMaximum = 20.0f;
		float transientMaximumBendMultiplier = 2.5f;
		bool unsaved = false;
	};

	struct SaveResult
	{
		bool success = false;
		std::size_t savedRuleCount = 0;
		std::string path;
		std::string error;
	};

	void LoadAndInstall();

	/** @brief a_geometry's model bounds were cached at startup, not recomputed live. */
	[[nodiscard]] Sensitivities GetSensitivities(const RE::BSGeometry* a_geometry);

	[[nodiscard]] std::size_t GetRuleCount();
	[[nodiscard]] RuleSnapshot GetRule(std::size_t a_index);

	[[nodiscard]] bool SetRule(std::size_t a_index, float a_bend, float a_leafAmbient,
		float a_upperBendRange, float a_maximumDisplacementPercent,
		float a_trunkGustInfluence, float a_leafGustInfluence,
		float a_transientWindInfluence, float a_leafTransientWindInfluence,
		float a_leafTransientFlutterMaximum,
		float a_transientMaximumBendMultiplier);

	[[nodiscard]] bool SetRule(std::string_view a_mesh, float a_bend, float a_leafAmbient,
		float a_upperBendRange, float a_maximumDisplacementPercent,
		float a_trunkGustInfluence, float a_leafGustInfluence,
		float a_transientWindInfluence, float a_leafTransientWindInfluence,
		float a_leafTransientFlutterMaximum,
		float a_transientMaximumBendMultiplier);

	void SetUniversalOverride(bool a_enabled, const Sensitivities& a_values);
	[[nodiscard]] std::pair<bool, Sensitivities> GetUniversalOverride();
	void RevertUnsavedChanges();
	[[nodiscard]] SaveResult SaveRules();
	[[nodiscard]] std::size_t GetUnsavedRuleCount();
	[[nodiscard]] std::vector<std::string> GetConflictingFiles();
}
