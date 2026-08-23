#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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
	};

	struct RuleSnapshot
	{
		std::uint32_t id = 0;
		std::string_view mesh;
		float bend = 1.0f;
		float leafAmbient = 1.0f;
		bool unsaved = false;
	};

	struct SaveResult
	{
		bool success = false;
		std::size_t savedRuleCount = 0;
		std::string path;
		std::string error;
	};

	/** @brief Loads tree wind patch files and installs the model-creation hook when rules exist. */
	void LoadAndInstall();

	/** @brief Reads live model-specific tree wind sensitivity metadata for a geometry. */
	[[nodiscard]] Sensitivities GetSensitivities(const RE::BSGeometry* a_geometry);

	/** @return Number of mesh rules currently available for live tuning. */
	[[nodiscard]] std::size_t GetRuleCount();

	/** @return A stable snapshot for the requested zero-based rule index. */
	[[nodiscard]] RuleSnapshot GetRule(std::size_t a_index);

	/** @brief Applies live sensitivity values to a rule. */
	[[nodiscard]] bool SetRule(std::size_t a_index, float a_bend, float a_leafAmbient);

	/** @brief Applies live sensitivity values to an exact normalized mesh path. */
	[[nodiscard]] bool SetRule(std::string_view a_mesh, float a_bend, float a_leafAmbient);

	/** @brief Restores all live values to their last saved values. */
	void RevertUnsavedChanges();

	/** @brief Writes all live rule values to the editable tree wind JSON. */
	[[nodiscard]] SaveResult SaveRules();

	/** @brief Restores the editable JSON and live values from its startup backup. */
	[[nodiscard]] SaveResult RestoreBackup();

	/** @return Number of rules changed since the editable JSON was loaded or saved. */
	[[nodiscard]] std::size_t GetUnsavedRuleCount();
}
