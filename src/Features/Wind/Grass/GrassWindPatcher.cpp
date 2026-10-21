#include "GrassWindPatcher.h"
#include "GrassWindSettings.h"

#include "Features/Wind/WindMath.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/ModelPath.h"

#include <algorithm>
#include <cctype>
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
		constexpr std::size_t kMaximumProfilesPerFile = 4096;
		constexpr std::size_t kMaximumRulesPerFile = 4096;
		constexpr int kLegacySettingsVersion = 1;
		constexpr int kSettingsVersion = 2;
		constexpr std::string_view kSchemaFileName = "GrassWindSettings.schema.json";
		constexpr std::string_view kAuthoringFileName = "zz_OpenShadersGrassAuthoring.json";

		struct RuntimeProfile
		{
			std::string name;
			Behavior behavior{};
			Behavior persisted{};
			bool persistedDefinition = true;
		};

		struct RuntimeRule
		{
			std::string mesh;
			std::uint32_t profileId = 0;
			bool persisted = true;
		};

		std::vector<RuntimeProfile> runtimeProfiles;
		std::vector<RuntimeRule> runtimeRules;
		std::unordered_map<std::string, std::uint32_t> profileIds;
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
			       ValuesDiffer(a_lhs.flutterSensitivity, a_rhs.flutterSensitivity) ||
			       ValuesDiffer(a_lhs.flutterTipConcentration, a_rhs.flutterTipConcentration);
		}

		Behavior SanitizeBehavior(Behavior a_behavior)
		{
			a_behavior.bendSensitivity = ClampFiniteOrDefault(
				a_behavior.bendSensitivity, GrassWindSettings::kBendSensitivity, 1.0f);
			a_behavior.flutterSensitivity = ClampFiniteOrDefault(
				a_behavior.flutterSensitivity, GrassWindSettings::kFlutterSensitivity, 1.0f);
			a_behavior.flutterTipConcentration = ClampFiniteOrDefault(
				a_behavior.flutterTipConcentration, GrassWindSettings::kFlutterTipConcentration, 1.0f);
			return a_behavior;
		}

		std::filesystem::path GetGrassSettingsPath()
		{
			return Util::PathHelpers::GetWindSettingsPath() / "Grass";
		}

		void ReadBehavior(const nlohmann::json& a_json, Behavior& a_behavior,
			const std::filesystem::path& a_path, std::string_view a_profile)
		{
			const auto read = [&](std::string_view a_key, float& a_value, const GrassWindSettings::Range& a_range) {
				const auto it = a_json.find(a_key);
				if (it == a_json.end())
					return false;
				if (!it->is_number()) {
					logger::warn("[GrassWindPatcher] Ignoring non-numeric '{}' in profile '{}' from {}",
						a_key, a_profile, a_path.string());
					return false;
				}
				const double value = it->get<double>();
				if (!std::isfinite(value)) {
					logger::warn("[GrassWindPatcher] Ignoring non-finite '{}' in profile '{}' from {}",
						a_key, a_profile, a_path.string());
					return false;
				}
				a_value = std::clamp(static_cast<float>(value), a_range.minimum, a_range.maximum);
				return true;
			};

			read("bendSensitivity", a_behavior.bendSensitivity, GrassWindSettings::kBendSensitivity);
			read("flutterSensitivity", a_behavior.flutterSensitivity, GrassWindSettings::kFlutterSensitivity);
			read("flutterTipConcentration",
				a_behavior.flutterTipConcentration, GrassWindSettings::kFlutterTipConcentration);
		}

		void RebuildIndices()
		{
			profileIds.clear();
			for (std::size_t index = 0; index < runtimeProfiles.size(); ++index)
				profileIds.emplace(runtimeProfiles[index].name, static_cast<std::uint32_t>(index + 1));

			ruleIds.clear();
			for (std::size_t index = 0; index < runtimeRules.size(); ++index)
				ruleIds.emplace(runtimeRules[index].mesh, static_cast<std::uint32_t>(index + 1));
		}

		void LoadRules()
		{
			std::scoped_lock lock(runtimeMutex);
			runtimeProfiles.clear();
			runtimeRules.clear();
			profileIds.clear();
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

			std::map<std::string, Behavior> mergedProfiles;
			std::map<std::string, std::string> mergedBindings;
			std::map<std::string, std::filesystem::path> profileSources;
			std::map<std::string, std::filesystem::path> bindingSources;
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
					if (!root.is_object() || (version != kLegacySettingsVersion && version != kSettingsVersion) ||
						!root.contains("profiles") || !root["profiles"].is_object() ||
						!root.contains("grass") || !root["grass"].is_array()) {
						logger::warn("[GrassWindPatcher] Invalid version, profiles, or grass data in {}", path.string());
						continue;
					}
					if (root["profiles"].size() > kMaximumProfilesPerFile || root["grass"].size() > kMaximumRulesPerFile) {
						logger::warn("[GrassWindPatcher] Too many profiles or grass bindings in {}", path.string());
						continue;
					}

					for (const auto& [name, profileJson] : root["profiles"].items()) {
						if (name.empty() || name.size() > 128 || !profileJson.is_object())
							continue;
						if (profileSources.contains(name) && Util::FixFilePath(path.filename().string()) != authoringName)
							conflicts.insert(profileSources[name].filename().string()), conflicts.insert(path.filename().string());
						Behavior behavior = mergedProfiles.contains(name) ? mergedProfiles[name] : Behavior{};
						ReadBehavior(profileJson, behavior, path, name);
						mergedProfiles[name] = SanitizeBehavior(behavior);
						profileSources[name] = path;
					}

					for (const auto& binding : root["grass"]) {
						if (!binding.is_object() || !binding.contains("mesh") || !binding["mesh"].is_string() ||
							!binding.contains("profile") || !binding["profile"].is_string())
							continue;
						const auto mesh = NormalizeModelPath(binding["mesh"].get<std::string>());
						const auto profile = binding["profile"].get<std::string>();
						if (!IsSafeModelPath(mesh) || profile.empty() || profile.size() > 128)
							continue;
						if (bindingSources.contains(mesh) && Util::FixFilePath(path.filename().string()) != authoringName)
							conflicts.insert(bindingSources[mesh].filename().string()), conflicts.insert(path.filename().string());
						mergedBindings[mesh] = profile;
						bindingSources[mesh] = path;
					}
				} catch (const nlohmann::json::exception& exception) {
					logger::error("[GrassWindPatcher] Failed to parse {}: {}", path.string(), exception.what());
				}
			}

			for (const auto& [name, behavior] : mergedProfiles)
				runtimeProfiles.push_back({ name, behavior, behavior, true });
			RebuildIndices();
			for (const auto& [mesh, profile] : mergedBindings) {
				const auto profileIt = profileIds.find(profile);
				if (profileIt == profileIds.end()) {
					logger::warn("[GrassWindPatcher] Mesh {} references missing profile '{}'", mesh, profile);
					continue;
				}
				runtimeRules.push_back({ mesh, profileIt->second, true });
				detectedMeshes.insert(mesh);
			}
			RebuildIndices();
			conflictingFiles.assign(conflicts.begin(), conflicts.end());
			logger::info("[GrassWindPatcher] Loaded {} profiles and {} mesh bindings from {} JSON files",
				runtimeProfiles.size(), runtimeRules.size(), paths.size());
		}

		std::string MakeProfileName(const std::string& a_mesh)
		{
			std::string name = std::filesystem::path(a_mesh).stem().string();
			for (char& character : name) {
				if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_' && character != '-')
					character = '_';
			}
			if (name.empty())
				name = "grass";
			std::string candidate = name;
			for (std::uint32_t suffix = 2; profileIds.contains(candidate); ++suffix)
				candidate = std::format("{}_{}", name, suffix);
			return candidate;
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
		const auto& rule = runtimeRules[ruleIt->second - 1];
		return rule.profileId > 0 && rule.profileId <= runtimeProfiles.size() ?
		           runtimeProfiles[rule.profileId - 1].behavior :
		           Behavior{};
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
		if (rule.profileId == 0 || rule.profileId > runtimeProfiles.size())
			return {};
		const auto& profile = runtimeProfiles[rule.profileId - 1];
		return { static_cast<std::uint32_t>(a_index + 1), rule.mesh, profile.name, profile.behavior,
			!rule.persisted || ValuesDiffer(profile.behavior, profile.persisted) };
	}

	bool SetRule(std::size_t a_index, const Behavior& a_behavior)
	{
		std::scoped_lock lock(runtimeMutex);
		if (a_index >= runtimeRules.size())
			return false;
		const auto profileId = runtimeRules[a_index].profileId;
		if (profileId == 0 || profileId > runtimeProfiles.size())
			return false;
		runtimeProfiles[profileId - 1].behavior = SanitizeBehavior(a_behavior);
		return true;
	}

	std::size_t GetUnsavedRuleCount()
	{
		std::scoped_lock lock(runtimeMutex);
		return static_cast<std::size_t>(std::ranges::count_if(runtimeRules, [](const RuntimeRule& a_rule) {
			if (!a_rule.persisted || a_rule.profileId == 0 || a_rule.profileId > runtimeProfiles.size())
				return !a_rule.persisted;
			const auto& profile = runtimeProfiles[a_rule.profileId - 1];
			return ValuesDiffer(profile.behavior, profile.persisted);
		}));
	}

	void RevertUnsavedChanges()
	{
		std::scoped_lock saveLock(saveMutex);
		std::scoped_lock lock(runtimeMutex);
		std::erase_if(runtimeRules, [](const RuntimeRule& a_rule) { return !a_rule.persisted; });
		std::erase_if(runtimeProfiles, [](const RuntimeProfile& a_profile) { return !a_profile.persistedDefinition; });
		for (auto& profile : runtimeProfiles)
			profile.behavior = profile.persisted;
		RebuildIndices();
	}

	SaveResult SaveRules()
	{
		std::scoped_lock saveLock(saveMutex);
		std::vector<RuntimeProfile> snapshotProfiles;
		std::vector<RuntimeRule> snapshotRules;
		{
			std::scoped_lock lock(runtimeMutex);
			snapshotProfiles = runtimeProfiles;
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
			{ "profiles", nlohmann::json::object() },
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
			(root["version"] != kLegacySettingsVersion && root["version"] != kSettingsVersion)) {
			result.error = std::format("Invalid grass authoring JSON version in {}", path.string());
			return result;
		}
		if (!root.contains("profiles") || !root["profiles"].is_object() ||
			!root.contains("grass") || !root["grass"].is_array()) {
			result.error = std::format("Invalid profiles or grass data in {}", path.string());
			return result;
		}

		root["version"] = kSettingsVersion;

		for (const auto& profile : snapshotProfiles) {
			if (!profile.persistedDefinition || ValuesDiffer(profile.behavior, profile.persisted)) {
				root["profiles"][profile.name] = {
					{ "bendSensitivity", profile.behavior.bendSensitivity },
					{ "flutterSensitivity", profile.behavior.flutterSensitivity },
					{ "flutterTipConcentration", profile.behavior.flutterTipConcentration }
				};
				++result.savedProfileCount;
			}
		}

		for (const auto& rule : snapshotRules) {
			if (rule.persisted || rule.profileId == 0 || rule.profileId > snapshotProfiles.size())
				continue;
			const auto& profile = snapshotProfiles[rule.profileId - 1];
			auto bindingIt = std::ranges::find_if(root["grass"], [&](const nlohmann::json& a_binding) {
				return a_binding.is_object() && a_binding.contains("mesh") && a_binding["mesh"].is_string() &&
				       NormalizeModelPath(a_binding["mesh"].get<std::string>()) == rule.mesh;
			});
			const nlohmann::json binding{ { "mesh", rule.mesh }, { "profile", profile.name } };
			if (bindingIt == root["grass"].end())
				root["grass"].push_back(binding);
			else
				*bindingIt = binding;
			++result.savedBindingCount;
		}

		if (result.savedProfileCount == 0 && result.savedBindingCount == 0) {
			result.success = true;
			return result;
		}
		if (!Util::FileHelpers::WriteJsonAtomically(path, root, 4, "grass authoring JSON")) {
			result.error = std::format("Failed to write grass authoring JSON: {}", path.string());
			return result;
		}

		{
			std::scoped_lock lock(runtimeMutex);
			for (const auto& snapshot : snapshotProfiles) {
				if (const auto it = profileIds.find(snapshot.name); it != profileIds.end()) {
					auto& profile = runtimeProfiles[it->second - 1];
					profile.persisted = snapshot.behavior;
					profile.persistedDefinition = true;
				}
			}
			for (const auto& snapshot : snapshotRules) {
				if (const auto it = ruleIds.find(snapshot.mesh); it != ruleIds.end())
					runtimeRules[it->second - 1].persisted = true;
			}
		}
		result.success = true;
		result.path = path.string();
		logger::info("[GrassWindPatcher] Saved {} profiles and {} mesh bindings to {}",
			result.savedProfileCount, result.savedBindingCount, result.path);
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
		const auto profileName = MakeProfileName(mesh);
		runtimeProfiles.push_back({ profileName, {}, {}, false });
		const auto profileId = static_cast<std::uint32_t>(runtimeProfiles.size());
		runtimeRules.push_back({ mesh, profileId, false });
		RebuildIndices();
		return true;
	}

	std::vector<std::string> GetConflictingFiles()
	{
		std::scoped_lock lock(runtimeMutex);
		return conflictingFiles;
	}
}
