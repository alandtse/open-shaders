#include "../Wind.h"

#include "I18n/I18n.h"
#include "Utils/UI.h"

#include <algorithm>

namespace
{
	std::string ControlText(const json& a_text)
	{
		const auto fallback = a_text.value("text", std::string{});
		const auto key = a_text.value("key", std::string{});
		return key.empty() ? fallback : I18n::GetSingleton()->Get(key, fallback.c_str());
	}

	bool MatchesCondition(const json& a_condition, const json& a_values, bool a_available)
	{
		if (a_condition.contains("all"))
			return std::ranges::all_of(a_condition.at("all"), [&](const auto& condition) {
				return MatchesCondition(condition, a_values, a_available);
			});
		if (a_condition.contains("any"))
			return std::ranges::any_of(a_condition.at("any"), [&](const auto& condition) {
				return MatchesCondition(condition, a_values, a_available);
			});
		if (a_condition.contains("available"))
			return a_condition.at("available") == a_available;
		if (!a_condition.contains("path"))
			return false;
		const json::json_pointer path(a_condition.at("path").get<std::string>());
		return a_values.contains(path) && a_values.at(path) == a_condition.at("equals");
	}

	void DrawNumericControl(const json& a_control, json& a_values, const json::json_pointer& a_path,
		const std::string& a_label)
	{
		auto& stored = a_values.at(a_path);
		if (!stored.is_number())
			return;
		float value = stored.get<float>();
		float minimum = a_control.at("min").get<float>();
		if (a_control.contains("minimumPath")) {
			const json::json_pointer minimumPath(a_control.at("minimumPath").get<std::string>());
			minimum = std::max(minimum, a_values.at(minimumPath).get<float>());
		}
		const float maximum = a_control.at("max").get<float>();
		const auto format = a_control.value("format", std::string("%.2f"));
		const auto flags = ImGuiSliderFlags_AlwaysClamp |
		                   (a_control.value("logarithmic", false) ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None);
		if (ImGui::SliderFloat(a_label.c_str(), &value, minimum, maximum, format.c_str(), flags)) {
			stored = value;
			if (const auto targets = a_control.find("onChangeClampMinimum"); targets != a_control.end()) {
				for (const auto& target : *targets) {
					auto& dependent = a_values.at(json::json_pointer(target.get<std::string>()));
					dependent = std::max(dependent.get<float>(), value);
				}
			}
		}
	}

	void DrawControls(const json& a_controls, json& a_values, bool a_available)
	{
		int index = 0;
		for (const auto& control : a_controls) {
			ImGui::PushID(index++);
			const SKSE::stl::scope_exit popID([] { ImGui::PopID(); });
			if (control.contains("visibleWhen") && !MatchesCondition(control.at("visibleWhen"), a_values, a_available))
				continue;
			const auto type = control.at("type").get<std::string>();
			const auto label = control.contains("label") ? ControlText(control.at("label")) : std::string{};
			ImGui::BeginDisabled(control.contains("disabledWhen") && MatchesCondition(control.at("disabledWhen"), a_values, a_available));
			const SKSE::stl::scope_exit endDisabled([] { ImGui::EndDisabled(); });
			if (type == "group") {
				const bool tree = control.value("tree", false);
				const auto flags = control.value("defaultOpen", false) ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
				const bool open = !control.value("collapsible", false) ||
				                  (tree ? ImGui::TreeNodeEx(label.c_str(), flags) : ImGui::CollapsingHeader(label.c_str(), flags));
				const SKSE::stl::scope_exit treePop([&] {
					if (open && tree && control.value("collapsible", false))
						ImGui::TreePop();
				});
				if (open)
					DrawControls(control.at("controls"), a_values, a_available);
			} else if (type == "text") {
				if (control.value("disabled", false))
					ImGui::TextDisabled("%s", label.c_str());
				else
					ImGui::TextWrapped("%s", label.c_str());
			} else {
				const json::json_pointer path(control.at("path").get<std::string>());
				if (!a_values.contains(path))
					continue;
				if (type == "checkbox" && a_values.at(path).is_boolean()) {
					bool value = a_values.at(path).get<bool>();
					if (ImGui::Checkbox(label.c_str(), &value))
						a_values.at(path) = value;
				} else if (type == "float") {
					DrawNumericControl(control, a_values, path, label);
				}
			}
			if (control.contains("tooltip")) {
				if (auto tooltip = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted(ControlText(control.at("tooltip")).c_str());
			}
		}
	}
}

void Wind::DrawWindEffectControls()
{
	auto effects = uiSettingsDocument.find("windEffects");
	if (!uiControlsDocument.is_object() || !uiControlsDocument.contains("tabs")) {
		ImGui::TextUnformatted("Open Wind did not supply its effect controls.");
		return;
	}
	if (effects == uiSettingsDocument.end() || !effects->is_object() || !ImGui::BeginTabBar("##WindEffectTabs"))
		return;
	const SKSE::stl::scope_exit endTabs([] { ImGui::EndTabBar(); });
	for (const auto& tab : uiControlsDocument.at("tabs")) {
		try {
			const auto id = tab.at("id").get<std::string>();
			auto entry = effects->find(id);
			if (entry == effects->end() || !entry->is_object())
				continue;
			const auto label = ControlText(tab.at("label"));
			ImGui::PushID(id.c_str());
			const SKSE::stl::scope_exit popID([] { ImGui::PopID(); });
			if (ImGui::BeginTabItem(label.c_str())) {
				const SKSE::stl::scope_exit endTab([] { ImGui::EndTabItem(); });
				uiState.activeWindEffect = id;
				const bool available = !tab.contains("availabilityAction") || RunProviderAction(tab.at("availabilityAction"));
				DrawControls(tab.at("controls"), *entry, available);
			}
		} catch (const json::exception&) {
			ImGui::TextUnformatted("Open Wind supplied an invalid effect control or setting.");
		}
	}
}

void Wind::SpawnDebugWindEffects()
{
	if (RunProviderAction({ { "action", "spawnDebugEffects" }, { "count", uiState.debugWindEffectSpawnCount },
			{ "worstCase", uiState.debugWindEffectWorstCase } })) {
		runtimeState.windFieldDebugView = WindFieldDebugView::TransientImpulses;
		runtimeState.visualizeWindField = true;
	}
}
