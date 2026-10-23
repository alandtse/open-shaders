#include "BrowserWidgets.h"

#include "../EditorWindow.h"
#include "Menu/Fonts.h"
#include "Utils/UI.h"
#include <algorithm>
#include <imgui_internal.h>

namespace
{
	constexpr float kSectionLabelGap = 8.0f;
	constexpr float kEmptyStateTopPad = 24.0f;
	ImVec2 TextCursor(ImGuiWindow* window)
	{
		return ImVec2(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
	}

	void DrawClippedText(ImDrawList* drawList, ImVec2 pos, float width, const char* text, const ImVec2& size,
		const ImVec4& color)
	{
		if (size.x <= width) {
			drawList->AddText(pos, ImGui::GetColorU32(color), text);
			return;
		}
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::RenderTextEllipsis(drawList, pos, ImVec2(pos.x + width, pos.y + size.y), pos.x + width, text, nullptr, &size);
		ImGui::PopStyleColor();
	}

}

namespace BrowserUI
{
	void SameLineIfFits(float width)
	{
		ImGui::SameLine();
		if (ImGui::GetContentRegionAvail().x < width)
			ImGui::NewLine();
	}

	void EllipsizedText(const char* text, float maxWidth, const ImVec4& color)
	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (window->SkipItems)
			return;
		if (!text)
			text = "";
		if (maxWidth <= 0.0f)
			maxWidth = ImGui::GetContentRegionAvail().x;
		maxWidth = std::max(maxWidth, 1.0f);

		const ImVec2 size = ImGui::CalcTextSize(text);
		const ImVec2 pos = TextCursor(window);
		const float width = std::min(size.x, maxWidth);
		const ImRect bb(pos, ImVec2(pos.x + width, pos.y + size.y));
		ImGui::ItemSize(ImVec2(width, size.y), 0.0f);
		if (!ImGui::ItemAdd(bb, 0))
			return;

		DrawClippedText(window->DrawList, pos, width, text, size, color);
		if (size.x > maxWidth && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			ImGui::SetTooltip("%s", text);
	}

	void MetaText(const char* text, float maxWidth)
	{
		EllipsizedText(text, maxWidth, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	}

	void SectionLabel(const char* label, bool alignRight)
	{
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const float right = start.x + ImGui::GetContentRegionAvail().x;
		if (alignRight)
			ImGui::SetCursorScreenPos(ImVec2(std::max(start.x, right - ImGui::CalcTextSize(label).x), start.y));
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextUnformatted(label);
		ImGui::PopStyleColor();

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const float gap = kSectionLabelGap * Util::GetUIScale();
		const float x0 = alignRight ? start.x : max.x + gap;
		const float x1 = alignRight ? min.x - gap : right;
		if (x0 < x1) {
			const float y = (min.y + max.y) * 0.5f;
			ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y), ImVec2(x1, y), ImGui::GetColorU32(ImGuiCol_Separator));
		}
	}

	void EmptyState(const char* title, const char* hint)
	{
		ImGui::Dummy(ImVec2(0.0f, kEmptyStateTopPad * Util::GetUIScale()));
		const float avail = ImGui::GetContentRegionAvail().x;
		const float wrapPosX = ImGui::GetCursorPosX() + avail;
		auto centredLine = [avail, wrapPosX](const char* text, const ImVec4& color) {
			const float width = ImGui::CalcTextSize(text).x;
			if (width < avail)
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width) * 0.5f);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::PushTextWrapPos(wrapPosX);
			ImGui::TextUnformatted(text);
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		};
		if (title)
			centredLine(title, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		if (hint)
			centredLine(hint, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	}

	void PageHeader(Icons::GlyphRef icon, const char* title, const char* subtitle)
	{
		ImGui::AlignTextToFramePadding();
		if (icon) {
			Icons::FontGuard font(icon);
			ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetInfo());
			ImGui::TextUnformatted(icon.utf8);
			ImGui::PopStyleColor();
			ImGui::SameLine();
		}
		{
			MenuFonts::FontRoleGuard heading(Menu::FontRole::Subheading);
			ImGui::TextUnformatted(title);
		}
		if (subtitle && subtitle[0]) {
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextUnformatted(subtitle);
			ImGui::PopStyleColor();
		}
	}

	float RowHeight()
	{
		return ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
	}

	void StackedSplit(const char* id, float& topRatio, float defaultTopRatio, const PaneDrawer& drawTop, const PaneDrawer& drawBottom)
	{
		constexpr float kSplitterThickness = 6.0f;
		constexpr float kMinPaneHeight = 80.0f;

		const float scale = Util::GetUIScale();
		if (EditorWindow::GetSingleton()->resetLayout)
			topRatio = defaultTopRatio;
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const float thickness = kSplitterThickness * scale;
		const float minPaneHeight = kMinPaneHeight * scale;
		float topHeight = std::clamp(avail.y * topRatio, minPaneHeight, std::max(minPaneHeight, avail.y - minPaneHeight - thickness));
		drawTop(ImVec2(avail.x, topHeight));

		const ImVec2 splitterPos = ImGui::GetCursorScreenPos();
		float bottomHeight = ImGui::GetContentRegionAvail().y - thickness;
		const ImRect splitterBounds(splitterPos, ImVec2(splitterPos.x + avail.x, splitterPos.y + thickness));
		ImGui::PushID(id);
		if (ImGui::SplitterBehavior(splitterBounds, ImGui::GetID("##StackedSplit"), ImGuiAxis_Y, &topHeight, &bottomHeight,
				minPaneHeight, minPaneHeight, 0.0f, 0.0f, ImGui::GetColorU32(ImGuiCol_Separator)))
			topRatio = topHeight / avail.y;
		ImGui::PopID();
		ImGui::Dummy(ImVec2(avail.x, thickness));
		drawBottom(ImVec2(avail.x, 0.0f));
	}

	void SplitPane(const SplitPaneSpec& spec, float& stackedRatio, const PaneDrawer& drawList, const PaneDrawer& drawDetail)
	{
		const float scale = Util::GetUIScale();
		const bool resetLayout = EditorWindow::GetSingleton()->resetLayout;
		const ImVec2 avail = ImGui::GetContentRegionAvail();

		if (avail.x < spec.besideMinWidth * scale) {
			StackedSplit(spec.id, stackedRatio, spec.defaultStackedRatio, drawList, drawDetail);
			return;
		}

		// The split is a two-column table so the divider is a native, remembered resize handle.
		const auto& style = ImGui::GetStyle();
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x, 0.0f));
		const bool split = ImGui::BeginTable(spec.id, 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, avail);
		ImGui::PopStyleVar();
		if (!split)
			return;
		ImGui::TableSetupColumn("##List", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("##Detail", ImGuiTableColumnFlags_WidthFixed, spec.detailWidth * scale);
		// Must precede TableNextRow: the first row locks the layout, after which widths are ignored.
		if (resetLayout)
			ImGui::TableSetColumnWidth(1, spec.detailWidth * scale);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		drawList(ImVec2(0.0f, ImGui::GetContentRegionAvail().y));
		ImGui::TableSetColumnIndex(1);
		drawDetail(ImVec2(0.0f, 0.0f));
		ImGui::EndTable();
	}

	bool SearchField(const char* id, char* buffer, size_t bufferSize, const char* hint, float width, bool ctrlFFocus)
	{
		if (ctrlFFocus && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F))
			ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(width);
		return ImGui::InputTextWithHint(id, hint, buffer, bufferSize, ImGuiInputTextFlags_EscapeClearsAll);
	}
}
