#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
	class BSGeometry;
	class BSMultiStreamInstanceTriShape;
}

namespace GrassWindPatcher
{
	struct Behavior
	{
		float bendSensitivity = 1.0f;
		float flutterSensitivity = 1.0f;
	};

	struct RuleSnapshot
	{
		std::uint32_t id = 0;
		std::string mesh;
		Behavior behavior{};
		bool unsaved = false;
	};

	struct SaveResult
	{
		bool success = false;
		std::size_t savedRuleCount = 0;
		std::string path;
		std::string error;
	};

	/** @brief Loads per-mesh grass settings before source-mesh lifecycle callbacks begin. */
	void LoadAndInstall();
	/** @brief Records the source mesh supplied by the shared grass load hook. */
	void RecordModelPath(RE::BSMultiStreamInstanceTriShape* a_shape, const char* a_modelPath);
	/** @brief Releases the geometry mapping before a grass shape is destroyed. */
	void ForgetShape(RE::BSMultiStreamInstanceTriShape* a_shape);

	/** @brief Returns the settings for a grass shape, or neutral multipliers. */
	[[nodiscard]] Behavior GetBehavior(const RE::BSGeometry* a_geometry);

	/** @brief Returns the number of authored source-mesh rules. */
	[[nodiscard]] std::size_t GetRuleCount();
	/** @brief Returns one source mesh and its settings. */
	[[nodiscard]] RuleSnapshot GetRule(std::size_t a_index);
	/** @brief Updates the settings for one source mesh. */
	[[nodiscard]] bool SetRule(std::size_t a_index, const Behavior& a_behavior);
	/** @brief Returns the number of mesh rules with unsaved changes. */
	[[nodiscard]] std::size_t GetUnsavedRuleCount();
	/** @brief Restores loaded settings and removes rules created since the last save. */
	void RevertUnsavedChanges();
	/** @brief Saves changed mesh rules to the authoring overlay. */
	[[nodiscard]] SaveResult SaveRules();

	/** @brief Returns loaded source meshes that do not yet have authored settings. */
	[[nodiscard]] std::vector<std::string> GetUnconfiguredMeshes();
	/** @brief Creates neutral settings for a detected source mesh. */
	[[nodiscard]] bool AddRule(std::string_view a_mesh);
	/** @brief Returns patch files that define conflicting mesh rules. */
	[[nodiscard]] std::vector<std::string> GetConflictingFiles();
}
