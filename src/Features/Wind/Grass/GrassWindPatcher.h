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
		float flutterTipConcentration = 1.0f;
	};

	struct RuleSnapshot
	{
		std::uint32_t id = 0;
		std::string mesh;
		std::string profile;
		Behavior behavior{};
		bool unsaved = false;
	};

	struct SaveResult
	{
		bool success = false;
		std::size_t savedProfileCount = 0;
		std::size_t savedBindingCount = 0;
		std::string path;
		std::string error;
	};

	/** @brief Loads grass response profiles before source-mesh lifecycle callbacks begin. */
	void LoadAndInstall();
	/** @brief Records the source mesh supplied by the shared grass load hook. */
	void RecordModelPath(RE::BSMultiStreamInstanceTriShape* a_shape, const char* a_modelPath);
	/** @brief Releases the geometry mapping before a grass shape is destroyed. */
	void ForgetShape(RE::BSMultiStreamInstanceTriShape* a_shape);

	/** @brief Returns the response profile bound to a grass shape, or neutral multipliers. */
	[[nodiscard]] Behavior GetBehavior(const RE::BSGeometry* a_geometry);

	/** @brief Returns the number of authored source-mesh bindings. */
	[[nodiscard]] std::size_t GetRuleCount();
	/** @brief Returns one authored source-mesh binding and its effective profile values. */
	[[nodiscard]] RuleSnapshot GetRule(std::size_t a_index);
	/** @brief Updates the reusable profile referenced by one source-mesh binding. */
	[[nodiscard]] bool SetRule(std::size_t a_index, const Behavior& a_behavior);
	/** @brief Returns the number of bindings with unsaved profile or binding changes. */
	[[nodiscard]] std::size_t GetUnsavedRuleCount();
	/** @brief Restores loaded profile values and removes bindings created since the last save. */
	void RevertUnsavedChanges();
	/** @brief Saves changed profiles and newly authored bindings to the authoring overlay. */
	[[nodiscard]] SaveResult SaveRules();

	/** @brief Returns loaded source meshes that do not yet have an authored binding. */
	[[nodiscard]] std::vector<std::string> GetUnconfiguredMeshes();
	/** @brief Creates a neutral reusable profile and binding for a detected source mesh. */
	[[nodiscard]] bool AddRule(std::string_view a_mesh);
	/** @brief Returns patch files that define conflicting profiles or mesh bindings. */
	[[nodiscard]] std::vector<std::string> GetConflictingFiles();
}
