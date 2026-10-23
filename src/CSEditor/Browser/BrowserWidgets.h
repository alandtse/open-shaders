#pragma once

#include "Menu/Icons/helpers/IconFonts.h"

#include <functional>
#include <imgui.h>

namespace BrowserUI
{
	/** @brief Places a control on the current line when its width fits. */
	void SameLineIfFits(float width);
	/** @brief Draws clipped text with an ellipsis and a tooltip containing the full text. */
	void EllipsizedText(const char* text, float maxWidth, const ImVec4& color);
	/** @brief Draws clipped secondary text. */
	void MetaText(const char* text, float maxWidth = 0.0f);
	/** @brief Draws a muted section label with a rule on the remaining side. */
	void SectionLabel(const char* label, bool alignRight = false);
	/** @brief Draws a centered hint for an empty panel. */
	void EmptyState(const char* title, const char* hint = nullptr);
	/** @brief Draws the title, icon and subtitle of an editor tool. */
	void PageHeader(Icons::GlyphRef icon, const char* title, const char* subtitle);
	/** @brief Draws a tool search input. */
	bool SearchField(const char* id, char* buffer, size_t bufferSize, const char* hint, float width, bool ctrlFFocus);
	/** @brief Returns a row height that fits frame-height controls. */
	float RowHeight();
	using PaneDrawer = std::function<void(const ImVec2&)>;
	/** @brief Responsive dimensions for a resizable list and detail panel. */
	struct SplitPaneSpec
	{
		const char* id;
		float besideMinWidth;
		float detailWidth;
		float defaultStackedRatio;
	};
	/** @brief Draws two panels separated by a draggable horizontal divider. */
	void StackedSplit(const char* id, float& topRatio, float defaultTopRatio, const PaneDrawer& drawTop, const PaneDrawer& drawBottom);
	/** @brief Draws resizable list/detail panels beside each other or stacked in narrow windows. */
	void SplitPane(const SplitPaneSpec& spec, float& stackedRatio, const PaneDrawer& drawList, const PaneDrawer& drawDetail);
}
