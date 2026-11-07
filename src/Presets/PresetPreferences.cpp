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

bool PresetPreferences::Save(const nlohmann::json& state, const std::function<bool()>& saveSettings)
{
	std::error_code error;
	const bool exists = std::filesystem::exists(GetPath(), error);
	const auto previous = exists ? Util::FileHelpers::ReadJsonObject(GetPath(), "OS preset preferences", PresetState::kMaxDocumentBytes) :
	                               std::optional<nlohmann::json>{ nlohmann::json::object() };
	if (error || !previous)
		return false;
	nlohmann::json preferences = nlohmann::json::object();
	for (const auto key : PresetState::kPrivateKeys)
		if (state.contains(key))
			preferences[key] = state.at(key);
	if (!Util::FileHelpers::WriteJsonAtomically(GetPath(), preferences, 2, "OS preset preferences"))
		return false;
	const auto restore = [&] {
		if (exists)
			Util::FileHelpers::WriteJsonAtomically(GetPath(), *previous, 2, "OS preset preferences rollback");
		else
			std::filesystem::remove(GetPath(), error);
	};
	try {
		if (!saveSettings || saveSettings())
			return true;
	} catch (...) {
		restore();
		throw;
	}
	restore();
	return false;
}
