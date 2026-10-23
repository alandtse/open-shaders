#pragma once

#include <imgui.h>

#include <memory>
#include <vector>

/** Builds a centered, uniformly scaled draw-data copy for the desktop mirror. */
class DesktopMirrorDrawData
{
public:
	struct FitTransform
	{
		double scale;
		double offsetX;
		double offsetY;

		/** Maps one ImGui point into the fitted target pixel canvas. */
		[[nodiscard]] bool MapPosition(ImVec2 position, ImVec2 displayPos, ImVec2 framebufferScale, ImVec2& result) const;
	};

	/** Copies and fits valid source draw data into a target pixel canvas. */
	bool Build(const ImDrawData& source, ImVec2 targetSize);
	/** Returns the pixel-space transform used by Build for source window geometry. */
	[[nodiscard]] static bool GetFitTransform(const ImDrawData& source, ImVec2 targetSize, FitTransform& transform);
	/** Checks whether any command list contains the exact callback pointer. */
	[[nodiscard]] static bool ContainsCallback(const ImDrawData& source, ImDrawCallback callback);
	/** Releases cloned lists and clears the built draw data. */
	void Reset();
	/** Returns the fitted draw data, or null when the last build failed. */
	[[nodiscard]] ImDrawData* Get() { return m_built ? &m_drawData : nullptr; }
	[[nodiscard]] const ImDrawData* Get() const { return m_built ? &m_drawData : nullptr; }

private:
	struct DrawListDeleter
	{
		void operator()(ImDrawList* drawList) const { IM_DELETE(drawList); }
	};

	ImDrawData m_drawData{};
	std::vector<std::unique_ptr<ImDrawList, DrawListDeleter>> m_clonedLists;
	bool m_built = false;
};
