#include "GrassWindPatcher.h"
#include "GrassWindSettings.h"

#include "Features/Wind/WindMath.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/ModelPath.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace GrassWindPatcher
{
	using Util::IsSafeModelPath;
	using Util::NormalizeModelPath;
	using WindMath::ClampFiniteOrDefault;

	namespace
	{
		constexpr std::size_t kMaximumPatchFileSize = 1024 * 1024;
		constexpr std::size_t kMaximumRulesPerFile = 4096;
		constexpr int kSettingsVersion = 2;
		constexpr std::string_view kSchemaFileName = "GrassWindSettings.schema.json";
		constexpr std::string_view kAuthoringFileName = "zz_OpenShadersGrassAuthoring.json";

		struct RuntimeRule
		{
			std::string mesh;
			Behavior behavior{};
			Behavior persisted{};
			bool persistedDefinition = true;
		};

		std::vector<RuntimeRule> runtimeRules;
		std::unordered_map<std::string, std::uint32_t> ruleIds;
		std::unordered_map<const RE::BSGeometry*, std::string> meshByGeometry;
		std::set<std::string> detectedMeshes;
		std::vector<std::string> conflictingFiles;
		std::mutex runtimeMutex;
		std::mutex saveMutex;

		bool ValuesDiffer(float a_lhs, float a_rhs)
		{
			return std::abs(a_lhs - a_rhs) > 0.0001f;
		}

		bool ValuesDiffer(const Behavior& a_lhs, const Behavior& a_rhs)
		{
			return ValuesDiffer(a_lhs.bendSensitivity, a_rhs.bendSensitivity) ||
			       ValuesDiffer(a_lhs.flutterSensitivity, a_rhs.flutterSensitivity);
		}

		Behavior SanitizeBehavior(Behavior a_behavior)
		{
			a_behavior.bendSensitivity = ClampFiniteOrDefault(
				a_behavior.bendSensitivity, GrassWindSettings::kBendSensitivity, 1.0f);
			a_behavior.flutterSensitivity = ClampFiniteOrDefault(
				a_behavior.flutterSensitivity, GrassWindSettings::kFlutterSensitivity, 1.0f);
			return a_behavior;
		}

		std::filesystem::path GetGrassSettingsPath()
		{
			return Util::PathHelpers::GetWindSettingsPath() / "Grass";
		}

		void ReadBehavior(const nlohmann::json& a_json, Behavior& a_behavior,
			const std::filesystem::path& a_path, std::string_view a_mesh)
		{
			const auto read = [&](std::string_view a_key, float& a_value, const GrassWindSettings::Range& a_range) {
				const auto it = a_json.find(a_key);
				if (it == a_json.end())
					return false;
				if (!it->is_number()) {
					logger::warn("[GrassWindPatcher] Ignoring non-numeric '{}' for mesh '{}' from {}",
						a_key, a_mesh, a_path.string());
					return false;
				}
				const double value = it->get<double>();
				if (!std::isfinite(value)) {
					logger::warn("[GrassWindPatcher] Ignoring non-finite '{}' for mesh '{}' from {}",
						a_key, a_mesh, a_path.string());
					return false;
				}
				a_value = std::clamp(static_cast<float>(value), a_range.minimum, a_range.maximum);
				return true;
			};

			read("bendSensitivity", a_behavior.bendSensitivity, GrassWindSettings::kBendSensitivity);
			read("flutterSensitivity", a_behavior.flutterSensitivity, GrassWindSettings::kFlutterSensitivity);
		}

		void RebuildIndices()
		{
			ruleIds.clear();
			for (std::size_t index = 0; index < runtimeRules.size(); ++index)
				ruleIds.emplace(runtimeRules[index].mesh, static_cast<std::uint32_t>(index + 1));
		}

		void LoadRules()
		{
			std::scoped_lock lock(runtimeMutex);
			runtimeRules.clear();
			ruleIds.clear();
			conflictingFiles.clear();

			const auto directory = GetGrassSettingsPath();
			std::error_code error;
			std::filesystem::create_directories(directory, error);
			if (error) {
				logger::warn("[GrassWindPatcher] Failed to create {}: {}", directory.string(), error.message());
				return;
			}

			std::vector<std::filesystem::path> paths;
			for (std::filesystem::directory_iterator iterator(directory, error), end; !error && iterator != end; iterator.increment(error)) {
				if (!iterator->is_regular_file() || Util::FixFilePath(iterator->path().extension().string()) != ".json")
					continue;
				const auto fileName = Util::FixFilePath(iterator->path().filename().string());
				if (fileName == Util::FixFilePath(std::string(kSchemaFileName)) || fileName.ends_with("_backup.json"))
					continue;
				paths.push_back(iterator->path());
			}
			const auto authoringName = Util::FixFilePath(std::string(kAuthoringFileName));
			std::ranges::sort(paths, [&authoringName](const auto& a_lhs, const auto& a_rhs) {
				const bool lhsAuthoring = Util::FixFilePath(a_lhs.filename().string()) == authoringName;
				const bool rhsAuthoring = Util::FixFilePath(a_rhs.filename().string()) == authoringName;
				return lhsAuthoring != rhsAuthoring ? !lhsAuthoring : a_lhs < a_rhs;
			});

			std::map<std::string, Behavior> mergedRules;
			std::map<std::string, std::filesystem::path> ruleSources;
			std::set<std::string> conflicts;

			for (const auto& path : paths) {
				const auto fileSize = std::filesystem::file_size(path, error);
				if (error || fileSize > kMaximumPatchFileSize) {
					logger::warn("[GrassWindPatcher] Skipping unreadable or oversized patch file: {}", path.string());
					error.clear();
					continue;
				}
				std::ifstream input(path, std::ios::binary);
				if (!input)
					continue;

				try {
					const auto root = nlohmann::json::parse(input);
					const auto version = root.is_object() ? root.value("version", 0) : 0;
					if (!root.is_object() || version != kSettingsVersion ||
						!root.contains("grass") || !root["grass"].is_array()) {
						logger::warn("[GrassWindPatcher] Invalid version or grass data in {}", path.string());
						continue;
					}
					if (root["grass"].size() > kMaximumRulesPerFile) {
						logger::warn("[GrassWindPatcher] Too many grass rules in {}", path.string());
						continue;
					}

					for (const auto& entry : root["grass"]) {
						if (!entry.is_object() || !entry.contains("mesh") || !entry["mesh"].is_string())
							continue;
						const auto mesh = NormalizeModelPath(entry["mesh"].get<std::string>());
						if (!IsSafeModelPath(mesh))
							continue;
						if (ruleSources.contains(mesh) && Util::FixFilePath(path.filename().string()) != authoringName)
							conflicts.insert(ruleSources[mesh].filename().string()), conflicts.insert(path.filename().string());
						auto& behavior = mergedRules[mesh];
						ReadBehavior(entry, behavior, path, mesh);
						behavior = SanitizeBehavior(behavior);
						ruleSources[mesh] = path;
					}
				} catch (const nlohmann::json::exception& exception) {
					logger::error("[GrassWindPatcher] Failed to parse {}: {}", path.string(), exception.what());
				}
			}

			for (const auto& [mesh, behavior] : mergedRules) {
				runtimeRules.push_back({ mesh, behavior, behavior, true });
				detectedMeshes.insert(mesh);
			}
			RebuildIndices();
			conflictingFiles.assign(conflicts.begin(), conflicts.end());
			logger::info("[GrassWindPatcher] Loaded {} mesh rules from {} JSON files",
				runtimeRules.size(), paths.size());
		}

	}

	void LoadAndInstall()
	{
		LoadRules();
	}

	void RecordModelPath(RE::BSMultiStreamInstanceTriShape* a_shape, const char* a_modelPath)
	{
		if (!a_shape || !a_modelPath || !*a_modelPath)
			return;
		const auto mesh = NormalizeModelPath(a_modelPath);
		if (!IsSafeModelPath(mesh))
			return;
		std::scoped_lock lock(runtimeMutex);
		meshByGeometry.insert_or_assign(a_shape, mesh);
		detectedMeshes.insert(mesh);
	}

	void ForgetShape(RE::BSMultiStreamInstanceTriShape* a_shape)
	{
		std::scoped_lock lock(runtimeMutex);
		meshByGeometry.erase(a_shape);
	}

	Behavior GetBehavior(const RE::BSGeometry* a_geometry)
	{
		std::scoped_lock lock(runtimeMutex);
		const auto meshIt = meshByGeometry.find(a_geometry);
		if (meshIt == meshByGeometry.end())
			return {};
		const auto ruleIt = ruleIds.find(meshIt->second);
		if (ruleIt == ruleIds.end())
			return {};
		return runtimeRules[ruleIt->second - 1].behavior;
	}

	std::size_t GetRuleCount()
	{
		std::scoped_lock lock(runtimeMutex);
		return runtimeRules.size();
	}

	RuleSnapshot GetRule(std::size_t a_index)
	{
		std::scoped_lock lock(runtimeMutex);
		if (a_index >= runtimeRules.size())
			return {};
		const auto& rule = runtimeRules[a_index];
		return { static_cast<std::uint32_t>(a_index + 1), rule.mesh, rule.behavior,
			!rule.persistedDefinition || ValuesDiffer(rule.behavior, rule.persisted) };
	}

	bool SetRule(std::size_t a_index, const Behavior& a_behavior)
	{
		std::scoped_lock lock(runtimeMutex);
		if (a_index >= runtimeRules.size())
			return false;
		runtimeRules[a_index].behavior = SanitizeBehavior(a_behavior);
		return true;
	}

	std::size_t GetUnsavedRuleCount()
	{
		std::scoped_lock lock(runtimeMutex);
		return static_cast<std::size_t>(std::ranges::count_if(runtimeRules, [](const RuntimeRule& a_rule) {
			return !a_rule.persistedDefinition || ValuesDiffer(a_rule.behavior, a_rule.persisted);
		}));
	}

	void RevertUnsavedChanges()
	{
		std::scoped_lock saveLock(saveMutex);
		std::scoped_lock lock(runtimeMutex);
		std::erase_if(runtimeRules, [](const RuntimeRule& a_rule) { return !a_rule.persistedDefinition; });
		for (auto& rule : runtimeRules)
			rule.behavior = rule.persisted;
		RebuildIndices();
	}

	SaveResult SaveRules()
	{
		std::scoped_lock saveLock(saveMutex);
		std::vector<RuntimeRule> snapshotRules;
		{
			std::scoped_lock lock(runtimeMutex);
			snapshotRules = runtimeRules;
		}
		SaveResult result;
		const auto directory = GetGrassSettingsPath();
		std::error_code filesystemError;
		std::filesystem::create_directories(directory, filesystemError);
		if (filesystemError) {
			result.error = std::format("Failed to create {}: {}", directory.string(), filesystemError.message());
			return result;
		}

		const auto path = directory / kAuthoringFileName;
		nlohmann::json root{
			{ "$schema", std::string(kSchemaFileName) },
			{ "version", kSettingsVersion },
			{ "grass", nlohmann::json::array() }
		};
		const bool fileExists = std::filesystem::exists(path, filesystemError);
		if (filesystemError) {
			result.error = std::format("Failed to inspect {}: {}", path.string(), filesystemError.message());
			return result;
		}
		if (fileExists) {
			std::ifstream input(path, std::ios::binary);
			if (!input) {
				result.error = std::format("Failed to open {} for reading", path.string());
				return result;
			}
			try {
				root = nlohmann::json::parse(input);
			} catch (const nlohmann::json::exception& exception) {
				result.error = std::format("Failed to parse {}: {}", path.string(), exception.what());
				return result;
			}
		}
		if (!root.is_object() || !root.contains("version") ||
			root["version"] != kSettingsVersion) {
			result.error = std::format("Invalid grass authoring JSON version in {}", path.string());
			return result;
		}
		if (!root.contains("grass") || !root["grass"].is_array()) {
			result.error = std::format("Invalid grass data in {}", path.string());
			return result;
		}

		for (const auto& rule : snapshotRules) {
			if (rule.persistedDefinition && !ValuesDiffer(rule.behavior, rule.persisted))
				continue;
			auto entryIt = std::ranges::find_if(root["grass"], [&](const nlohmann::json& a_entry) {
				return a_entry.is_object() && a_entry.contains("mesh") && a_entry["mesh"].is_string() &&
				       NormalizeModelPath(a_entry["mesh"].get<std::string>()) == rule.mesh;
			});
			const nlohmann::json entry{
				{ "mesh", rule.mesh },
				{ "bendSensitivity", rule.behavior.bendSensitivity },
				{ "flutterSensitivity", rule.behavior.flutterSensitivity }
			};
			if (entryIt == root["grass"].end())
				root["grass"].push_back(entry);
			else
				*entryIt = entry;
			++result.savedRuleCount;
		}

		if (result.savedRuleCount == 0) {
			result.success = true;
			return result;
		}
		if (!Util::FileHelpers::WriteJsonAtomically(path, root, 4, "grass authoring JSON")) {
			result.error = std::format("Failed to write grass authoring JSON: {}", path.string());
			return result;
		}

		{
			std::scoped_lock lock(runtimeMutex);
			for (const auto& snapshot : snapshotRules) {
				if (const auto it = ruleIds.find(snapshot.mesh); it != ruleIds.end()) {
					auto& rule = runtimeRules[it->second - 1];
					rule.persisted = snapshot.behavior;
					rule.persistedDefinition = true;
				}
			}
		}
		result.success = true;
		result.path = path.string();
		logger::info("[GrassWindPatcher] Saved {} mesh rules to {}",
			result.savedRuleCount, result.path);
		return result;
	}

	std::vector<std::string> GetUnconfiguredMeshes()
	{
		std::scoped_lock lock(runtimeMutex);
		std::vector<std::string> meshes;
		for (const auto& mesh : detectedMeshes) {
			if (!ruleIds.contains(mesh))
				meshes.push_back(mesh);
		}
		return meshes;
	}

	bool AddRule(std::string_view a_mesh)
	{
		const auto mesh = NormalizeModelPath(std::string(a_mesh));
		if (!IsSafeModelPath(mesh))
			return false;
		std::scoped_lock lock(runtimeMutex);
		if (ruleIds.contains(mesh))
			return false;
		runtimeRules.push_back({ mesh, {}, {}, false });
		RebuildIndices();
		return true;
	}

	std::vector<std::string> GetConflictingFiles()
	{
		std::scoped_lock lock(runtimeMutex);
		return conflictingFiles;
	}
}
