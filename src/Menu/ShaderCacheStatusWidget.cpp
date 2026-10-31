#include "Menu/ShaderCacheStatusWidget.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>

#include <imgui.h>

#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu/ThemeManager.h"
#include "ShaderCache.h"
#include "Util.h"

namespace ShaderCacheStatusWidget
{
	namespace
	{
		struct Segment
		{
			uint64_t count;
			ImU32 color;
			const char* label;
		};

		/** @brief Nonoverlapping duration buckets from the current cache generation. */
		std::array<Segment, 3> GetSegments()
		{
			const auto durations = globals::shaderCache->GetTaskDurationStats();
			return { {
				{ durations.fast, ThemeManager::Constants::SHADER_BAR_FAST, T("menu.settings.shader_fast", "Fast (<2s)") },
				{ durations.slow, ThemeManager::Constants::SHADER_BAR_SLOW, T("menu.settings.shader_slow", "Slow (2-8s)") },
				{ durations.verySlow, ThemeManager::Constants::SHADER_BAR_VERY_SLOW, T("menu.settings.shader_very_slow", "Very slow (>=8s)") },
			} };
		}
	}

	void DrawBreakdownBar()
	{
		const auto segments = GetSegments();
		uint64_t total = 0;
		for (const auto& segment : segments)
			total += segment.count;
		if (total == 0)
			return;

		const float barHeight = ThemeManager::Constants::SHADER_BREAKDOWN_BAR_HEIGHT * Util::GetUIScale();
		const float barWidth = ImGui::GetContentRegionAvail().x;
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		drawList->AddRectFilled(cursor, ImVec2(cursor.x + barWidth, cursor.y + barHeight), ImGui::GetColorU32(ThemeManager::Constants::SHADER_BAR_BACKGROUND));

		float x = cursor.x;
		uint64_t bucketEnd = 0;
		for (const auto& seg : segments) {
			if (seg.count == 0)
				continue;
			bucketEnd += seg.count;
			const float endX = cursor.x + static_cast<float>(bucketEnd) / static_cast<float>(total) * barWidth;
			drawList->AddRectFilled(ImVec2(x, cursor.y), ImVec2(endX, cursor.y + barHeight), ImGui::GetColorU32(seg.color));
			x = endX;
		}

		ImGui::Dummy(ImVec2(barWidth, barHeight));
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(T("menu.settings.shader_task_duration_tooltip",
				"CPU task wall time includes cache work and shader-object creation.\n"
				"Queue wait and untimed standalone tasks are excluded. This is not GPU timing.\n"
				"Percentages use timed tasks in the current cache generation."));
			for (const auto& seg : segments) {
				if (seg.count == 0)
					continue;
				const float pct = 100.0f * static_cast<float>(seg.count) / static_cast<float>(total);
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(seg.color), "%s: %llu (%.1f%%)", seg.label, seg.count, pct);
			}
			ImGui::EndTooltip();
		}
	}

	void DrawBreakdownLegend()
	{
		const uint64_t total = globals::shaderCache->GetTaskDurationStats().GetTotal();
		if (total == 0)
			return;
		const ImGuiStyle& style = ImGui::GetStyle();
		MenuFonts::FontRoleGuard subtext(Menu::FontRole::Subtext);
		const float swatch = ImGui::GetTextLineHeight() * 0.6f;
		const float startX = ImGui::GetCursorScreenPos().x;
		const float rightEdge = startX + ImGui::GetContentRegionAvail().x;
		bool first = true;
		for (const auto& seg : GetSegments()) {
			if (seg.count == 0)
				continue;
			const std::string text = std::format("{} {}", seg.label, seg.count);
			const float width = swatch + style.ItemInnerSpacing.x + ImGui::CalcTextSize(text.c_str()).x;
			// Entries flow left to right and wrap at the content edge
			if (!first && ImGui::GetItemRectMax().x + style.ItemSpacing.x * 2.0f + width <= rightEdge)
				ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
			first = false;
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			const float y = pos.y + (ImGui::GetTextLineHeight() - swatch) * 0.5f;
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + swatch, y + swatch), ImGui::GetColorU32(seg.color), swatch * 0.25f);
			ImGui::Dummy(ImVec2(swatch, ImGui::GetTextLineHeight()));
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			ImGui::TextDisabled("%s", text.c_str());
		}
	}
}
