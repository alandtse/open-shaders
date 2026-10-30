#include "SceneTransitionField.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

#include <imgui.h>
#include <imgui_internal.h>  // GetActiveID
#include <imgui_stdlib.h>

#include "IconsTabler.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "SceneSettingsManager.h"
#include "Utils/UI.h"

namespace
{
	/// Text-metrics basis only: sizing the field for the widest expected value stops it resizing as
	/// digits are typed. It tracks kMaxLocationTransitionSeconds by eye, and drift costs a few pixels.
	constexpr const char* kWidestValue = "300.0";
	constexpr float kResetHoverAlpha = 0.35f;

	/// Live text per field, so a partially typed value survives the frame without being overwritten
	/// by the stored one. One entry per unique id and never removed, as Util::DrawComboSearchInput does.
	std::unordered_map<ImGuiID, std::string> fieldBuffers;

	std::string FormatSeconds(float a_seconds)
	{
		char text[16]{};
		std::snprintf(text, sizeof(text), "%.1f", a_seconds);
		return text;
	}
}

float SceneTransitionField::GetWidth()
{
	return ImGui::CalcTextSize(kWidestValue).x + ImGui::GetStyle().FramePadding.x * 2.0f +
	       ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
}

bool SceneTransitionField::Draw(const char* a_id, std::optional<float>& a_value, float a_inherited, bool a_editable, bool a_mixed, float a_width)
{
	ImGui::PushID(a_id);

	const auto fieldId = ImGui::GetID("##TransitionSeconds");
	auto& buffer = fieldBuffers[fieldId];
	// Re-seeding while the field is active would fight the user mid-edit.
	if (ImGui::GetActiveID() != fieldId)
		buffer = a_mixed ? std::string{} : FormatSeconds(a_value.value_or(a_inherited));

	const auto& style = ImGui::GetStyle();
	const auto start = ImGui::GetCursorScreenPos();
	const float height = ImGui::GetFrameHeight();
	const float width = a_width > 0.0f ? a_width : GetWidth();
	const ImVec2 end(start.x + width, start.y + height);
	ImGui::BeginDisabled(!a_editable);
	ImGui::BeginGroup();
	ImGui::RenderFrame(start, end, ImGui::GetColorU32(ImGuiCol_FrameBg), true, style.FrameRounding);
	ImGui::SetNextItemWidth(std::max(height, width - height));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4());
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	ImGui::InputTextWithHint("##TransitionSeconds", "--", &buffer, ImGuiInputTextFlags_CharsDecimal);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	bool changed = false;
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		std::optional<float> parsed;
		const std::string_view typed{ buffer };
		float seconds = 0.0f;
		bool valid = true;
		if (typed.empty()) {
			// An emptied field means "inherit", which no float widget can express.
			parsed.reset();
		} else if (const auto result = std::from_chars(typed.data(), typed.data() + typed.size(), seconds);
			result.ec == std::errc{} && result.ptr == typed.data() + typed.size()) {
			parsed = std::clamp(seconds, 0.0f, SceneSettingsManager::kMaxLocationTransitionSeconds);
		} else
			valid = false;
		if (valid) {
			changed = a_mixed || parsed != a_value;
			a_value = parsed;
		}
	}
	ImGui::SetCursorScreenPos(ImVec2(end.x - height, start.y));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4());
	auto hover = style.Colors[ImGuiCol_ButtonHovered];
	hover.w *= kResetHoverAlpha;
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, hover);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	if (Icons::Button("##ResetTransition", Icons::TI(ICON_TI_ERASER), ImVec2(height, height))) {
		changed = a_value.has_value() || a_mixed;
		a_value.reset();
		buffer = FormatSeconds(a_inherited);
	}
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(3);
	Util::AddTooltip(T("feature.scene_manager.action.revert", "Revert to default"));
	ImGui::EndGroup();
	ImGui::EndDisabled();

	ImGui::PopID();
	return changed;
}
