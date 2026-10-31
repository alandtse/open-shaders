#pragma once

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>

#include <imgui_internal.h>

namespace Util::ImGuiLayout
{
	inline constexpr unsigned kVersion = 2;

	/** @brief Fits a floating window's complete rectangle inside the viewport work area. */
	inline ImRect ConstrainWindowRect(const ImRect& rectangle, const ImRect& workArea)
	{
		const ImVec2 available(std::max(1.0f, workArea.GetWidth()), std::max(1.0f, workArea.GetHeight()));
		const ImVec2 size(std::clamp(rectangle.GetWidth(), 1.0f, available.x), std::clamp(rectangle.GetHeight(), 1.0f, available.y));
		const ImVec2 position(
			std::clamp(rectangle.Min.x, workArea.Min.x, workArea.Min.x + available.x - size.x),
			std::clamp(rectangle.Min.y, workArea.Min.y, workArea.Min.y + available.y - size.y));
		return ImRect(position, ImVec2(position.x + size.x, position.y + size.y));
	}

	/** @brief Scales a floating window's user size while keeping it inside the viewport. */
	inline ImRect ScaleWindowRect(const ImRect& rectangle, float scaleRatio, const ImRect& workArea)
	{
		IM_ASSERT(scaleRatio > 0.0f);
		return ConstrainWindowRect(ImRect(rectangle.Min,
									   ImVec2(rectangle.Min.x + rectangle.GetWidth() * scaleRatio, rectangle.Min.y + rectangle.GetHeight() * scaleRatio)),
			workArea);
	}

	/** @brief Updates existing floating window geometry before Begin when its effective UI scale changes. */
	inline void PrepareScaledWindow(ImGuiWindow* window, float scale, const ImRect& workArea, bool reset)
	{
		if (!window)
			return;
		const ImGuiID scaleKey = ImHashStr("##EffectiveUIScale", 0, window->ID);
		const float previousScale = window->StateStorage.GetFloat(scaleKey, scale);
		window->StateStorage.SetFloat(scaleKey, scale);
		if (reset || window->DockIsActive)
			return;
		const ImRect rectangle(window->Pos, ImVec2(window->Pos.x + window->SizeFull.x, window->Pos.y + window->SizeFull.y));
		const auto fitted = ScaleWindowRect(rectangle, scale / previousScale, workArea);
		if (scale != previousScale || fitted.GetSize().x != window->SizeFull.x || fitted.GetSize().y != window->SizeFull.y)
			ImGui::SetNextWindowSize(fitted.GetSize(), ImGuiCond_Always);
		if (fitted.Min.x != window->Pos.x || fitted.Min.y != window->Pos.y)
			ImGui::SetNextWindowPos(fitted.Min, ImGuiCond_Always);
	}

	/** @brief Reanchors auto-sized windows until their measured content and window size agree. */
	class AutoResizeAnchor
	{
	public:
		/** @brief Tracks scale and viewport changes without restricting dragging at a stable scale. */
		bool Update(float scale, const ImRect& workArea, bool reset, ImGuiWindow* window, bool waitForAnchor = false)
		{
			const bool changed = reset || scale != previousScale || workArea.Min.x != previousWorkArea.Min.x || workArea.Min.y != previousWorkArea.Min.y ||
			                     workArea.Max.x != previousWorkArea.Max.x || workArea.Max.y != previousWorkArea.Max.y;
			if (changed) {
				previousScale = scale;
				previousWorkArea = workArea;
				settling = true;
			}
			if (window && window->DockIsActive)
				settling = false;
			const bool reanchor = settling;
			if (settling && !changed && window) {
				const ImVec2 nextSize = ImGui::CalcWindowNextAutoFitSize(window);
				settling = waitForAnchor || nextSize.x != window->Size.x || nextSize.y != window->Size.y;
			}
			if (window)
				window->StateStorage.SetBool(ImHashStr("##AutoResizeAnchorSettling", 0, window->ID), settling);
			return reanchor;
		}

		/** @brief Reports whether another auto-sized anchor window is still settling. */
		static bool IsSettling(ImGuiWindow* window)
		{
			return window && window->StateStorage.GetBool(ImHashStr("##AutoResizeAnchorSettling", 0, window->ID));
		}

	private:
		float previousScale = 0.0f;
		ImRect previousWorkArea;
		bool settling = false;
	};

	/** @brief Prepared layout text; current and future versions need no rewrite. */
	struct Migration
	{
		unsigned version = kVersion;
		bool changed = false;
		std::string contents;
	};

	namespace Detail
	{
		template <class Visitor>
		void ForEachLine(std::string_view contents, Visitor&& visit)
		{
			for (std::size_t offset = 0; offset < contents.size();) {
				const auto newline = contents.find('\n', offset);
				const auto length = newline == std::string_view::npos ? contents.size() - offset : newline - offset + 1;
				const auto raw = contents.substr(offset, length);
				auto line = raw;
				while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
					line.remove_suffix(1);
				if (offset == 0 && line.starts_with("\xef\xbb\xbf"))
					line.remove_prefix(3);
				visit(line, raw);
				offset += length;
			}
		}
	}

	/** @brief Drops incompatible window, table, and docking geometry, preserving non-layout sections. */
	inline Migration Prepare(std::string_view contents)
	{
		unsigned loadedVersion = 0;
		bool ownedSection = false;
		Detail::ForEachLine(contents, [&](std::string_view line, std::string_view) {
			if (line.starts_with('['))
				ownedSection = line == "[CommunityShaders][Data]";
			if (!ownedSection || !line.starts_with("LayoutVersion="))
				return;
			const auto value = line.substr(std::string_view("LayoutVersion=").size());
			unsigned version = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), version);
			if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
				loadedVersion = std::max(loadedVersion, version);
		});
		if (loadedVersion >= kVersion)
			return { loadedVersion, false, {} };

		Migration migration;
		migration.changed = true;
		const std::string_view newline = contents.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
		ownedSection = false;
		bool discardSection = false;
		bool wroteVersion = false;
		Detail::ForEachLine(contents, [&](std::string_view line, std::string_view raw) {
			if (line.starts_with('[')) {
				ownedSection = line == "[CommunityShaders][Data]";
				discardSection = line.ends_with(']') && (line.starts_with("[Window][") || line.starts_with("[Table][") || line.starts_with("[Docking]["));
			}
			if (discardSection || (ownedSection && line.starts_with("LayoutVersion=")))
				return;
			migration.contents.append(raw);
			if (ownedSection && line == "[CommunityShaders][Data]" && !wroteVersion) {
				if (!migration.contents.ends_with('\n'))
					migration.contents.append(newline);
				migration.contents.append("LayoutVersion=" + std::to_string(kVersion)).append(newline);
				wroteVersion = true;
			}
		});
		if (!wroteVersion) {
			if (!migration.contents.empty() && !migration.contents.ends_with('\n'))
				migration.contents.append(newline);
			migration.contents.append("[CommunityShaders][Data]").append(newline);
			migration.contents.append("LayoutVersion=" + std::to_string(kVersion)).append(newline);
		}
		return migration;
	}
}
