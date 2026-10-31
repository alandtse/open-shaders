#include "ABTesting.h"

#include "Feature.h"
#include "I18n/I18n.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/SettingsControl.h"
#include "Utils/UI.h"

namespace
{
	using json = nlohmann::json;

	std::string FormatSettingValue(const SceneSettingsCatalog::SettingMetadata& setting, const json& value)
	{
		if (value.is_number_integer() && setting.choiceDisplay) {
			for (size_t index = 0; index < setting.choiceCount; ++index) {
				if (setting.choices[index] == value.get<std::int64_t>()) {
					const auto& display = setting.choiceDisplay[index];
					const std::string fallback(display.displayName);
					return Util::Settings::StripImGuiId(T(display.displayNameKey, fallback.c_str()));
				}
			}
		}
		if (value.is_number() && setting.numericTransform == SceneSettingsCatalog::NumericTransform::Identity)
			return std::format("{:.4g}", value.get<double>() * setting.displayScale);
		return Util::Settings::FormatValue(value);
	}

	void DrawBenchmarkMenuItem(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const json& currentValue,
		const std::optional<std::vector<ABSettingPrerequisite>>& prerequisites)
	{
		auto* manager = ABTestingManager::GetSingleton();
		const auto key = ABTestingManager::SettingKey(setting);
		if (manager->IsQueued(key)) {
			if (ImGui::MenuItem(T("menu.settings.benchmark_remove", "Remove from benchmark")))
				manager->Unqueue(key);
			return;
		}

		static std::string savedValueKey;
		static std::optional<json> savedValue;
		static std::string restartReason;
		const bool refresh = ImGui::IsWindowAppearing() || savedValueKey != key;
		if (refresh) {
			savedValueKey = key;
			savedValue = manager->SavedSettingValue(feature, setting);
		}

		const auto formatValue = [&setting](const json& value) {
			auto text = FormatSettingValue(setting, value);
			return text.empty() ? value.dump() : text;
		};
		const char* addPrefix = T("menu.settings.benchmark_change", "Add to benchmark:");

		if (!savedValue || *savedValue == currentValue) {
			ImGui::MenuItem(T("menu.settings.benchmark_unchanged", "Add to benchmark (change this setting first)"), nullptr, false, false);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", T("menu.settings.benchmark_unchanged_tooltip",
											"Change this setting first, then add it. The benchmark compares it against its saved value."));
			return;
		}

		const auto from = *savedValue;
		const auto valueText = formatValue(from) + " -> " + formatValue(currentValue);
		auto variantLabel = Util::Settings::GetCatalogLeafDisplayName(setting) + ": " + valueText;
		auto addLabel = std::string(addPrefix) + " " + valueText;
		if (refresh)
			restartReason = manager->SettingRestartReason(feature, setting, currentValue, savedValue);

		if (!restartReason.empty()) {
			addLabel += std::string(" ") + T("menu.settings.benchmark_restart", "(requires restart)");
			ImGui::MenuItem(addLabel.c_str(), nullptr, false, false);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s",
					fmt::format(fmt::runtime(T("menu.settings.benchmark_restart_tooltip",
									"Cannot be measured live: {} needs a restart to apply this, and the benchmark queue does not survive a restart.")),
						restartReason)
						.c_str());
			return;
		}
		if (!prerequisites) {
			addLabel += std::string(" ") + T("menu.settings.benchmark_blocked", "(disabled by another setting)");
			ImGui::MenuItem(addLabel.c_str(), nullptr, false, false);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", T("menu.settings.benchmark_blocked_tooltip",
											"This setting is disabled by another setting that benchmarking cannot turn on automatically. Turn it on yourself, then add this one; the benchmark compares against your current settings."));
			return;
		}
		if (!prerequisites->empty()) {
			std::string names;
			for (const auto& prerequisite : *prerequisites)
				names += (names.empty() ? "" : ", ") + Util::Settings::GetCatalogLeafDisplayName(*prerequisite.setting);
			addLabel += " " + fmt::format(fmt::runtime(T("menu.settings.benchmark_prereq", "(turns on {})")), names);
		}
		if (!ImGui::MenuItem(addLabel.c_str()))
			return;

		std::string error;
		if (!manager->QueueSettingChange(feature, setting, from, currentValue, variantLabel, &error))
			logger::warn("[A/B] {}", error);
	}

	void DrawContextMenu(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, bool disabled)
	{
		auto* manager = ABTestingManager::GetSingleton();
		if (manager->IsEnabled())
			return;
		json settings;
		feature.SaveSettings(settings);
		const auto* current = Util::Settings::GetCatalogSerializedValue(settings, setting);
		if (!current)
			return;
		std::optional<std::vector<ABSettingPrerequisite>> prerequisites{ std::in_place };
		if (disabled) {
			prerequisites = manager->PrerequisitesFor(feature, setting);
			if (setting.enabledByValue < 0)
				prerequisites.reset();
		}
		DrawBenchmarkMenuItem(feature, setting, *current, prerequisites);
	}

	bool IsHighlighted(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return ABTestingManager::GetSingleton()->IsQueued(ABTestingManager::SettingKey(setting));
	}
}

void ABTestingManager::RegisterSettingActions()
{
	SettingsControl::SetActions({ &DrawContextMenu, &IsHighlighted });
}
