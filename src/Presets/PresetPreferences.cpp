#include "PresetPreferences.h"

#include "PresetState.h"
#include "Utils/FileSystem.h"

namespace
{
	std::filesystem::path GetPath()
	{
		return Util::PathHelpers::GetCommunityShaderPath() / "OpenShadersPresetState.json";
	}
}

nlohmann::json PresetPreferences::Restore(nlohmann::json state)
{
	if (!state.is_object())
		state = nlohmann::json::object();
	std::error_code error;
	if (!std::filesystem::exists(GetPath(), error))
		return state;
	const auto saved = Util::FileHelpers::ReadJsonObject(GetPath(), "OS preset preferences", PresetState::kMaxDocumentBytes);
	if (!saved)
		return state;
	return PresetState::RestorePrivateState(std::move(state), *saved);
}

bool PresetPreferences::Save(const nlohmann::json& state)
{
	std::error_code error;
	const bool exists = std::filesystem::exists(GetPath(), error);
	if (error || (exists && !Util::FileHelpers::ReadJsonObject(GetPath(), "OS preset preferences", PresetState::kMaxDocumentBytes)))
		return false;
	nlohmann::json preferences = nlohmann::json::object();
	for (const auto key : PresetState::kPrivateKeys)
		if (state.contains(key))
			preferences[key] = state.at(key);
	return Util::FileHelpers::WriteJsonAtomically(GetPath(), preferences, 2, "OS preset preferences");
}
