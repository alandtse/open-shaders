#include "SettingsPatches.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "Features/Effects11/Effects/Effect.h"
#include "Utils/StringUtils.h"

namespace Util::SettingsPatches
{
	static std::vector<Entry> entries;
	static bool loaded = false;

	void Load()
	{
		entries.clear();
		loaded = true;

		std::filesystem::path path = "Data\\Shaders\\Effects11\\SettingsPatches.json";
		std::ifstream ifs(path);
		if (!ifs.is_open())
			return;

		try {
			nlohmann::json root = nlohmann::json::parse(ifs);
			for (auto& item : root) {
				Entry entry;
				entry.file = item.at("file").get<std::string>();
				for (auto& p : item.at("patches")) {
					Patch patch;
					patch.variable = p.at("variable").get<std::string>();
					patch.value = p.at("value").get<std::string>();
					entry.patches.push_back(std::move(patch));
				}
				entries.push_back(std::move(entry));
			}
		} catch (const std::exception& e) {
			logger::error("[SettingsPatches] Failed to parse {}: {}", path.string(), e.what());
		}

		if (!entries.empty())
			logger::info("[SettingsPatches] Loaded {} entries", entries.size());
	}

	static bool FilenameMatches(const std::string& effectName, const std::string& pattern)
	{
		return Util::IEquals(effectName, pattern);
	}

	void Apply(Effect& effect)
	{
		if (!loaded)
			Load();

		for (auto& entry : entries) {
			if (!FilenameMatches(effect.GetName(), entry.file))
				continue;

			for (auto& patch : entry.patches) {
				bool found = false;
				for (auto& uiVar : effect.uiVariables) {
					if (uiVar.isLabel)
						continue;

					if (uiVar.displayName != patch.variable)
						continue;

					found = true;
					bool patched = false;
					switch (uiVar.type) {
					case Effect::UIVariableType::Float:
						try {
							float val = std::stof(patch.value);
							if (uiVar.effectVariable && SUCCEEDED(uiVar.effectVariable->AsScalar()->SetFloat(val))) {
								uiVar.floatValue = val;
								patched = true;
							}
						} catch (...) {}
						break;
					case Effect::UIVariableType::Int:
						try {
							int val = std::stoi(patch.value);
							if (uiVar.effectVariable && SUCCEEDED(uiVar.effectVariable->AsScalar()->SetInt(val))) {
								uiVar.intValue = val;
								patched = true;
							}
						} catch (...) {}
						break;
					case Effect::UIVariableType::Bool:
						{
							std::string lv = Util::ToLowerAscii(patch.value);
							if (lv == "true" || lv == "1" || lv == "false" || lv == "0") {
								bool val = (lv == "true" || lv == "1");
								if (uiVar.effectVariable && SUCCEEDED(uiVar.effectVariable->AsScalar()->SetBool(val))) {
									uiVar.boolValue = val;
									patched = true;
								}
							}
						}
						break;
					default:
						break;
					}

					if (patched) {
						uiVar.isReadOnly = true;
						uiVar.isPatched = true;
						logger::debug("[SettingsPatches] Patched '{}' in '{}' to '{}'",
							patch.variable, effect.GetName(), patch.value);
					}
					break;
				}
				if (!found) {
					logger::debug("[SettingsPatches] No match for '{}' in '{}'",
						patch.variable, effect.GetName());
				}
			}
		}
	}
}
