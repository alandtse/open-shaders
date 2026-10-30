#include "UI.h"

#include "../I18n/I18n.h"
#include "D3D.h"
#include "Feature.h"
#include "FileSystem.h"
#include "Format.h"
#include "IconsTabler.h"
#include "MathUtils.h"
#include "Menu.h"
#include "Menu/FeatureListRenderer.h"
#include "Menu/Fonts.h"
#include "Menu/IconLoader.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/MenuIcons.h"
#include "Menu/ThemeManager.h"
#include "PerfUtils.h"
#include "SceneSettingsManager.h"
#include "SceneSettingsUIHooks.h"
#include "ShaderCache.h"
#include "State.h"
#include "TabLabel.h"
#include "Utils/SettingsControl.h"
#include "Utils/UIAnim.h"
#include "Utils/UISurface.h"

#ifndef DIRECTINPUT_VERSION
#	define DIRECTINPUT_VERSION 0x0800
#endif
#include <DirectXTex.h>
#include <d3d11.h>
#include <dinput.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <wrl/client.h>

#include "../Feature.h"
#include "../Features/VR.h"
#include "../Globals.h"
#include "../Menu.h"
#include "FileSystem.h"
#include "VRUtils.h"

#define STB_IMAGE_IMPLEMENTATION
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <numbers>
#include <sstream>
#include <stb_image.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Util
{
	void DrawEmbeddedFeatureSettings(Feature& feature)
	{
		if (!feature.loaded)
			return;

		ImGui::PushID(feature.GetShortName().c_str());
		const SKSE::stl::scope_exit restoreID([]() noexcept { ImGui::PopID(); });
		SceneSettingsUIHooks::FeatureDrawGuard featureDrawGuard(&feature, false);
		feature.DrawSettings();
	}

	void DrawSelectionButtons(std::span<uint8_t> selected, const char* selectAll, const char* selectNone)
	{
		if (ImGui::SmallButton(selectAll))
			std::ranges::fill(selected, uint8_t{ 1 });
		ImGui::SameLine();
		if (ImGui::SmallButton(selectNone))
			std::ranges::fill(selected, uint8_t{ 0 });
	}
	using C = ThemeManager::Constants;

	static ImVec2 g_screenScaleRatio = { 1.0f, 1.0f };
	static ImVec2 g_displaySize = { 0.0f, 0.0f };

	static int g_lastWindowWidth = 0;
	static int g_lastWindowHeight = 0;
	static thread_local const void* g_activeControlStorageAddress = nullptr;

	class ActiveControlStorageGuard
	{
	public:
		explicit ActiveControlStorageGuard(const void* address) :
			previousAddress(g_activeControlStorageAddress)
		{
			g_activeControlStorageAddress = address;
		}

		~ActiveControlStorageGuard()
		{
			g_activeControlStorageAddress = previousAddress;
		}

	private:
		const void* previousAddress;
	};

	void RefreshScreenScale(HWND hwnd, float bufferWidth, float bufferHeight)
	{
		RECT rect{};
		if (!GetClientRect(hwnd, &rect) || rect.right <= 0 || rect.bottom <= 0)
			return;

		if (rect.right == g_lastWindowWidth && rect.bottom == g_lastWindowHeight)
			return;

		g_displaySize.x = bufferWidth;
		g_displaySize.y = bufferHeight;

		g_screenScaleRatio.x = bufferWidth / static_cast<float>(rect.right);
		g_screenScaleRatio.y = bufferHeight / static_cast<float>(rect.bottom);

		g_lastWindowWidth = rect.right;
		g_lastWindowHeight = rect.bottom;
	}

	void UpdateImGuiInput(HWND hwnd, float bufferWidth, float bufferHeight)
	{
		RefreshScreenScale(hwnd, bufferWidth, bufferHeight);

		auto& io = ImGui::GetIO();
		io.DisplaySize = g_displaySize;

		POINT cursorPos{};
		if (GetCursorPos(&cursorPos) &&
			ScreenToClient(hwnd, &cursorPos)) {
			io.AddMousePosEvent(
				static_cast<float>(cursorPos.x) * g_screenScaleRatio.x,
				static_cast<float>(cursorPos.y) * g_screenScaleRatio.y);
		}
	}

	HoverTooltipWrapper::HoverTooltipWrapper() :
		previousFont(nullptr)
	{
		hovered = ImGui::IsItemHovered(kTooltipWhenDisabled);
		if (hovered) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * ThemeManager::Constants::TOOLTIP_WRAP_FONT_SIZES);
			// Apply Subtext font for consistent tooltip styling
			if (auto* menu = globals::menu) {
				if (auto* subtextFont = menu->GetFont(Menu::FontRole::Subtext)) {
					previousFont = ImGui::GetFont();
					ImGui::PushFont(subtextFont, subtextFont->LegacySize);
				}
			}
		}
	}

	HoverTooltipWrapper::~HoverTooltipWrapper()
	{
		if (hovered) {
			if (previousFont) {
				ImGui::PopFont();
			}
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	bool CloseButton(const char* id, float size)
	{
		const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
		const auto minimum = ImGui::GetItemRectMin();
		const auto maximum = ImGui::GetItemRectMax();
		const bool held = ImGui::IsItemActive() || pressed;
		const bool hovered = ImGui::IsItemHovered();
		const auto color = ImGui::GetColorU32(
			held ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered :
													 ImGuiCol_Button);
		ImGui::RenderFrame(minimum, maximum, color, true, ImGui::GetStyle().FrameRounding);

		const ImVec2 center(
			(minimum.x + maximum.x) * 0.5f,
			(minimum.y + maximum.y) * 0.5f);
		const float scale = GetUIScale();
		const float halfExtent = std::max(ThemeManager::Constants::DIALOG_CLOSE_MIN_EXTENT * scale, size * 0.2f);
		const float stroke = std::max(ThemeManager::Constants::DIALOG_CLOSE_MIN_STROKE * scale, size * 0.1f);
		auto* drawList = ImGui::GetWindowDrawList();
		const auto glyphColor = ImGui::GetColorU32(ImGuiCol_Text);
		drawList->AddLine(
			ImVec2(center.x - halfExtent, center.y - halfExtent),
			ImVec2(center.x + halfExtent, center.y + halfExtent), glyphColor, stroke);
		drawList->AddLine(
			ImVec2(center.x + halfExtent, center.y - halfExtent),
			ImVec2(center.x - halfExtent, center.y + halfExtent), glyphColor, stroke);
		return pressed;
	}

	struct DialogSizeAnimation
	{
		ImVec2 size;
		ImGuiSizeCallback callback = nullptr;
		void* callbackData = nullptr;
	};

	static void AnimateDialogSize(ImGuiSizeCallbackData* data)
	{
		auto& animation = *static_cast<DialogSizeAnimation*>(data->UserData);
		if (animation.callback) {
			data->UserData = animation.callbackData;
			animation.callback(data);
			data->UserData = &animation;
		}
		const float blend = 1.0f - std::exp(-ThemeManager::Constants::DIALOG_RESIZE_RESPONSE * ImGui::GetIO().DeltaTime);
		for (int axis = 0; axis < 2; ++axis) {
			const float difference = data->DesiredSize[axis] - animation.size[axis];
			const float step = std::min(std::abs(difference), std::ceil(std::abs(difference) * blend));
			data->DesiredSize[axis] = animation.size[axis] + std::copysign(step, difference);
		}
	}

	static void PrepareDialogSizeAnimation(ImGuiWindow* window, ImGuiWindowFlags flags, DialogSizeAnimation& animation)
	{
		const auto& context = *GImGui;
		if (!(flags & ImGuiWindowFlags_AlwaysAutoResize) || !window || window->Hidden ||
			window->LastFrameActive != context.FrameCount - 1 || window->AutoFitFramesX > 0 || window->AutoFitFramesY > 0)
			return;
		animation.size = window->SizeFull;
		const bool constrained = context.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSizeConstraint;
		const auto bounds = constrained ? context.NextWindowData.SizeConstraintRect : ImRect(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
		if (constrained) {
			animation.callback = context.NextWindowData.SizeCallback;
			animation.callbackData = context.NextWindowData.SizeCallbackUserData;
		}
		ImGui::SetNextWindowSizeConstraints(bounds.Min, bounds.Max, AnimateDialogSize, &animation);
	}

	static ImGuiWindow* GetOpenDialogWindow(const char* id)
	{
		const auto& context = *GImGui;
		if (context.OpenPopupStack.Size > context.BeginPopupStack.Size) {
			const auto& popup = context.OpenPopupStack[context.BeginPopupStack.Size];
			if (popup.PopupId == ImGui::GetID(id) && popup.OpenFrameCount < context.FrameCount - 1)
				return popup.Window;
		}
		return nullptr;
	}

	static bool BeginDialogPopup(const char* id, const char* title, bool* open, bool modal, ImGuiWindowFlags flags)
	{
		if (!(GImGui->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize))
			ImGui::SetNextWindowSize(ImVec2(ThemeManager::Constants::POPUP_BUTTON_WIDTH * GetUIScale() * 2.0f +
												ImGui::GetStyle().WindowPadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x,
										 0.0f),
				ImGuiCond_Appearing);
		DialogSizeAnimation animation;
		PrepareDialogSizeAnimation(GetOpenDialogWindow(id), flags, animation);
		const bool visible = [&] {
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
			const SKSE::stl::scope_exit restoreStyle([]() noexcept { ImGui::PopStyleColor(); });
			flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;
			return modal ? ImGui::BeginPopupModal(id, open, flags) : ImGui::BeginPopup(id, flags);
		}();
		if (!visible)
			return false;
		const bool closable = !modal || open;
		const float size = ImGui::GetFrameHeight();
		const auto start = ImGui::GetCursorScreenPos();
		const float right = start.x + ImGui::GetContentRegionAvail().x;
		const float textRight = closable ? right - size - ImGui::GetStyle().ItemSpacing.x : right;
		ImGui::PushClipRect(start, ImVec2(std::max(start.x, textRight), start.y + size), true);
		ImGui::AlignTextToFramePadding();
		const std::string_view label(title);
		const auto visibleLabel = label.substr(0, label.find("##"));
		ImGui::TextUnformatted(visibleLabel.data(), visibleLabel.data() + visibleLabel.size());
		ImGui::PopClipRect();
		if (closable) {
			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(right - size, start.y));
			if (CloseButton("##ClosePopup", size)) {
				if (open)
					*open = false;
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return false;
			}
		}
		return true;
	}

	Popup::Popup(const char* id, const char* title, ImGuiWindowFlags flags) :
		isOpen(BeginDialogPopup(id, title, nullptr, false, flags))
	{}

	Popup::~Popup()
	{
		if (isOpen)
			ImGui::EndPopup();
	}

	CenteredPopupModal::CenteredPopupModal(const char* name, bool* p_open, ImGuiWindowFlags flags, ImVec2 pos, ImVec2 pivot)
	{
		if (pos.x == -FLT_MAX && pos.y == -FLT_MAX)
			pos = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
		isOpen = BeginDialogPopup(name, name, p_open, true, flags);
	}

	CenteredPopupModal::~CenteredPopupModal()
	{
		if (isOpen)
			ImGui::EndPopup();
	}

	DisableGuard::DisableGuard(bool disable) :
		disable(disable)
	{
		if (disable)
			ImGui::BeginDisabled();
	}
	DisableGuard::~DisableGuard()
	{
		if (disable)
			ImGui::EndDisabled();
	}

	bool InteractionEndedThisFrame()
	{
		const auto& context = *ImGui::GetCurrentContext();
		if (context.ActiveIdPreviousFrame != 0 && context.ActiveId != context.ActiveIdPreviousFrame)
			return true;
		// NewFrame can clear ActiveId before copying it; stale deactivation data must not split a new active drag.
		if (context.ActiveId == 0 && context.DeactivatedItemData.ID != 0 &&
			context.DeactivatedItemData.ElapseFrame >= context.FrameCount)
			return true;
		for (int button = 0; button < ImGuiMouseButton_COUNT; ++button)
			if (ImGui::IsMouseReleased(button) && (context.ActiveId == 0 ||
													  (context.ActiveIdSource == ImGuiInputSource_Mouse && context.ActiveIdMouseButton == button)))
				return true;
		return false;
	}

	namespace
	{
		bool debugSectionsHidden = false;
	}

	HideDebugSectionsGuard::HideDebugSectionsGuard(bool hide) :
		previous(std::exchange(debugSectionsHidden, hide))
	{}

	HideDebugSectionsGuard::~HideDebugSectionsGuard()
	{
		debugSectionsHidden = previous;
	}

	bool ShowDebugSections()
	{
		return !debugSectionsHidden;
	}

	void StepFade(float& fade, bool visible, float deltaTime)
	{
		const float step = deltaTime / ThemeManager::Constants::UI_FADE_SECONDS;
		fade = std::clamp(fade + (visible ? step : -step), 0.0f, 1.0f);
	}

	FadeScope::FadeScope(float fade) :
		context(*ImGui::GetCurrentContext()),
		hoveredWindow(context.HoveredWindow),
		hoveredWindowUnderMoving(context.HoveredWindowUnderMovingWindow)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * fade);
		if (fade < 1.0f)
			context.HoveredWindow = context.HoveredWindowUnderMovingWindow = nullptr;
	}

	FadeScope::~FadeScope()
	{
		context.HoveredWindow = hoveredWindow;
		context.HoveredWindowUnderMovingWindow = hoveredWindowUnderMoving;
		ImGui::PopStyleVar();
	}

	void TextUnformattedDisabled(const char* a_text, const char* a_textEnd)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextUnformatted(a_text, a_textEnd);
		ImGui::PopStyleColor();
	}

	enum class SelectableShape
	{
		Theme,
		TableRow
	};

	static bool DrawSelectable(const char* label, bool selected, ImGuiSelectableFlags flags, const ImVec2& size, SelectableShape shape)
	{
		auto* window = ImGui::GetCurrentWindow();
		if (window->SkipItems)
			return false;
		auto* table = ImGui::GetCurrentTable();
		const bool fullTableRow = shape == SelectableShape::TableRow;
		const bool spanColumns = (flags & ImGuiSelectableFlags_SpanAllColumns) != 0;
		const bool tableBackground = table && (spanColumns || fullTableRow);
		const bool columnsBackground = !table && spanColumns && window->DC.CurrentColumns;
		const bool splitBackground = !tableBackground && !columnsBackground;
		ImDrawListSplitter splitter;
		if (splitBackground) {
			splitter.Split(window->DrawList, 2);
			splitter.SetCurrentChannel(window->DrawList, 1);
		}
		const SKSE::stl::scope_exit restoreChannels([&] {
			if (splitBackground)
				splitter.Merge(window->DrawList);
		});
		const bool disabledByFlag = (flags & ImGuiSelectableFlags_Disabled) && !(GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled);
		if (disabledByFlag)
			ImGui::BeginDisabled();
		const SKSE::stl::scope_exit restoreDisabled([&] {
			if (disabledByFlag)
				ImGui::EndDisabled();
		});
		bool pressed;
		{
			const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
			for (const auto color : { ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive, ImGuiCol_NavCursor })
				ImGui::PushStyleColor(color, transparent);
			const SKSE::stl::scope_exit restoreColors([] { ImGui::PopStyleColor(4); });
			const Anim::NoListHighlight customHighlight;
			pressed = ImGui::Selectable(label, selected, flags, size);
		}
		const bool hovered = ImGui::IsItemHovered() || (flags & ImGuiSelectableFlags_Highlight);
		const bool active = ImGui::IsItemActive();
		if (ImGui::IsItemToggledSelection())
			selected = !selected;
		const bool drawFill = hovered || active || selected;
		const bool drawCursor = GImGui->NavId == ImGui::GetItemID();
		if (ImGui::IsItemVisible() && (drawFill || drawCursor)) {
			const ImRect itemBounds(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
			if (tableBackground) {
				ImGui::TablePushBackgroundChannel();
			} else if (columnsBackground) {
				ImGui::PushColumnsBackground();
			} else {
				splitter.SetCurrentChannel(window->DrawList, 0);
			}
			const float rounding = fullTableRow ? 0.0f : ImGui::GetStyle().FrameRounding;
			if (drawFill) {
				const ImGuiCol color = active ? ImGuiCol_HeaderActive : hovered ? ImGuiCol_HeaderHovered :
				                                                                  ImGuiCol_Header;
				if (fullTableRow && table) {
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(color));
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, 0);
				} else {
					window->DrawList->AddRectFilled(itemBounds.Min, itemBounds.Max, ImGui::GetColorU32(color), rounding);
				}
			}
			if (drawCursor) {
				if (splitBackground)
					splitter.SetCurrentChannel(window->DrawList, 1);
				ImGuiNavRenderCursorFlags cursorFlags = ImGuiNavRenderCursorFlags_Compact;
				if (GImGui->LastItemData.ItemFlags & ImGuiItemFlags_IsMultiSelect)
					cursorFlags |= ImGuiNavRenderCursorFlags_AlwaysDraw;
				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
				const SKSE::stl::scope_exit restoreRounding([] { ImGui::PopStyleVar(); });
				ImGui::RenderNavCursor(itemBounds, ImGui::GetItemID(), cursorFlags);
			}
			if (tableBackground)
				ImGui::TablePopBackgroundChannel();
			else if (columnsBackground)
				ImGui::PopColumnsBackground();
		}

		return pressed;
	}

	bool RoundedSelectable(const char* label, bool selected, ImGuiSelectableFlags flags, const ImVec2& size)
	{
		return Anim::ReducedMotion() ? DrawSelectable(label, selected, flags, size, SelectableShape::Theme) :
		                               ImGui::Selectable(label, selected, flags, size);
	}

	template <class DrawEntry>
	static bool DrawRoundedMenuEntry(bool selected, bool enabled, bool isMenu, DrawEntry&& drawEntry)
	{
		auto* window = ImGui::GetCurrentWindow();
		if (window->SkipItems)
			return false;
		ImDrawListSplitter splitter;
		splitter.Split(window->DrawList, 2);
		splitter.SetCurrentChannel(window->DrawList, 1);
		const SKSE::stl::scope_exit restoreChannels([&] { splitter.Merge(window->DrawList); });
		enabled = enabled && !(GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled);
		bool result;
		{
			const ImVec4 transparent{};
			for (const auto color : { ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive, ImGuiCol_NavCursor })
				ImGui::PushStyleColor(color, transparent);
			const SKSE::stl::scope_exit restoreColors([] { ImGui::PopStyleColor(4); });
			const Anim::NoListHighlight ownHighlight;
			result = drawEntry();
		}
		const auto& item = GImGui->LastItemData;
		const bool hovered = enabled && (GImGui->HoveredId == item.ID ||
											(GImGui->NavId == item.ID && GImGui->NavCursorVisible && GImGui->NavHighlightItemUnderNav));
		const bool active = hovered && GImGui->ActiveId == item.ID;
		const bool highlighted = enabled && (isMenu ? result : selected && window->DC.LayoutType == ImGuiLayoutType_Horizontal);
		if (item.StatusFlags & ImGuiItemStatusFlags_Visible) {
			splitter.SetCurrentChannel(window->DrawList, 0);
			{
				auto* currentWindow = std::exchange(GImGui->CurrentWindow, window);
				const SKSE::stl::scope_exit restoreWindow([&] { GImGui->CurrentWindow = currentWindow; });
				const auto blend = Anim::ItemBlend(item.ID, hovered, active, highlighted);
				Anim::FillBlend(window->DrawList, item.Rect.Min, item.Rect.Max, ImGui::GetStyle().FrameRounding, blend);
			}
			if (GImGui->NavId == item.ID) {
				splitter.SetCurrentChannel(window->DrawList, 1);
				auto* currentWindow = std::exchange(GImGui->CurrentWindow, window);
				const SKSE::stl::scope_exit restoreWindow([&] { GImGui->CurrentWindow = currentWindow; });
				ImGui::RenderNavCursor(item.Rect, item.ID, ImGuiNavRenderCursorFlags_Compact);
			}
		}
		return result;
	}

	bool RoundedMenuItem(const char* label, const char* shortcut, bool selected, bool enabled)
	{
		return DrawRoundedMenuEntry(selected, enabled, false, [&] { return ImGui::MenuItem(label, shortcut, selected, enabled); });
	}

	bool BeginRoundedMenu(const char* label, bool enabled)
	{
		const auto& style = ImGui::GetStyle();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::max(style.WindowPadding.x, style.PopupRounding), std::max(style.WindowPadding.y, style.PopupRounding)));
		const SKSE::stl::scope_exit restorePadding([] { ImGui::PopStyleVar(); });
		return DrawRoundedMenuEntry(false, enabled, true, [&] { return ImGui::BeginMenu(label, enabled); });
	}

	bool TableRowSelectable(const char* label, bool selected, ImGuiSelectableFlags flags)
	{
		return DrawSelectable(label, selected, flags, ImVec2(0.0f, ImGui::GetFrameHeight()), SelectableShape::TableRow);
	}

	void SetTooltipPositionNearMouse(float estimatedHeight, float estimatedWidth)
	{
		const ImVec2 mousePos = ImGui::GetMousePos();
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		constexpr float kTooltipOffsetX = 16.0f;
		constexpr float kTooltipOffsetY = 12.0f;

		const float viewportLeft = viewport->WorkPos.x;
		const float viewportRight = viewport->WorkPos.x + viewport->WorkSize.x;
		const float viewportTop = viewport->WorkPos.y;
		const float viewportBottom = viewport->WorkPos.y + viewport->WorkSize.y;

		// Vertical: flip above cursor when it would overflow the bottom.
		const bool placeAboveCursor = (mousePos.y + kTooltipOffsetY + estimatedHeight) > viewportBottom;
		float posY;
		float pivotY;
		if (placeAboveCursor) {
			const float tentativeTopY = mousePos.y - kTooltipOffsetY - estimatedHeight;
			posY = (tentativeTopY < viewportTop) ? (viewportTop + estimatedHeight) : (mousePos.y - kTooltipOffsetY);
			pivotY = 1.0f;
		} else {
			posY = mousePos.y + kTooltipOffsetY;
			pivotY = 0.0f;
		}

		// Horizontal: clamp so the tooltip stays within viewport bounds.
		float posX = mousePos.x + kTooltipOffsetX;
		if (estimatedWidth > 0.0f) {
			const float maxX = viewportRight - estimatedWidth;
			posX = ImMax(viewportLeft, ImMin(posX, maxX));
		}

		ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always, ImVec2(0.0f, pivotY));
	}

	void AddTooltip(const char* a_desc, ImGuiHoveredFlags a_flags)
	{
		if (ImGui::IsItemHovered(a_flags)) {
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 8, 8 });
			const ImVec2 pad = ImGui::GetStyle().WindowPadding;
			const float wrapWidth = ImGui::GetFontSize() * 50.0f;
			const ImVec2 wrappedTextSize = ImGui::CalcTextSize(a_desc, nullptr, false, wrapWidth);
			const float estimatedTooltipHeight = wrappedTextSize.y + pad.y * 2.0f;
			const float estimatedTooltipWidth = wrappedTextSize.x + pad.x * 2.0f;
			SetTooltipPositionNearMouse(estimatedTooltipHeight, estimatedTooltipWidth);

			if (ImGui::BeginTooltip()) {
				ImGui::PushTextWrapPos(wrapWidth);
				ImGui::TextUnformatted(a_desc);
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
			ImGui::PopStyleVar();
		}
	}

	float HelpMarkerWidth()
	{
		return ImGui::GetTextLineHeight();
	}

	void HelpMarker(const char* a_desc)
	{
		ImGui::AlignTextToFramePadding();
		// Centred on the frame-aligned line at the footer help glyph's size
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 box(HelpMarkerWidth(), ImGui::GetFrameHeight());
		ImGui::Dummy(box);
		Icons::DrawCenteredGlyph(ImGui::GetWindowDrawList(), pos, box, ImGui::GetFontSize() * ThemeManager::Constants::HEADER_GLYPH_ICON_SCALE, Icons::TI(MenuIcons::kHelp),
			ImGui::GetColorU32(ImGuiCol_TextDisabled));
		if (a_desc)
			HoverTip(a_desc);
	}

	namespace
	{
		struct GlassSegmentPalette
		{
			ImVec4 track;
			ImVec4 selectedFill;
			ImVec4 selectedText;
			ImVec4 idleText;
			ImVec4 hoverFill;
			float rounding = 0.0f;
			float inset = 0.0f;
			float segmentHeight = 0.0f;
			float trackHeight = 0.0f;
		};

		GlassSegmentPalette MakeGlassSegmentPalette(bool muted = false)
		{
			const auto& style = ImGui::GetStyle();
			const float scale = GetUIScale();
			GlassSegmentPalette p;
			p.trackHeight = ImGui::GetFrameHeight();
			p.inset = std::max(ThemeManager::Constants::PILL_TAB_MIN_INSET, std::floor(style.FramePadding.y / scale * 0.55f)) * scale;
			p.segmentHeight = p.trackHeight - p.inset * 2.0f;
			p.rounding = std::max(style.FrameRounding, p.trackHeight * 0.5f);

			// Translucent mica-style track over the blurred panel.
			p.track = ImVec4(1.0f, 1.0f, 1.0f, C::GLASS_TRACK_IDLE_ALPHA);
			const ImVec4 frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
			if (frame.w > 0.01f) {
				p.track.x = frame.x * C::GLASS_TRACK_FRAME_MIX + (1.0f - C::GLASS_TRACK_FRAME_MIX);
				p.track.y = frame.y * C::GLASS_TRACK_FRAME_MIX + (1.0f - C::GLASS_TRACK_FRAME_MIX);
				p.track.z = frame.z * C::GLASS_TRACK_FRAME_MIX + (1.0f - C::GLASS_TRACK_FRAME_MIX);
				p.track.w = std::clamp(frame.w * C::GLASS_TRACK_ALPHA_SCALE + C::GLASS_TRACK_ALPHA_MIN, C::GLASS_TRACK_ALPHA_MIN, C::GLASS_TRACK_ALPHA_MAX);
			}

			p.selectedFill = ImGui::GetStyleColorVec4(ImGuiCol_Header);
			p.selectedFill.w = std::clamp(std::max(p.selectedFill.w, C::GLASS_SELECTED_ALPHA_FLOOR) * C::GLASS_SELECTED_ALPHA_SCALE, C::GLASS_SELECTED_ALPHA_MIN, C::GLASS_SELECTED_ALPHA_MAX);
			p.selectedText = style.Colors[ImGuiCol_Text];
			p.idleText = Colors::GetSecondary();
			p.hoverFill = p.selectedFill;
			p.hoverFill.w *= C::GLASS_HOVER_ALPHA_SCALE;

			if (muted) {
				p.selectedFill.w *= C::GLASS_MUTED_SELECTED_SCALE;
				p.selectedText = p.idleText;
				p.idleText.w *= C::GLASS_MUTED_TEXT_SCALE;
			}
			p.track.w = Surface::FillOpacity(p.track.w);
			p.selectedFill.w = Surface::FillOpacity(p.selectedFill.w);
			p.hoverFill.w = Surface::FillOpacity(p.hoverFill.w);
			return p;
		}

		std::string_view PillTabDisplayLabel(const char* label)
		{
			std::string_view view(label ? label : "");
			if (const auto hash = view.find("##"); hash != std::string_view::npos)
				view = view.substr(0, hash);
			return view;
		}

		struct PillTabItemState
		{
			ImGuiID id = 0;
			std::string label;
		};

		struct PillTabBarState
		{
			ImGuiID id = 0;
			ImGuiID selectedId = 0;
			ImGuiID nextSelectedId = 0;
			int lastFrameVisible = -1;
			bool itemOpen = false;
			bool itemPushedId = false;
			std::vector<PillTabItemState> itemsThisFrame;
			std::vector<PillTabItemState> itemsForStrip;
			std::vector<std::string> labelsForStrip;
		};

		std::unordered_map<ImGuiID, PillTabBarState> g_pillTabBars;
		std::vector<ImGuiID> g_pillTabStack;

		PillTabBarState* CurrentPillTabBar()
		{
			if (g_pillTabStack.empty())
				return nullptr;
			auto it = g_pillTabBars.find(g_pillTabStack.back());
			return it != g_pillTabBars.end() ? &it->second : nullptr;
		}

		/** @brief Draws the glass track + pills. Returns true if the selection changed. */
		bool DrawGlassPillStrip(const char* id, const std::vector<std::string>& labels, int& selected, int marked = -1, bool muted = false, bool centered = false)
		{
			if (labels.empty())
				return false;

			const auto palette = MakeGlassSegmentPalette(muted);
			const auto& style = ImGui::GetStyle();
			const float scale = GetUIScale();
			const float padX = std::max(ThemeManager::Constants::PILL_TAB_MIN_HORIZONTAL_PADDING * scale, style.FramePadding.x * 1.35f);

			float contentWidth = palette.inset * static_cast<float>(labels.size() + 1);
			for (const auto& label : labels)
				contentWidth += ImGui::CalcTextSize(label.c_str()).x + padX * 2.0f;

			const float avail = ImGui::GetContentRegionAvail().x;
			const bool needScroll = contentWidth > avail + 0.5f;
			const float navigationSize = std::min(palette.trackHeight, std::max(1.0f, avail));
			const float navigationGap = style.ItemInnerSpacing.x;
			const bool showArrows = needScroll && avail >= navigationSize * 4.0f + navigationGap * 3.0f;
			const float navigationWidth = needScroll ? (showArrows ? 3.0f : 1.0f) * (navigationSize + navigationGap) : 0.0f;
			const float viewportWidth = std::max(0.0f, avail - navigationWidth);
			const bool showStrip = !needScroll || viewportWidth > 0.0f;
			const float trackWidth = needScroll ? contentWidth : std::min(contentWidth, avail);
			ImGui::PushID(id);
			ImGui::BeginGroup();
			auto* selectionStorage = ImGui::GetStateStorage();
			const ImGuiID selectionKey = ImGui::GetID("##VisibleSelection");
			const ImGuiID viewportKey = ImGui::GetID("##StripViewportWidth");
			const ImGuiID contentKey = ImGui::GetID("##StripContentWidth");
			const int previousSelection = selectionStorage->GetInt(selectionKey, -1);
			const bool layoutChanged = selectionStorage->GetFloat(viewportKey, -1.0f) != viewportWidth ||
			                           selectionStorage->GetFloat(contentKey, -1.0f) != contentWidth;
			const int displayedSelection = selected;
			ImGuiWindow* scrollWindow = nullptr;
			bool changed = false;

			if (needScroll && showStrip) {
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowContentSize(ImVec2(contentWidth, palette.trackHeight));
				ImGui::BeginChild("##PillStripScroll", ImVec2(viewportWidth, palette.trackHeight),
					ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				scrollWindow = ImGui::GetCurrentWindow();
			}

			if (showStrip) {
				if (centered && !needScroll)
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - trackWidth) * 0.5f);
				const ImVec2 trackMin = ImGui::GetCursorScreenPos();
				const ImVec2 trackMax{ trackMin.x + trackWidth, trackMin.y + palette.trackHeight };
				auto* drawList = ImGui::GetWindowDrawList();

				drawList->AddRectFilled(trackMin, trackMax, ImGui::GetColorU32(palette.track), palette.rounding);
				drawList->AddRect(trackMin, trackMax, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)), palette.rounding, 0,
					ThemeManager::Constants::CONTROL_OUTLINE_WIDTH * scale);

				ImGui::BeginGroup();
				ImGui::SetCursorScreenPos({ trackMin.x + palette.inset, trackMin.y + palette.inset });
				ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { palette.inset, 0.0f });
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { padX, (palette.segmentHeight - ImGui::GetFontSize()) * 0.5f });
				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, std::max(0.0f, palette.rounding - palette.inset));

				// The selected pill is one shape that glides to the chosen tab; the buttons themselves stay clear.
				// Its rectangle follows from the label widths, since the buttons are laid out in a row.
				if (selected >= 0 && selected < static_cast<int>(labels.size())) {
					float selectedX = trackMin.x + palette.inset;
					for (int i = 0; i < selected; ++i)
						selectedX += ImGui::CalcTextSize(labels[static_cast<size_t>(i)].c_str()).x + padX * 2.0f + palette.inset;
					const float selectedWidth = ImGui::CalcTextSize(labels[static_cast<size_t>(selected)].c_str()).x + padX * 2.0f;
					const auto pill = Anim::SelectionGlide(ImGui::GetID("pillGlide"), ImVec2(selectedX, trackMin.y + palette.inset),
						ImVec2(selectedX + selectedWidth, trackMin.y + palette.inset + palette.segmentHeight));
					drawList->AddRectFilled(pill.min, pill.max, ImGui::GetColorU32(palette.selectedFill), std::max(0.0f, palette.rounding - palette.inset));
				}
				ImVec4 clearFill = palette.hoverFill;
				clearFill.w = 0.0f;

				for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
					if (i > 0)
						ImGui::SameLine();
					const bool isSelected = i == selected;
					ImGui::PushID(i);
					ImGui::PushStyleColor(ImGuiCol_Button, clearFill);
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, isSelected ? clearFill : palette.hoverFill);
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, clearFill);
					ImGui::PushStyleColor(ImGuiCol_Text, isSelected ? palette.selectedText : palette.idleText);
					if (Util::Button(labels[i].c_str())) {
						selected = i;
						changed = true;
					}
					if (needScroll && i == selected && (selected != previousSelection || changed || layoutChanged))
						ImGui::SetScrollHereX(0.5f);
					ImGui::PopStyleColor(4);

					if (i == marked && !isSelected) {
						const ImVec2 min = ImGui::GetItemRectMin();
						const ImVec2 max = ImGui::GetItemRectMax();
						const float radius = ImGui::GetFontSize() * 0.1f;
						drawList->AddCircleFilled({ max.x - padX * 0.35f, (min.y + max.y) * 0.5f }, radius,
							ImGui::GetColorU32(Colors::GetAccent()));
					}
					ImGui::PopID();
				}

				ImGui::SameLine(0.0f, 0.0f);
				ImGui::Dummy({ palette.inset, palette.segmentHeight });
				ImGui::Dummy({ 0.0f, palette.inset });
				ImGui::PopStyleVar(4);
				ImGui::EndGroup();
				selectionStorage->SetInt(selectionKey, displayedSelection);
				selectionStorage->SetFloat(viewportKey, viewportWidth);
				selectionStorage->SetFloat(contentKey, contentWidth);

				if (needScroll) {
					if (ImGui::IsWindowHovered() && std::abs(ImGui::GetIO().MouseWheel) > 0.0f)
						ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * ThemeManager::Constants::PILL_TAB_SCROLL_STEP * scale);
					ImGui::EndChild();
					ImGui::PopStyleVar();
				}
			}

			if (needScroll) {
				if (showStrip)
					ImGui::SameLine(0.0f, navigationGap);
				if (showArrows) {
					ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
					ImGui::BeginDisabled(scrollWindow->Scroll.x <= 0.0f);
					if (ImGui::ArrowButtonEx("##PreviousTabs", ImGuiDir_Left, ImVec2(navigationSize, palette.trackHeight)))
						ImGui::SetScrollX(scrollWindow, std::max(0.0f, scrollWindow->Scroll.x - viewportWidth));
					ImGui::EndDisabled();
					AddTooltip(T("ui.tabs.previous", "Previous tabs"), kTooltipWhenDisabled);
					ImGui::SameLine(0.0f, navigationGap);
					ImGui::BeginDisabled(scrollWindow->Scroll.x >= scrollWindow->ScrollMax.x);
					if (ImGui::ArrowButtonEx("##NextTabs", ImGuiDir_Right, ImVec2(navigationSize, palette.trackHeight)))
						ImGui::SetScrollX(scrollWindow, scrollWindow->Scroll.x + viewportWidth);
					ImGui::EndDisabled();
					AddTooltip(T("ui.tabs.next", "Next tabs"), kTooltipWhenDisabled);
					ImGui::PopItemFlag();
					ImGui::SameLine(0.0f, navigationGap);
				}
				if (ImGui::ArrowButtonEx("##TabListButton", ImGuiDir_Down, ImVec2(navigationSize, palette.trackHeight)))
					ImGui::OpenPopup("##TabList");
				AddTooltip(T("ui.tabs.list", "Show all tabs"));
				if (ImGui::BeginPopup("##TabList")) {
					for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
						ImGui::PushID(i);
						if (ImGui::Selectable(labels[i].c_str(), i == selected)) {
							selected = i;
							changed = true;
						}
						if (i == selected)
							ImGui::SetItemDefaultFocus();
						ImGui::PopID();
					}
					ImGui::EndPopup();
				}
			}
			ImGui::EndGroup();
			ImGui::PopID();

			return changed;
		}
	}  // namespace

	bool SegmentedControl(const char* a_id, const char* const* a_labels, int a_count, int& a_selected, int a_marked,
		bool a_muted)
	{
		if (!a_labels || a_count <= 0)
			return false;
		std::vector<std::string> labels;
		labels.reserve(static_cast<size_t>(a_count));
		for (int i = 0; i < a_count; ++i)
			labels.emplace_back(a_labels[i] ? a_labels[i] : "");
		return SettingsControl::InterceptInt(a_id, a_selected,
			[&](int& a_bound) { return DrawGlassPillStrip(a_id, labels, a_bound, a_marked, a_muted); });
	}

	bool IsInsidePillTabBar()
	{
		return !g_pillTabStack.empty();
	}

	bool BeginPillTabBar(const char* str_id, bool centered)
	{
		if (ImGui::GetCurrentWindow()->SkipItems)
			return false;
		const ImGuiID id = ImGui::GetID(str_id);
		auto& state = g_pillTabBars[id];
		IM_ASSERT(!state.itemOpen);
		state.id = id;
		state.lastFrameVisible = GImGui->FrameCount;
		state.itemsThisFrame.clear();
		g_pillTabStack.push_back(id);

		int selected = -1;
		for (std::size_t index = 0; index < state.itemsForStrip.size(); ++index)
			if (state.itemsForStrip[index].id == state.selectedId)
				selected = static_cast<int>(index);
		if (selected < 0 && !state.itemsForStrip.empty()) {
			selected = 0;
			state.selectedId = state.itemsForStrip.front().id;
		}
		{
			MenuFonts::FontRoleGuard bodyFont(Menu::FontRole::Body);
			if (DrawGlassPillStrip(str_id, state.labelsForStrip, selected, -1, false, centered))
				state.selectedId = state.itemsForStrip[selected].id;
		}
		ImGui::Spacing();
		ImGui::PushOverrideID(id);
		return true;
	}

	bool BeginPillTabItem(const char* label, bool* p_open, ImGuiTabItemFlags flags)
	{
		auto* state = CurrentPillTabBar();
		if (!state)
			return false;
		IM_ASSERT(!state->itemOpen);

		if (p_open && !*p_open)
			return false;

		const ImGuiID id = ImGui::GetID(label);
		state->itemsThisFrame.push_back({ id, label });

		if (flags & ImGuiTabItemFlags_SetSelected)
			state->nextSelectedId = id;

		if (state->selectedId == 0)
			state->selectedId = id;
		if (state->selectedId != id)
			return false;

		state->itemOpen = true;
		state->itemPushedId = !(flags & ImGuiTabItemFlags_NoPushId);
		if (state->itemPushedId)
			ImGui::PushOverrideID(id);
		Anim::BeginFadeIn(id, ThemeManager::Constants::ANIM_PAGE_ENTER_SECONDS, ThemeManager::Constants::ANIM_PAGE_FADE_FROM);
		return true;
	}

	void EndPillTabItem()
	{
		auto* state = CurrentPillTabBar();
		IM_ASSERT(state && state->itemOpen);
		if (!state || !state->itemOpen)
			return;
		Anim::EndFadeIn();
		if (state->itemPushedId)
			ImGui::PopID();
		state->itemOpen = false;
		state->itemPushedId = false;
	}

	void EndPillTabBar()
	{
		auto* state = CurrentPillTabBar();
		if (!state)
			return;
		IM_ASSERT(!state->itemOpen);

		state->itemsForStrip.swap(state->itemsThisFrame);
		state->labelsForStrip.clear();
		for (const auto& item : state->itemsForStrip)
			state->labelsForStrip.emplace_back(PillTabDisplayLabel(item.label.c_str()));
		const auto hasTab = [&](ImGuiID id) {
			return std::ranges::any_of(state->itemsForStrip, [&](const auto& item) { return item.id == id; });
		};
		if (state->nextSelectedId != 0 && hasTab(state->nextSelectedId))
			state->selectedId = state->nextSelectedId;
		else if (!hasTab(state->selectedId))
			state->selectedId = state->itemsForStrip.empty() ? 0 : state->itemsForStrip.front().id;
		state->nextSelectedId = 0;

		ImGui::PopID();
		g_pillTabStack.pop_back();
	}

	bool StatusBanner(const char* a_icon, const char* a_message, const ImVec4& a_color)
	{
		const float scale = GetUIScale();
		const ImVec2 padding{ 12.0f * scale, 8.0f * scale };
		const float gap = C::STATUS_BANNER_GAP * scale;
		const float width = ImGui::GetContentRegionAvail().x;
		const Icons::GlyphRef icon = Icons::TI(a_icon);
		const float iconWidth = Icons::CalcGlyphSize(icon).x;
		const float wrapWidth = std::max(1.0f, width - padding.x * 2.0f - iconWidth - gap);
		const ImVec2 textSize = ImGui::CalcTextSize(a_message, nullptr, false, wrapWidth);
		const float height = textSize.y + padding.y * 2.0f;

		const ImVec2 min = ImGui::GetCursorScreenPos();
		ImVec4 fill = a_color;
		fill.w *= ThemeManager::Constants::STATUS_BANNER_FILL_ALPHA;
		auto* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, { min.x + width, min.y + height }, ImGui::GetColorU32(fill), ImGui::GetStyle().FrameRounding);
		Icons::DrawCenteredGlyph(drawList, { min.x + padding.x, min.y + padding.y },
			{ iconWidth, textSize.y }, icon, ImGui::GetColorU32(a_color));

		ImGui::SetCursorScreenPos({ min.x + padding.x + iconWidth + gap, min.y + padding.y });
		ImGui::PushStyleColor(ImGuiCol_Text, a_color);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
		ImGui::TextUnformatted(a_message);
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();

		ImGui::SetCursorScreenPos(min);
		const bool clicked = ImGui::InvisibleButton("##StatusBanner", { width, height });
		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		return clicked;
	}

	float GetLockStatusBadgeSize()
	{
		const float scale = GetUIScale();
		const char* icon = ICON_TI_LOCK;
		const ImVec2 iconSize = Icons::CalcGlyphSize(Icons::TI(icon));
		constexpr float kPad = 3.0f;
		return std::max(iconSize.x, iconSize.y) + kPad * 2.0f * scale;
	}

	void DrawLockStatusBadge(ImVec2 a_min, bool a_locked, ImDrawList* a_drawList, bool a_hovered, bool a_held)
	{
		if (!a_drawList)
			a_drawList = ImGui::GetWindowDrawList();

		const float size = GetLockStatusBadgeSize();
		const char* icon = a_locked ? ICON_TI_LOCK : ICON_TI_LOCK_OPEN_2;
		const auto& statusPalette = Menu::GetSingleton()->GetTheme().StatusPalette;
		const ImVec4 statusColor = a_locked ? statusPalette.SuccessColor : statusPalette.Error;
		ImVec4 fill = statusColor;
		fill.w = ThemeManager::Constants::LOCK_BUTTON_FILL_ALPHA;
		ImVec4 border = statusColor;
		border.w = ThemeManager::Constants::LOCK_BUTTON_BORDER_ALPHA;

		const ImVec2 max(a_min.x + size, a_min.y + size);
		const float rounding = std::min(ImGui::GetStyle().FrameRounding, size * ThemeManager::Constants::LOCK_BUTTON_ROUNDING_RATIO);
		// Both callers draw this right after the badge's item, so the last item id keys the eased highlight, in the
		// badge's own state colour
		{
			auto tint = StateHoverButtonStyle(statusColor);
			DrawAnimatedButtonHighlight(ImRect(a_min, max), ImGui::GetItemID(), a_hovered, a_held, false, a_drawList);
		}
		a_drawList->AddRectFilled(a_min, max, ImGui::GetColorU32(fill), rounding);
		a_drawList->AddRect(a_min, max, ImGui::GetColorU32(border), rounding, 0, ThemeManager::Constants::CONTROL_OUTLINE_WIDTH * GetUIScale());
		Icons::DrawCenteredGlyph(a_drawList, a_min, ImVec2(size, size),
			Icons::TI(icon), ImGui::GetColorU32(statusColor));
	}

	bool LockStatusBadgeButton(const char* a_id, bool a_locked, const char* a_tooltip)
	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (!window)
			return false;

		const float size = GetLockStatusBadgeSize();
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const ImRect bb(min, ImVec2(min.x + size, min.y + size));
		const ImGuiID id = window->GetID(a_id);
		ImGui::ItemSize(bb.GetSize());
		if (!ImGui::ItemAdd(bb, id))
			return false;

		bool hovered = false;
		bool held = false;
		const bool clicked = ImGui::ButtonBehavior(bb, id, &hovered, &held);
		DrawLockStatusBadge(min, a_locked, nullptr, hovered, held);
		if (a_tooltip)
			AddTooltip(a_tooltip);
		return clicked;
	}

	namespace
	{
		constexpr float kBadgePadX = 5.0f;
		constexpr float kBadgePadY = 1.5f;
		constexpr float kBadgeRounding = 3.0f;
		constexpr float kBadgeFillAlpha = 0.22f;
		constexpr float kBadgeLabelTowardText = 0.35f;
	}

	float MeasureBadgeWidth(const char* a_label)
	{
		return ImGui::CalcTextSize(a_label).x + kBadgePadX * GetUIScale() * 2.0f;
	}

	float DrawBadgeAt(ImVec2 a_min, float a_rowHeight, const char* a_label, const ImVec4& a_bg, const ImVec4& a_fg,
		ImDrawList* a_drawList)
	{
		if (!a_drawList)
			a_drawList = ImGui::GetWindowDrawList();

		const float scale = GetUIScale();
		const ImVec2 textSize = ImGui::CalcTextSize(a_label);
		const float padX = kBadgePadX * scale;
		const float padY = kBadgePadY * scale;
		const float pillW = textSize.x + padX * 2.0f;
		const float pillH = std::min(a_rowHeight, textSize.y + padY * 2.0f);
		const float y = a_min.y + (a_rowHeight - pillH) * 0.5f;

		a_drawList->AddRectFilled(ImVec2(a_min.x, y), ImVec2(a_min.x + pillW, y + pillH),
			ImGui::GetColorU32(a_bg), kBadgeRounding * scale);
		a_drawList->AddText(ImVec2(a_min.x + padX, CenterTextTopY(y, pillH)), ImGui::GetColorU32(a_fg), a_label);
		return pillW;
	}

	void Badge(const char* a_label, const ImVec4& a_tint)
	{
		const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
		const ImVec4 bg(a_tint.x, a_tint.y, a_tint.z, kBadgeFillAlpha);
		const ImVec4 fg(a_tint.x + (text.x - a_tint.x) * kBadgeLabelTowardText,
			a_tint.y + (text.y - a_tint.y) * kBadgeLabelTowardText,
			a_tint.z + (text.z - a_tint.z) * kBadgeLabelTowardText, 1.0f);
		const float lineH = ImGui::GetTextLineHeight();
		const float width = DrawBadgeAt(ImGui::GetCursorScreenPos(), lineH, a_label, bg, fg);
		ImGui::Dummy(ImVec2(width, lineH));
	}

	float MeasureHdrSdrCapabilityPillWidth(bool a_supportsHDR)
	{
		return MeasureBadgeWidth(a_supportsHDR ? T("ui.badge.hdr", "HDR") : T("ui.badge.sdr", "SDR"));
	}

	void DrawHdrSdrCapabilityPillAt(ImVec2 a_min, float a_rowHeight, bool a_supportsHDR, ImDrawList* a_drawList)
	{
		const char* label = a_supportsHDR ?
		                        T("ui.badge.hdr", "HDR") :
		                        T("ui.badge.sdr", "SDR");

		const ImVec4 bg = a_supportsHDR ?
		                      ThemeManager::Constants::BADGE_HDR_BG :
		                      ThemeManager::Constants::BADGE_SDR_BG;
		const ImVec4 fg = a_supportsHDR ?
		                      ThemeManager::Constants::BADGE_HDR_FG :
		                      ThemeManager::Constants::BADGE_SDR_FG;

		DrawBadgeAt(a_min, a_rowHeight, label, bg, fg, a_drawList);
	}

	void DrawHdrSdrCapabilityPill(bool a_supportsHDR)
	{
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const float lineH = ImGui::GetTextLineHeight();
		DrawHdrSdrCapabilityPillAt(cursor, lineH, a_supportsHDR);
		ImGui::Dummy(ImVec2(MeasureHdrSdrCapabilityPillWidth(a_supportsHDR), lineH));
	}

	LockedSection::LockedSection(bool a_locked, const char* a_message, bool* a_outBannerClicked) :
		m_locked(a_locked)
	{
		if (!m_locked)
			return;
		const bool clicked = StatusBanner(ICON_TI_LOCK, a_message, Colors::GetWarning());
		if (a_outBannerClicked && clicked)
			*a_outBannerClicked = true;
		ImGui::Spacing();
		ImGui::BeginDisabled();
	}

	LockedSection::~LockedSection()
	{
		if (m_locked)
			ImGui::EndDisabled();
	}

	void Explainer(const char* a_label, const char* a_text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Colors::GetSecondary());
		if (ImGui::TreeNodeEx(a_label, ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
			ImGui::Indent();
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(a_text);
			ImGui::PopTextWrapPos();
			ImGui::Unindent();
		}
		ImGui::PopStyleColor();
	}

	void ToolbarDivider(bool a_continueLine)
	{
		const float thickness = ThemeManager::Constants::CONTROL_OUTLINE_WIDTH * GetUIScale();
		if (!a_continueLine) {
			ImGui::SameLine();
			ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical, thickness);
			return;
		}
		const float spacing = ImGui::GetStyle().ItemSpacing.x * 2.0f;
		ImGui::SameLine(0.0f, spacing);
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical, thickness);
		ImGui::SameLine(0.0f, spacing);
	}

	// Static state for clear shader cache confirmation popup
	static bool showClearCacheConfirmation = false;
	static bool dontAskAgainCheckbox = false;
	static ShaderCacheClearScope pendingClearScope = ShaderCacheClearScope::Full;

	ShaderCacheClearScope ResolveShaderCacheClearScope()
	{
		auto* menu = globals::menu;
		const bool smartDefault = menu && menu->GetSettings().SmartClearShaderCacheDefault;
		return ResolveShaderCacheClearScope(smartDefault, ImGui::GetIO().KeyShift);
	}

	const char* GetClearShaderCacheTooltip()
	{
		if (ResolveShaderCacheClearScope() == ShaderCacheClearScope::ActiveOnly) {
			return T("ui.clear_shader_cache_tooltip_smart",
				"Clears only the shaders drawing the current scene. They recompile in the "
				"background as the scene redraws; everything else stays cached.");
		}
		return T("menu.clear_shader_cache_tooltip",
			"Clears the shader cache and disk cache (if enabled). "
			"The Shader Cache is the collection of compiled shaders which replace the vanilla shaders at runtime. "
			"The Disk Cache is a collection of compiled shaders on disk. "
			"Clearing will mean that shaders are recompiled only when the game re-encounters them.");
	}

	// Helper function to perform the actual cache clearing
	static void PerformClearShaderCache(ShaderCacheClearScope a_scope)
	{
		auto* shaderCache = globals::shaderCache;
		if (!shaderCache)
			return;

		if (a_scope == ShaderCacheClearScope::ActiveOnly) {
			shaderCache->BeginActiveShaderCapture();
			return;
		}

		shaderCache->Clear();
		if (shaderCache->IsDiskCache()) {
			shaderCache->DeleteDiskCache();
		}
		shaderCache->ClearContentStore();
		shaderCache->SetRecachePending(true);
	}

	void DrawRecacheBanner()
	{
		auto* shaderCache = globals::shaderCache;
		if (!shaderCache || !shaderCache->IsRecachePending())
			return;
		if (!shaderCache->IsCompiling()) {
			shaderCache->SetRecachePending(false);
			return;
		}
		if (StatusBanner(ICON_TI_REFRESH,
				T("ui.recache_banner", "Shaders are loaded or compiled as needed after this change. Click to dismiss."),
				Colors::GetWarning()))
			shaderCache->SetRecachePending(false);
		ImGui::Spacing();
	}

	void RequestClearShaderCacheConfirmation(ShaderCacheClearScope a_scope)
	{
		auto* menu = globals::menu;
		if (!menu)
			return;

		if (globals::shaderCache &&
			(globals::shaderCache->IsCapturingActiveShaders() || globals::shaderCache->IsAwaitingMenuCloseCapture())) {
			return;  // ignore re-trigger while a capture is already in progress
		}

		// If user has opted to skip confirmation, clear immediately
		if (menu->GetSettings().SkipClearCacheConfirmation) {
			PerformClearShaderCache(a_scope);
			return;
		}

		// Show confirmation popup
		pendingClearScope = a_scope;
		showClearCacheConfirmation = true;
		dontAskAgainCheckbox = false;
	}

	void DrawClearShaderCacheConfirmation()
	{
		if (!showClearCacheConfirmation)
			return;

		const bool isSmart = pendingClearScope == ShaderCacheClearScope::ActiveOnly;
		const char* title = isSmart ? T("ui.smart_clear_shader_cache", "Clear Active Shaders?") : T("ui.clear_shader_cache", "Clear Shader Cache?");

		ImGui::OpenPopup(title);

		if (auto popup = CenteredPopupModal(title, &showClearCacheConfirmation)) {
			ImGui::Text("%s", T("ui.clear_cache_confirm", "Are you sure you want to clear the shader cache?"));
			ImGui::Spacing();
			ImGui::Spacing();
			if (isSmart) {
				ImGui::TextWrapped(
					"%s", T("ui.smart_clear_cache_desc",
							  "Watches the next second of rendering and clears only the shaders drawing this scene, "
							  "twice - once now, and once after this menu closes to catch what it was covering. "
							  "Everything else stays cached.\n\n"
							  "The cleared shaders recompile in the background as the scene redraws, so expect a "
							  "brief stutter and some surfaces reverting to their vanilla appearance until it "
							  "finishes. That is expected - this clears far less than a full wipe, it does not "
							  "make the recompile invisible.\n\n"
							  "Keep the broken area visible while it captures. If the problem persists, use a full clear."));
			} else {
				ImGui::TextWrapped(
					"%s", T("ui.clear_cache_desc",
							  "This will clear all compiled shaders from memory and disk cache (if enabled). "
							  "Shaders will be recompiled when the game next encounters them."));
				if (globals::shaderCache) {
					// Measured once when the popup opens, since it walks the store directory.
					static std::optional<SIE::ShaderCache::ContentStoreUsage> storeUsage;
					if (ImGui::IsWindowAppearing() || !storeUsage)
						storeUsage = globals::shaderCache->GetContentStoreUsage();
					if (storeUsage->blobs > 0) {
						ImGui::Spacing();
						ImGui::TextWrapped("%s", I18n::GetSingleton()->Format("ui.clear_cache_store_note",
																		 { { "count", std::to_string(storeUsage->blobs) },
																			 { "size", std::format("{:.0f}", static_cast<double>(storeUsage->bytes) / (1024.0 * 1024.0)) } },
																		 "This also deletes the persistent shader store ({count} shaders, {size} MB).")
													 .c_str());
					}
				}
			}
			ImGui::Spacing();
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			ImGui::Checkbox(T("ui.dont_ask_again", "Don't ask me again"), &dontAskAgainCheckbox);

			ImGui::Spacing();

			// Center buttons
			const float buttonWidth = ThemeManager::Constants::POPUP_BUTTON_WIDTH * GetUIScale();
			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float totalWidth = buttonWidth * 2 + spacing;
			const float windowWidth = ImGui::GetWindowWidth();
			const float offset = (windowWidth - totalWidth) * 0.5f;
			if (offset > 0)
				ImGui::SetCursorPosX(offset);

			if (Util::Button(T("ui.clear_cache", "Clear Cache"), ImVec2(buttonWidth, 0))) {
				// Save preference if checkbox is checked
				if (dontAskAgainCheckbox) {
					if (auto* menu = globals::menu) {
						menu->GetSettings().SkipClearCacheConfirmation = true;
					}
				}

				PerformClearShaderCache(pendingClearScope);
				showClearCacheConfirmation = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if (Util::Button(T("ui.cancel", "Cancel"), ImVec2(buttonWidth, 0))) {
				showClearCacheConfirmation = false;
				ImGui::CloseCurrentPopup();
			}
		}
	}

	// Shared by the header icon and the text button, drawn once per frame from Menu::DrawSettings
	static ConfirmationPopup restoreSavedSettingsPopup;

	void RequestRestoreSavedSettingsConfirmation()
	{
		auto* state = globals::state;
		if (!state)
			return;

		// Nothing to lose: restore straight away
		if (!state->HasUnsavedChanges()) {
			state->Load();
			globals::menu->ClearUndoHistory();
			return;
		}

		restoreSavedSettingsPopup.Request();
	}

	void DrawRestoreSavedSettingsConfirmation()
	{
		if (!restoreSavedSettingsPopup.IsOpen())
			return;

		// Refreshed every frame so a language switch while open updates the text; the ### id keeps the popup open
		restoreSavedSettingsPopup.title = std::format("{}###RestoreSavedSettingsConfirmation", T("ui.restore_saved_settings_title", "Restore Saved Settings?"));
		restoreSavedSettingsPopup.message = T("ui.restore_saved_settings_confirm",
			"You have unsaved changes. Restoring the saved settings will discard them. This cannot be undone.");
		restoreSavedSettingsPopup.confirmLabel = T("ui.restore", "Restore");
		restoreSavedSettingsPopup.cancelLabel = T("ui.cancel", "Cancel");
		restoreSavedSettingsPopup.focus = ConfirmationPopup::Focus::Cancel;

		if (restoreSavedSettingsPopup.Draw()) {
			if (auto* state = globals::state) {
				state->Load();
				globals::menu->ClearUndoHistory();
			}
		}
	}

	// --- Reusable ConfirmationPopup ---

	void ConfirmationPopup::Request()
	{
		assert(rememberedChoice == Choice::Confirm || rememberedChoice == Choice::Alternate);
		if (dontAskAgainPersist && *dontAskAgainPersist) {
			pendingChoice = rememberedChoice;
			return;
		}
		show = true;
		pendingChoice = Choice::None;
		dontAskCheckbox = false;
	}

	bool ConfirmationPopup::Draw()
	{
		return DrawChoice() == Choice::Confirm;
	}

	ConfirmationPopup::Choice ConfirmationPopup::DrawChoice()
	{
		if (pendingChoice != Choice::None)
			return std::exchange(pendingChoice, Choice::None);
		if (!show)
			return Choice::None;

		ImGui::OpenPopup(title.c_str());

		Choice result = Choice::None;
		if (auto popup = CenteredPopupModal(title.c_str(), &show)) {
			ImGui::TextWrapped("%s", message.c_str());
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			if (showDontAskAgain)
				ImGui::Checkbox(dontAskAgainLabel.empty() ? T("ui.dont_ask_again", "Don't ask me again") : dontAskAgainLabel.c_str(), &dontAskCheckbox);

			const auto buttonWidthForLabel = [](const std::string& label) {
				return ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			};
			const float buttonWidth = std::max({
				ThemeManager::Constants::POPUP_BUTTON_WIDTH * GetUIScale(),
				buttonWidthForLabel(confirmLabel),
				buttonWidthForLabel(cancelLabel),
				buttonWidthForLabel(alternateLabel),
			});
			const ImVec2 buttonSize(buttonWidth, 0.0f);
			const int buttonCount = 1 + !confirmLabel.empty() + !alternateLabel.empty();
			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float totalWidth = buttonSize.x * buttonCount + spacing * (buttonCount - 1);
			const float offset = (ImGui::GetWindowWidth() - totalWidth) * 0.5f;
			if (offset > 0)
				ImGui::SetCursorPosX(offset);

			const auto pick = [&](Choice choice) {
				if (choice == rememberedChoice && showDontAskAgain && dontAskCheckbox && dontAskAgainPersist)
					*dontAskAgainPersist = true;
				result = choice;
				show = false;
				ImGui::CloseCurrentPopup();
			};

			if (!confirmLabel.empty()) {
				if (primaryConfirm ? SuccessButton(confirmLabel.c_str(), buttonSize) : Util::Button(confirmLabel.c_str(), buttonSize))
					pick(Choice::Confirm);
				if (focus == Focus::Confirm)
					ImGui::SetItemDefaultFocus();
				ImGui::SameLine();
			}

			if (!alternateLabel.empty()) {
				if (ErrorButton(alternateLabel.c_str(), buttonSize))
					pick(Choice::Alternate);
				ImGui::SameLine();
			}

			if (Util::Button(cancelLabel.empty() ? T("ui.cancel", "Cancel") : cancelLabel.c_str(), buttonSize))
				pick(Choice::Cancel);
			if (focus == Focus::Cancel)
				ImGui::SetItemDefaultFocus();
		}
		// Closed from its title bar.
		if (result == Choice::None && !show)
			result = Choice::Cancel;
		return result;
	}

	constexpr float kPercentageScale = 1e2f;

	bool PercentageSlider(const char* label, float* data, float lb, float ub, const char* format, ImGuiSliderFlags flags)
	{
		// The slider binds a temporary, so name the member it stands for or scene authoring misses it.
		SettingsControl::ProxyScope sceneProxy(data, kPercentageScale);

		float percentageData = (*data) * kPercentageScale;
		bool retval = ImGui::SliderFloat(label, &percentageData, lb, ub, format, flags);
		// Only on an edit: the scaled round trip is not exact in float, so an idle write-back drifts the member.
		if (retval && !SettingsControl::IsReplicating())
			(*data) = percentageData / kPercentageScale;
		return retval;
	}

	bool InvertedCheckbox(const char* label, bool* storedValue)
	{
		if (!storedValue)
			return false;

		SettingsControl::ProxyScope sceneProxy(storedValue, 1.0f);
		bool displayValue = !*storedValue;
		const bool changed = ImGui::Checkbox(label, &displayValue);
		if (changed && !SettingsControl::IsReplicating())
			*storedValue = !displayValue;
		return changed;
	}

	bool RadioButton(const char* label, unsigned int* storedValue, unsigned int buttonValue)
	{
		if (!storedValue)
			return false;

		SettingsControl::ProxyScope sceneProxy(storedValue, 1.0f);
		int displayValue = static_cast<int>(*storedValue);
		const bool changed = ImGui::RadioButton(label, &displayValue, static_cast<int>(buttonValue));
		if (changed && !SettingsControl::IsReplicating())
			*storedValue = static_cast<unsigned int>(displayValue);
		return changed;
	}

	const void* GetActiveControlStorageAddress()
	{
		return g_activeControlStorageAddress;
	}

	ImVec2 GetNativeViewportSizeScaled(float scale)
	{
		const auto Size = ImGui::GetMainViewport()->Size;
		return { Size.x * scale, Size.y * scale };
	}

	bool InitializeMenuIcons(Menu* menu)
	{
		return IconLoader::InitializeMenuIcons(menu);
	}

	bool DrawClippedTextExpansion(float contentLeft, float contentRight, const std::function<void(ImDrawList*)>& drawContents)
	{
		auto* window = ImGui::GetCurrentWindow();
		if (!ImGui::IsItemVisible() || contentRight <= window->ClipRect.Max.x)
			return false;
		const auto& style = ImGui::GetStyle();
		const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
		ImGui::PushID(ImGui::GetItemID());
		const SKSE::stl::scope_exit restoreID([]() noexcept { ImGui::PopID(); });
		auto* storage = ImGui::GetStateStorage();
		const ImGuiID progressId = ImGui::GetID("##ExpansionProgress");
		const ImGuiID frameId = ImGui::GetID("##ExpansionFrame");
		const int frame = ImGui::GetFrameCount();
		float progress = storage->GetInt(frameId, -1) == frame - 1 ? storage->GetFloat(progressId) : 0.0f;
		const float step = ImGui::GetIO().DeltaTime / ThemeManager::Constants::SIDEBAR_ROW_FADE_DURATION;
		progress = std::clamp(progress + (hovered ? step : -step), 0.0f, 1.0f);
		storage->SetFloat(progressId, progress);
		storage->SetInt(frameId, frame);
		const float expansionAlpha = Util::Smoothstep(0.0f, 1.0f, progress);
		if (expansionAlpha <= 0.0f)
			return false;
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * expansionAlpha);
		const SKSE::stl::scope_exit restoreAlpha([]() noexcept { ImGui::PopStyleVar(); });
		auto* drawList = ImGui::GetForegroundDrawList(window->Viewport);
		const ImRect expanded({ contentLeft, ImGui::GetItemRectMin().y }, { contentRight + style.FramePadding.x, ImGui::GetItemRectMax().y });
		ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
		background.w = 1.0f;
		drawList->PushClipRect({ window->Viewport->Pos.x, window->ClipRect.Min.y },
			{ window->Viewport->Pos.x + window->Viewport->Size.x, window->ClipRect.Max.y });
		drawList->AddRectFilled(expanded.Min, expanded.Max, ImGui::GetColorU32(background));
		drawList->AddRectFilled(expanded.Min, expanded.Max, ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered));
		drawList->AddRect(expanded.Min, expanded.Max, ImGui::GetColorU32(ImGuiCol_Border));
		drawContents(drawList);
		drawList->PopClipRect();
		return true;
	}

	bool CheckboxWithClippedText(const char* label, bool* value)
	{
		const bool changed = ImGui::Checkbox(label, value);
		const auto& style = ImGui::GetStyle();
		const auto start = ImGui::GetItemRectMin();
		const ImVec2 textPos(start.x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x, start.y + style.FramePadding.y);
		const auto* textEnd = ImGui::FindRenderedTextEnd(label);
		const float contentRight = textPos.x + ImGui::CalcTextSize(label, textEnd).x;
		const auto* viewport = ImGui::GetWindowViewport();
		if (contentRight + style.FramePadding.x > viewport->Pos.x + viewport->Size.x) {
			if (auto tooltip = HoverTooltipWrapper())
				ImGui::TextUnformatted(label, textEnd);
		} else {
			DrawClippedTextExpansion(textPos.x, contentRight, [&](ImDrawList* drawList) {
				drawList->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), label, textEnd);
			});
		}
		return changed;
	}

	// Text rendering helpers
	ImVec2 DrawSharpText(const char* text, bool alignToPixelGrid, float scale)
	{
		ImVec2 startPos = ImGui::GetCursorPos();

		if (alignToPixelGrid) {
			// Get current position
			ImVec2 pos = ImGui::GetCursorPos();

			// Align to pixel grid for sharper rendering
			pos.x = std::round(pos.x);
			pos.y = std::round(pos.y);

			// Set aligned position
			ImGui::SetCursorPos(pos);
		}
		// Apply scale if needed
		if (scale != 1.0f) {
			ImGui::SetWindowFontScale(scale);
		}

		// Use Text instead of TextUnformatted for better rendering
		ImGui::Text("%s", text);
		// Restore original scale if needed
		if (scale != 1.0f)
			ImGui::SetWindowFontScale(1.0f);

		// Calculate and return the rendered size
		ImVec2 endPos = ImGui::GetCursorPos();
		return ImVec2(endPos.x - startPos.x, endPos.y - startPos.y);
	}

	ImVec2 DrawAlignedTextWithLogo(ID3D11ShaderResourceView* logoTexture, const ImVec2& logoSize, const char* text, float textScale, ImU32 logoTint)
	{
		// Save current cursor position
		ImVec2 startPos = ImGui::GetCursorPos();

		const auto& style = ImGui::GetStyle();
		const float availableWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
		const bool inlineLogo = availableWidth > logoSize.x + style.ItemSpacing.x;
		const float textOffsetX = inlineLogo ? logoSize.x + style.ItemSpacing.x : 0.0f;
		ImGui::SetWindowFontScale(textScale);
		const ImVec2 textSize = ImGui::CalcTextSize(text, nullptr, false, std::max(1.0f, availableWidth - textOffsetX));
		ImGui::SetWindowFontScale(1.0f);
		const float blockWidth = inlineLogo ? textOffsetX + textSize.x : std::max(logoSize.x, textSize.x);
		const float blockHeight = inlineLogo ? std::max(logoSize.y, textSize.y) : logoSize.y + style.ItemSpacing.y + textSize.y;

		// Calculate vertical offset to center align logo with text
		const float verticalOffset = inlineLogo ? (blockHeight - logoSize.y) * 0.5f : 0.0f;

		// Position cursor for logo with vertical alignment
		ImGui::BeginGroup();
		ImGui::SetCursorPos(ImVec2(startPos.x, startPos.y + verticalOffset));

		// Render logo using draw list with tint color support
		ImVec2 logoPos = ImGui::GetCursorScreenPos();
		const ImVec2 logoMin = IconLoader::SnapToPixel(logoPos);
		const ImVec2 snappedSize = IconLoader::SnapToPixel(logoSize);
		const ImVec2 logoMax = ImVec2(logoMin.x + snappedSize.x, logoMin.y + snappedSize.y);
		ImGui::GetWindowDrawList()->AddImage(IconLoader::ViewForSize(logoTexture, snappedSize), logoMin, logoMax, ImVec2(0, 0), ImVec2(1, 1), logoTint);

		// Reset cursor for text with proper vertical alignment
		const float textOffsetY = inlineLogo ? (blockHeight - textSize.y) * 0.5f : logoSize.y + style.ItemSpacing.y;
		ImGui::SetCursorPos(ImVec2(startPos.x + textOffsetX, startPos.y + textOffsetY));
		// Use windowed font scale for sharper text
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
		ImGui::SetWindowFontScale(textScale);

		// Render text aligned to pixel grid for sharpness
		ImGui::Text("%s", text);
		// Restore style
		ImGui::SetWindowFontScale(1.0f);
		ImGui::PopStyleVar();
		ImGui::SetCursorPos(startPos);
		ImGui::Dummy(ImVec2(blockWidth, blockHeight));
		ImGui::EndGroup();

		// Calculate and return the total rendered size
		return ImGui::GetItemRectSize();
	}

	// StyledButtonWrapper implementation
	StyledButtonWrapper::StyledButtonWrapper(const ImVec4& normalColor, const ImVec4& hoveredColor, const ImVec4& activeColor) :
		m_pushedStyles(0)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, normalColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoveredColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeColor);
		m_pushedStyles = 3;
	}

	StyledButtonWrapper::~StyledButtonWrapper()
	{
		if (m_pushedStyles > 0) {
			ImGui::PopStyleColor(m_pushedStyles);
		}
	}

	namespace ButtonHelpers
	{
		constexpr ImVec4 kUnlockButtonColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
		constexpr ImVec4 kUnlockButtonHoverColor = ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
		constexpr ImVec4 kLockButtonColor = ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
		constexpr ImVec4 kLockButtonHoverColor = ImVec4(0.9f, 0.3f, 0.3f, 1.0f);

		ImVec4 AdjustButtonColor(const ImVec4& color, float amount)
		{
			const float maxChannel = std::max({ color.x, color.y, color.z });
			const float minChannel = ThemeManager::Constants::BUTTON_MIN_COLOR_CHANNEL;
			const float maxColorChannel = ThemeManager::Constants::BUTTON_MAX_COLOR_CHANNEL;
			const float adjustment = maxChannel <= (maxColorChannel - amount) ? amount : -amount;
			return ImVec4(
				std::clamp(color.x + adjustment, minChannel, maxColorChannel),
				std::clamp(color.y + adjustment, minChannel, maxColorChannel),
				std::clamp(color.z + adjustment, minChannel, maxColorChannel),
				color.w);
		}

		ImVec4 WithAlpha(const ImVec4& color, float alpha)
		{
			return ImVec4(color.x, color.y, color.z, alpha);
		}
	}

	StyledButtonWrapper StatusButtonStyle(const ImVec4& color)
	{
		auto hover = ButtonHelpers::AdjustButtonColor(color, ThemeManager::Constants::BUTTON_HOVER_BRIGHTEN);
		auto active = ButtonHelpers::AdjustButtonColor(color, ThemeManager::Constants::BUTTON_ACTIVE_BRIGHTEN);
		return StyledButtonWrapper(color, hover, active);
	}

	StyledButtonWrapper DestructiveButtonStyle()
	{
		return StatusButtonStyle(Menu::GetSingleton()->GetTheme().StatusPalette.Error);
	}

	bool ErrorButton(const char* label, const ImVec2& size)
	{
		auto _style = DestructiveButtonStyle();
		return Util::Button(label, size);
	}

	bool ErrorButtonWithFlash(const char* label, const ImVec2& size, int flashDurationMs)
	{
		auto _style = DestructiveButtonStyle();
		return ButtonWithFlash(label, size, flashDurationMs);
	}

	bool ErrorButtonWithFlash(const char* id, const char* tablerIcon, const char* label, int flashDurationMs)
	{
		auto _style = DestructiveButtonStyle();
		return ButtonWithFlash(id, tablerIcon, label, flashDurationMs);
	}

	StyledButtonWrapper StatusTextButtonStyle(const ImVec4& color)
	{
		return StyledButtonWrapper(color,
			ButtonHelpers::WithAlpha(color, ThemeManager::Constants::BUTTON_STATUS_TEXT_HOVER_ALPHA),
			ButtonHelpers::WithAlpha(color, ThemeManager::Constants::BUTTON_STATUS_TEXT_ACTIVE_ALPHA));
	}

	StyledButtonWrapper StateHoverButtonStyle(const ImVec4& color, bool apply)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		if (!apply)
			return StyledButtonWrapper(style.Colors[ImGuiCol_Button], style.Colors[ImGuiCol_ButtonHovered], style.Colors[ImGuiCol_ButtonActive]);
		return StyledButtonWrapper(style.Colors[ImGuiCol_Button], Anim::ScaleAlpha(color, C::BUTTON_STATE_HOVER_ALPHA), Anim::ScaleAlpha(color, C::BUTTON_STATE_PRESS_ALPHA));
	}

	StyledButtonWrapper SuccessButtonStyle()
	{
		return StatusTextButtonStyle(Menu::GetSingleton()->GetTheme().StatusPalette.SuccessColor);
	}

	StyledButtonWrapper WarningButtonStyle()
	{
		return StatusTextButtonStyle(Menu::GetSingleton()->GetTheme().StatusPalette.Warning);
	}

	bool SuccessButton(const char* label, const ImVec2& size)
	{
		auto _style = SuccessButtonStyle();
		return Util::Button(label, size);
	}

	bool WarningButton(const char* label, const ImVec2& size)
	{
		auto _style = WarningButtonStyle();
		return Util::Button(label, size);
	}

	StyledButtonWrapper UnlockButtonStyle()
	{
		return StyledButtonWrapper(ButtonHelpers::kUnlockButtonColor, ButtonHelpers::kUnlockButtonHoverColor, ButtonHelpers::kUnlockButtonHoverColor);
	}

	bool UnlockButton(const char* label, const ImVec2& size)
	{
		auto style = UnlockButtonStyle();
		return Util::Button(label, size);
	}

	StyledButtonWrapper LockButtonStyle()
	{
		return StyledButtonWrapper(ButtonHelpers::kLockButtonColor, ButtonHelpers::kLockButtonHoverColor, ButtonHelpers::kLockButtonHoverColor);
	}

	bool LockButton(const char* label, const ImVec2& size)
	{
		auto style = LockButtonStyle();
		return Util::Button(label, size);
	}

	bool ErrorTextButton(const char* label, const ImVec2& size)
	{
		auto _style = StatusTextButtonStyle(Menu::GetSingleton()->GetTheme().StatusPalette.Error);
		return Util::Button(label, size);
	}

	StyledButtonWrapper TransparentIconButtonStyle()
	{
		// The theme's button hover and press fills, as the header's nav buttons use
		const ImGuiStyle& style = ImGui::GetStyle();
		return StyledButtonWrapper(ImVec4(0, 0, 0, 0), style.Colors[ImGuiCol_ButtonHovered], style.Colors[ImGuiCol_ButtonActive]);
	}

	ImVec4 GetIconTint()
	{
		const auto& theme = Menu::GetSingleton()->GetTheme();
		return theme.UseMonochromeIcons ? theme.Palette.Text : ImVec4(1, 1, 1, 1);
	}

	static float GetPillRounding(const ImVec2& min, const ImVec2& max)
	{
		IM_ASSERT(max.x >= min.x && max.y >= min.y);
		return ImMin(max.x - min.x, max.y - min.y) * 0.5f;
	}

	static float GetThemedButtonHighlightRounding(const ImVec2& min, const ImVec2& max)
	{
		const float frameRounding = ImGui::GetStyle().FrameRounding;
		IM_ASSERT(frameRounding >= 0.0f);
		return ImMin(ImMax(frameRounding, 0.0f), GetPillRounding(min, max));
	}

	bool DrawRoundedButtonHighlight(const ImVec2& min, const ImVec2& max, bool hovered, bool active, ImDrawList* drawList)
	{
		return DrawRoundedButtonHighlight(min, max, hovered, active, GetThemedButtonHighlightRounding(min, max), drawList);
	}

	bool DrawRoundedButtonHighlight(const ImRect& rect, bool hovered, bool active, ImDrawList* drawList)
	{
		return DrawRoundedButtonHighlight(rect.Min, rect.Max, hovered, active, drawList);
	}

	bool DrawRoundedButtonHighlight(const ImVec2& min, const ImVec2& max, bool hovered, bool active, float rounding, ImDrawList* drawList)
	{
		if (!hovered && !active)
			return false;

		IM_ASSERT(max.x >= min.x && max.y >= min.y);
		IM_ASSERT(rounding >= 0.0f);
		if (!drawList)
			drawList = ImGui::GetWindowDrawList();

		drawList->AddRectFilled(min, max, ImGui::GetColorU32(active ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), rounding);
		return true;
	}

	UnderlayScope::UnderlayScope() :
		list(ImGui::GetWindowDrawList())
	{
		split = list->_Splitter._Count <= 1 && !ImGui::GetCurrentTable();
		if (split) {
			list->ChannelsSplit(2);
			list->ChannelsSetCurrent(1);
		}
	}

	UnderlayScope::~UnderlayScope()
	{
		if (split)
			list->ChannelsMerge();
	}

	ImDrawList* UnderlayScope::Background()
	{
		if (split)
			list->ChannelsSetCurrent(0);
		return list;
	}

	namespace
	{
		Anim::Blend PendingBlend(ImGuiID id, bool selected)
		{
			const ImGuiContext& g = *ImGui::GetCurrentContext();
			const bool enabled = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) == 0;
			return Anim::ItemBlend(id, enabled && g.HoveredIdPreviousFrame == id, enabled && g.ActiveId == id, selected);
		}
	}

	bool Button(const char* label, const ImVec2& size)
	{
		bool clicked;
		if (Anim::ReducedMotion()) {
			clicked = ImGui::Button(label, size);
		} else {
			const auto blend = PendingBlend(ImGui::GetID(label), false);
			const ImVec4 rest = ImGui::GetStyleColorVec4(ImGuiCol_Button);
			const ImVec4 hovered = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
			const ImVec4 active = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
			// Every state shows the eased colour, so the instant hover / press swap of ImGui is replaced by the blend
			const ImVec4 eased = Anim::Lerp(Anim::Lerp(rest, hovered, blend.hover), active, blend.press);
			ImGui::PushStyleColor(ImGuiCol_Button, eased);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, eased);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, eased);
			clicked = ImGui::Button(label, size);
			ImGui::PopStyleColor(3);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		return clicked;
	}

	bool Selectable(const char* label, bool selected, ImGuiSelectableFlags flags, const ImVec2& size)
	{
		// ImGui::Selectable is hooked (UIHooks), so every row already gets the gliding list highlight
		return ImGui::Selectable(label, selected, flags, size);
	}

	bool Selectable(const char* label, bool* selected, ImGuiSelectableFlags flags, const ImVec2& size)
	{
		return ImGui::Selectable(label, selected, flags, size);
	}

	void AddressButton(const char* a_name, uintptr_t a_address)
	{
		if (Util::Button(a_name))
			ImGui::SetClipboardText(std::format("{0:x}", a_address).c_str());
		if (ImGui::IsItemHovered()) {
			const auto tooltip = I18n::GetSingleton()->Format("ui.copy_address_tooltip", { { "name", a_name } }, "Copy {name} Address to Clipboard");
			ImGui::SetTooltip("%s", tooltip.c_str());
		}
	}

	ImU32 QuietIconColor(float hover, float selected)
	{
		const ImVec4 resting = Anim::Lerp(Colors::GetSecondary(), ImGui::GetStyleColorVec4(ImGuiCol_Text), hover);
		return ImGui::GetColorU32(Anim::Lerp(resting, Colors::GetAccent(), selected));
	}

	void SectionCard::Begin(const char* id)
	{
		const float pad = ThemeManager::Constants::HOME_CARD_PADDING * GetUIScale();
		// Without a drawn surface the vertical padding only adds dead space above and below the content
		const bool surfaceShown = Surface::Get(Surface::Role::SectionCard).enabled;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, surfaceShown ? pad : 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32_BLACK_TRANS);
		ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
			ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();

		// Surface backing, drawn first so the content sits on it
		Surface::Options surface;
		surface.rounding = ImGui::GetStyle().ChildRounding;
		surface.clip = Surface::ParentClipRect();
		const ImVec2 min = ImGui::GetWindowPos();
		const ImVec2 size = ImGui::GetWindowSize();
		Surface::Draw(ImGui::GetWindowDrawList(), min, ImVec2(min.x + size.x, min.y + size.y), Surface::Role::SectionCard, surface);
	}

	SectionCard::SectionCard(const char* id, ID3D11ShaderResourceView* image, const char* title)
	{
		Begin(id);
		MenuFonts::FontRoleGuard heading(Menu::FontRole::Heading);
		if (image) {
			// Category icons are white art tinted by the accent, matching the glyph icons of other cards
			const float size = ImGui::GetTextLineHeight();
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(size, size));
			ImGui::GetWindowDrawList()->AddImage(IconLoader::ViewForSize(image, ImVec2(size, size)), pos, ImVec2(pos.x + size, pos.y + size),
				ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(Colors::GetAccent()));
			ImGui::SameLine();
		}
		ImGui::TextUnformatted(title);
		ImGui::Spacing();
	}

	SectionCard::SectionCard(const char* id, const char* icon, const char* title)
	{
		Begin(id);
		MenuFonts::FontRoleGuard heading(Menu::FontRole::Heading);
		if (icon) {
			ImGui::PushStyleColor(ImGuiCol_Text, Colors::GetAccent());
			Icons::Text(Icons::TI(icon));
			ImGui::PopStyleColor();
			ImGui::SameLine();
		}
		ImGui::TextUnformatted(title);
		ImGui::Spacing();
	}

	SectionCard::~SectionCard()
	{
		ImGui::EndChild();
		ImGui::Dummy(ImVec2(0.0f, ThemeManager::Constants::HOME_CARD_SPACING * GetUIScale()));
	}

	SettingColumns::SettingColumns(const char* id, int columns, float minColumnFontSizes)
	{
		const float needed = ImGui::GetFontSize() * minColumnFontSizes * static_cast<float>(columns);
		if (columns > 1 && ImGui::GetContentRegionAvail().x >= needed)
			table = ImGui::BeginTable(id, columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX);
		if (table)
			ImGui::TableNextColumn();
	}

	SettingColumns::~SettingColumns()
	{
		if (table)
			ImGui::EndTable();
	}

	void SettingColumns::Next()
	{
		if (table)
			ImGui::TableNextColumn();
		else
			ImGui::Spacing();
	}

	void FitNextItemWithLabel(const char* label)
	{
		const float labelWidth = ImGui::CalcTextSize(label, nullptr, true).x;
		const float width = ImGui::GetContentRegionAvail().x - labelWidth - ImGui::GetStyle().ItemInnerSpacing.x;
		ImGui::SetNextItemWidth(std::max(width, ImGui::GetFontSize() * 6.0f));
	}

	void SettingHint(const char* text, bool alignToCheckboxLabel)
	{
		const float indent = alignToCheckboxLabel ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f;
		if (indent > 0.0f)
			ImGui::Indent(indent);
		{
			MenuFonts::FontRoleGuard subtext(Menu::FontRole::Subtext);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", text);
			ImGui::PopStyleColor();
		}
		if (indent > 0.0f)
			ImGui::Unindent(indent);
	}

	namespace
	{
		/** @brief Inner padding of an OptionCard, the same on every side: the Home card padding scaled to the UI. */
		float OptionCardPadding()
		{
			return ThemeManager::Constants::HOME_CARD_PADDING * GetUIScale();
		}

		/** @brief Line heights of an OptionCard's rows, measured in their font roles. */
		struct OptionCardMetrics
		{
			float titleHeight = 0.0f;
			float statusHeight = 0.0f;
			float descriptionHeight = 0.0f;
			float actionHeight = 0.0f;
			float gap = 0.0f;
		};

		OptionCardMetrics MeasureOptionCard(const OptionCardContent& content, float innerWidth)
		{
			OptionCardMetrics m;
			m.gap = ImGui::GetStyle().ItemSpacing.y;
			{
				MenuFonts::FontRoleGuard heading(Menu::FontRole::Heading);
				m.titleHeight = ImGui::GetTextLineHeight() * ThemeManager::Constants::OPTION_CARD_TITLE_SCALE;
			}
			if (content.status)
				m.statusHeight = ImGui::GetTextLineHeight();
			if (content.description) {
				MenuFonts::FontRoleGuard subtext(Menu::FontRole::Subtext);
				m.descriptionHeight = ImGui::CalcTextSize(content.description, nullptr, false, std::max(1.0f, innerWidth)).y;
			}
			if (content.action)
				m.actionHeight = ImGui::GetFrameHeight();
			return m;
		}
	}

	float OptionCardHeight(const OptionCardContent& content, float width)
	{
		const float padding = OptionCardPadding();
		const auto m = MeasureOptionCard(content, width - padding * 2.0f);
		float height = padding * 2.0f + m.titleHeight;
		if (m.statusHeight > 0.0f)
			height += m.gap + m.statusHeight;
		if (m.descriptionHeight > 0.0f)
			height += m.gap + m.descriptionHeight;
		if (m.actionHeight > 0.0f)
			height += m.gap * 2.0f + m.actionHeight;
		return height;
	}

	OptionCardClick OptionCard(const char* id, const OptionCardContent& content, bool selected, const ImVec2& size)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float width = size.x > 0.0f ? size.x : ImGui::GetContentRegionAvail().x;
		const float height = size.y > 0.0f ? size.y : OptionCardHeight(content, width);
		const float padding = OptionCardPadding();
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const ImVec2 max(min.x + width, min.y + height);
		const ImVec4 inner(min.x + padding, min.y + padding, max.x - padding, max.y - padding);
		const auto m = MeasureOptionCard(content, inner.z - inner.x);
		const float contentAlpha = content.unavailable ? style.DisabledAlpha : 1.0f;
		const auto faded = [contentAlpha](ImVec4 color) {
			color.w *= contentAlpha;
			return ImGui::GetColorU32(color);
		};

		// Content goes on the upper channel; the surface is drawn underneath once the card's hover is known
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImDrawListSplitter splitter;
		splitter.Split(drawList, 2);
		splitter.SetCurrentChannel(drawList, 1);
		ImGui::PushID(id);

		// The action button comes before the card button so it wins the hover where they overlap
		OptionCardClick result = OptionCardClick::None;
		if (content.action) {
			ImGui::SetCursorScreenPos(ImVec2(inner.x, inner.w - m.actionHeight));
			if (Button(content.action))
				result = OptionCardClick::Action;
			if (content.actionTooltip)
				HoverTip(content.actionTooltip);
		}

		drawList->PushClipRect(ImVec2(inner.x, inner.y), ImVec2(inner.z, inner.w), true);
		float y = inner.y;
		{
			MenuFonts::FontRoleGuard heading(Menu::FontRole::Heading);
			const float titleSize = ImGui::GetFontSize() * ThemeManager::Constants::OPTION_CARD_TITLE_SCALE;
			float x = inner.x;
			if (content.icon) {
				Icons::DrawCenteredGlyph(drawList, ImVec2(x, y), ImVec2(m.titleHeight, m.titleHeight), titleSize,
					Icons::TI(content.icon), faded(style.Colors[ImGuiCol_Text]));
				x += m.titleHeight + style.ItemInnerSpacing.x;
			}
			drawList->AddText(ImGui::GetFont(), titleSize, ImVec2(x, y), faded(style.Colors[ImGuiCol_Text]), content.title);
			y += m.titleHeight;
		}
		if (content.status) {
			y += m.gap;
			drawList->AddText(ImVec2(inner.x, y), faded(content.statusColor), content.status);
			y += m.statusHeight;
		}
		if (content.description) {
			y += m.gap;
			MenuFonts::FontRoleGuard subtext(Menu::FontRole::Subtext);
			drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(inner.x, y), faded(style.Colors[ImGuiCol_TextDisabled]),
				content.description, nullptr, inner.z - inner.x);
		}
		drawList->PopClipRect();

		// Submitted last, so the layout continues below the card with no cursor fix-up
		ImGui::SetCursorScreenPos(min);
		if (ImGui::InvisibleButton("##Card", ImVec2(width, height)) && result == OptionCardClick::None)
			result = OptionCardClick::Card;
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();

		// Selected takes the accent border; hover previews it at half strength
		splitter.SetCurrentChannel(drawList, 0);
		Surface::Options options;
		const ImVec2 clipMin = drawList->GetClipRectMin();
		const ImVec2 clipMax = drawList->GetClipRectMax();
		options.clip = ImVec4(clipMin.x, clipMin.y, clipMax.x, clipMax.y);
		ImVec4 accent = Colors::GetAccent();
		if (!selected)
			accent.w *= 0.5f;
		if (selected || hovered)
			options.borderColor = ImGui::GetColorU32(accent);
		Surface::Draw(drawList, min, max, Surface::Role::OptionCard, options);
		splitter.Merge(drawList);
		return result;
	}

	bool DrawAnimatedButtonHighlight(const ImRect& rect, ImGuiID id, bool hovered, bool held, bool current, ImDrawList* drawList)
	{
		if (hovered)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		const auto blend = Anim::ItemBlend(id, hovered || current, held, false);
		if (blend.hover <= 0.0f && blend.press <= 0.0f)
			return false;
		if (!drawList)
			drawList = ImGui::GetWindowDrawList();
		const float rounding = GetThemedButtonHighlightRounding(rect.Min, rect.Max);
		Anim::FillLayer(drawList, rect.Min, rect.Max, rounding, ImGuiCol_ButtonHovered, blend.hover);
		Anim::FillLayer(drawList, rect.Min, rect.Max, rounding, ImGuiCol_ButtonActive, blend.press);
		return true;
	}

	void DrawDisclosureChevron(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, float progress)
	{
		IM_ASSERT(drawList != nullptr);
		IM_ASSERT(max.x >= min.x && max.y >= min.y);

		constexpr float radiusRatio = 0.30f;
		constexpr float horizontalRatio = 0.866f;
		constexpr float verticalRatio = 0.75f;
		const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		const float radius = std::min(max.x - min.x, max.y - min.y) * radiusRatio;
		const float angle = std::numbers::pi_v<float> * std::clamp(progress, 0.0f, 1.0f);
		const float cosine = std::cos(angle);
		const float sine = std::sin(angle);

		std::array<ImVec2, 3> points = {
			ImVec2(0.0f, verticalRatio * radius),
			ImVec2(-horizontalRatio * radius, -verticalRatio * radius),
			ImVec2(horizontalRatio * radius, -verticalRatio * radius)
		};
		for (auto& point : points) {
			const ImVec2 rotated(
				point.x * cosine - point.y * sine,
				point.x * sine + point.y * cosine);
			point = ImVec2(center.x + rotated.x, center.y + rotated.y);
		}

		drawList->AddTriangleFilled(points[0], points[1], points[2], ImGui::GetColorU32(ImGuiCol_Text));
	}

	void DrawStarIcon(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ImU32 color)
	{
		constexpr int pointCount = 5;
		constexpr int vertexCount = pointCount * 2;
		constexpr float innerRadiusRatio = 0.381966f;
		const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		const float outerRadius = std::min(max.x - min.x, max.y - min.y) * 0.5f;
		std::array<ImVec2, vertexCount> points;
		for (int i = 0; i < vertexCount; ++i) {
			const float angle = std::numbers::pi_v<float> * (static_cast<float>(i) / pointCount - 0.5f);
			const float radius = outerRadius * (i % 2 == 0 ? 1.0f : innerRadiusRatio);
			points[i] = ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
		}
		drawList->AddConcavePolyFilled(points.data(), vertexCount, color);
	}

	bool FlyoutMenuItem(const char* label, std::optional<bool> selected, bool enabled, float checkmarkLeftOffset, IconDrawCallback selectedIcon)
	{
		IM_ASSERT(checkmarkLeftOffset >= 0.0f);

		auto* window = ImGui::GetCurrentWindow();
		if (window->SkipItems)
			return false;

		auto* drawList = ImGui::GetWindowDrawList();
		IM_ASSERT(drawList->_Splitter._Count == 1);
		drawList->ChannelsSplit(2);
		const SKSE::stl::scope_exit mergeChannels([drawList]() noexcept { drawList->ChannelsMerge(); });
		drawList->ChannelsSetCurrent(1);

		bool pressed;
		{
			constexpr ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
			ImGui::PushStyleColor(ImGuiCol_Header, transparent);
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
			ImGui::PushStyleColor(ImGuiCol_NavCursor, transparent);
			const SKSE::stl::scope_exit restoreColors([]() noexcept { ImGui::PopStyleColor(4); });

			if (!selected.has_value()) {
				constexpr ImGuiSelectableFlags flags = ImGuiSelectableFlags_SelectOnRelease | ImGuiSelectableFlags_NoHoldingActiveID |
				                                       ImGuiSelectableFlags_SetNavIdOnHover | ImGuiSelectableFlags_SpanAvailWidth;
				pressed = ImGui::Selectable(label, false, flags | (enabled ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled));
			} else {
				auto& menuColumns = window->DC.MenuColumns;
				const ImU16 originalMarkOffset = menuColumns.OffsetMark;
				ImU16 baseMarkOffset = originalMarkOffset;
				if (baseMarkOffset == 0) {
					const float fallbackMarkOffset = std::trunc(ImGui::CalcTextSize(label, nullptr, true).x) + menuColumns.Spacing;
					baseMarkOffset = static_cast<ImU16>(std::clamp(
						fallbackMarkOffset, 0.0f, static_cast<float>(std::numeric_limits<ImU16>::max())));
				}
				const auto markShift = static_cast<ImU16>(std::clamp(
					std::lround(checkmarkLeftOffset), 0L, static_cast<long>(baseMarkOffset)));
				menuColumns.OffsetMark = static_cast<ImU16>(baseMarkOffset - (*selected ? markShift : 0));
				const SKSE::stl::scope_exit restoreMarkOffset(
					[&menuColumns, originalMarkOffset]() noexcept { menuColumns.OffsetMark = originalMarkOffset; });

				const ImVec2 textPos(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
				const float availableWidth = ImGui::GetContentRegionAvail().x;
				pressed = ImGui::MenuItem(label, nullptr, *selected && !selectedIcon, enabled);
				if (*selected && selectedIcon) {
					constexpr float checkmarkOffsetXRatio = 0.40f;
					constexpr float checkmarkOffsetYRatio = 0.134f * 0.5f;
					constexpr float checkmarkSizeRatio = 0.866f;
					const float fontSize = ImGui::GetFontSize();
					const float iconSize = fontSize * checkmarkSizeRatio;
					const float minimumWidth = static_cast<float>(std::max(menuColumns.TotalWidth, menuColumns.NextTotalWidth));
					const float stretchWidth = std::max(0.0f, availableWidth - minimumWidth);
					const ImVec2 iconMin(textPos.x + menuColumns.OffsetMark + stretchWidth + fontSize * checkmarkOffsetXRatio,
						textPos.y + fontSize * checkmarkOffsetYRatio);
					selectedIcon(drawList, iconMin, ImVec2(iconMin.x + iconSize, iconMin.y + iconSize),
						ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled));
				}
			}
		}

		const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		const bool hovered = ImGui::IsItemHovered();
		const bool active = ImGui::IsItemActive();
		const bool focused = ImGui::IsItemFocused();

		drawList->ChannelsSetCurrent(0);
		if (hovered || active || focused) {
			drawList->AddRectFilled(
				itemRect.Min,
				itemRect.Max,
				ImGui::GetColorU32(active ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered),
				GetThemedButtonHighlightRounding(itemRect.Min, itemRect.Max));
		}

		return pressed;
	}

	void DrawIconCircle(ImVec2 center, float radius, ImU32 color, bool filled)
	{
		auto* drawList = ImGui::GetWindowDrawList();
		if (filled)
			drawList->AddCircleFilled(center, radius, color, ThemeManager::Constants::ICON_CIRCLE_SEGMENTS);
		else
			drawList->AddCircle(center, radius, color, ThemeManager::Constants::ICON_CIRCLE_SEGMENTS,
				ThemeManager::Constants::ICON_OUTLINE_THICKNESS * GetUIScale());
	}

	void DrawInlineIndicatorDot(ImU32 color, bool filled)
	{
		const float lineHeight = ImGui::GetTextLineHeight();
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(lineHeight, lineHeight));
		const float halfLine = lineHeight * 0.5f;
		DrawIconCircle(ImVec2(origin.x + halfLine, origin.y + halfLine),
			lineHeight * ThemeManager::Constants::SCENE_INDICATOR_RADIUS_RATIO, color, filled);
	}

	// Shared constants for title-bar button overlays
	static constexpr float kTitleBarButtonPadding = 2.0f;
	static constexpr float kCloseCrossDiagonalScale = 0.5f / std::numbers::sqrt2_v<float>;
	static constexpr float kCloseCrossInset = 1.0f;
	static constexpr ImVec4 kTransparentButtonChrome(0, 0, 0, 0);

	static ImRect TitleBarButtonRect(const ImVec2& origin, float fontSize)
	{
		const float full = fontSize + kTitleBarButtonPadding * 2.0f;
		return ImRect(origin, ImVec2(origin.x + full, origin.y + full));
	}

	static ImVec2 RightTitleBarButtonOrigin(ImGuiWindow* window, float fontSize, float offset = 0.0f)
	{
		const auto& style = ImGui::GetStyle();
		return ImVec2(window->Rect().Max.x - window->WindowBorderSize - style.FramePadding.x - fontSize - offset - kTitleBarButtonPadding,
			window->Rect().Min.y + style.FramePadding.y - kTitleBarButtonPadding);
	}

	static ImVec2 CollapseTitleBarButtonOrigin(ImGuiWindow* window, bool hasCloseButton, float fontSize)
	{
		const auto& style = ImGui::GetStyle();
		IM_ASSERT(style.WindowMenuButtonPosition == ImGuiDir_Left || style.WindowMenuButtonPosition == ImGuiDir_Right);

		if (style.WindowMenuButtonPosition == ImGuiDir_Right)
			return RightTitleBarButtonOrigin(window, fontSize, hasCloseButton ? fontSize : 0.0f);

		return ImVec2(window->Pos.x + window->WindowBorderSize + style.FramePadding.x - kTitleBarButtonPadding,
			window->Pos.y + style.FramePadding.y - kTitleBarButtonPadding);
	}

	static bool IsTitleBarButtonHovered(ImGuiWindow* window, const ImRect& bb)
	{
		ImGuiContext& g = *ImGui::GetCurrentContext();
		return g.HoveredWindow == window && ImGui::IsMouseHoveringRect(bb.Min, bb.Max, false);
	}

	class NativeTitleBarButtonHighlightGuard
	{
	public:
		NativeTitleBarButtonHighlightGuard()
		{
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTransparentButtonChrome);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, kTransparentButtonChrome);
		}

		~NativeTitleBarButtonHighlightGuard() { ImGui::PopStyleColor(2); }
	};

	// Draws the rounded hover/press fill of a title-bar button at `pos`; `drawGlyph` redraws the glyph when the fill was drawn.
	template <typename GlyphFn>
	static void DrawRoundedTitleBarButton(ImGuiWindow* window, const ImVec2& pos, float sz, GlyphFn drawGlyph)
	{
		const ImRect bb = TitleBarButtonRect(pos, sz);
		const bool hovered = IsTitleBarButtonHovered(window, bb);
		const bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);

		window->DrawList->PushClipRect(window->Rect().Min, window->Rect().Max);
		if (DrawRoundedButtonHighlight(bb, hovered, held, window->DrawList))
			drawGlyph(bb);
		window->DrawList->PopClipRect();
	}

	// Draws a rounded close button overlay, matching native ImGui CloseButton position.
	static void DrawRoundedCloseHighlight(ImGuiWindow* window)
	{
		if (window->Flags & ImGuiWindowFlags_NoTitleBar)
			return;

		const float sz = ImGui::GetFontSize();
		DrawRoundedTitleBarButton(window, RightTitleBarButtonOrigin(window, sz), sz, [&](const ImRect& bb) {
			// Cross lines match ImGui's internal RenderCloseButton geometry.
			const ImVec2 c = bb.GetCenter();
			const float d = sz * kCloseCrossDiagonalScale - kCloseCrossInset;
			const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
			window->DrawList->AddLine({ c.x - d, c.y - d }, { c.x + d, c.y + d }, col);
			window->DrawList->AddLine({ c.x + d, c.y - d }, { c.x - d, c.y + d }, col);
		});
	}

	// Draws a rounded highlight for the collapse/triangle button in the title bar.
	static void DrawRoundedCollapseHighlight(ImGuiWindow* window, bool hasCloseButton)
	{
		if (window->Flags & (ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse))
			return;
		if (ImGui::GetStyle().WindowMenuButtonPosition == ImGuiDir_None)
			return;

		const float sz = ImGui::GetFontSize();
		const ImVec2 pos = CollapseTitleBarButtonOrigin(window, hasCloseButton, sz);
		DrawRoundedTitleBarButton(window, pos, sz, [&](const ImRect&) {
			const ImVec2 arrowPos(pos.x + kTitleBarButtonPadding, pos.y + kTitleBarButtonPadding);
			const ImGuiDir dir = window->Collapsed ? ImGuiDir_Right : ImGuiDir_Down;
			ImGui::RenderArrow(window->DrawList, arrowPos, ImGui::GetColorU32(ImGuiCol_Text), dir, 1.0f);
		});
	}

	static void DrawRoundedTitleBarButtonHighlights(ImGuiWindow* window, bool hasCloseButton, bool hasCollapseButton)
	{
		if (!window)
			return;

		if (hasCollapseButton)
			DrawRoundedCollapseHighlight(window, hasCloseButton);
		if (hasCloseButton)
			DrawRoundedCloseHighlight(window);
	}

	bool BeginWithRoundedClose(const char* name, bool* p_open, ImGuiWindowFlags flags)
	{
		DialogSizeAnimation animation;
		PrepareDialogSizeAnimation(ImGui::FindWindowByName(name), flags, animation);
		bool visible = false;
		{
			NativeTitleBarButtonHighlightGuard guard;
			visible = ImGui::Begin(name, p_open, flags);
		}
		DrawRoundedTitleBarButtonHighlights(ImGui::GetCurrentWindowRead(), p_open != nullptr, true);
		return visible;
	}

	bool BeginPopupModalWithRoundedClose(const char* name, bool* p_open, ImGuiWindowFlags flags)
	{
		DialogSizeAnimation animation;
		PrepareDialogSizeAnimation(GetOpenDialogWindow(name), flags, animation);
		bool visible = false;
		{
			NativeTitleBarButtonHighlightGuard guard;
			visible = ImGui::BeginPopupModal(name, p_open, flags);
		}
		if (visible)
			DrawRoundedTitleBarButtonHighlights(ImGui::GetCurrentWindowRead(), p_open != nullptr, false);
		return visible;
	}

	// Whether each custom-header window was docked last frame, keyed by its full "Label###id"
	// title. NoTitleBar has to be decided before Begin(), but IsWindowDocked() only reports the
	// true state after it - so, like the main window's own header, this frame draws whatever last
	// frame was and re-checks are one frame behind a dock/undock transition.
	static std::unordered_map<std::string, bool> s_customHeaderWasDocked;

	// Draws the close button's crossed lines, matching DrawRoundedCloseHighlight's native geometry.
	// Unlike the native button, nothing else renders the X, so it has to be drawn unconditionally
	// rather than only while highlighted.
	static void DrawCustomHeaderCloseCross(const ImVec2& min, const ImVec2& max)
	{
		const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		const float sz = max.x - min.x;
		const float scale = GetUIScale();
		const float d = sz * kCloseCrossDiagonalScale - kCloseCrossInset * scale;
		const float thickness = ThemeManager::Constants::EDITOR_CHROME_STROKE_WIDTH * scale;
		const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddLine({ c.x - d, c.y - d }, { c.x + d, c.y + d }, col, thickness);
		drawList->AddLine({ c.x + d, c.y - d }, { c.x - d, c.y + d }, col, thickness);
	}

	float GetEditorChromeHeaderHeight()
	{
		// FontSize + FramePadding.y on each side of the glyph, then the same pad again as outer air
		// so title glyphs / icons are not flush with the window's top edge.
		return ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * C::EDITOR_CHROME_PAD_FACTOR;
	}

	float GetEditorChromeTextCursorOffsetY(float rowHeight)
	{
		return rowHeight * 0.5f - GetTextInkCenterY();
	}

	float GetTextInkCenterY()
	{
		// Centre capital-letter ink, measured from 'H' since cap height and ascent vary per font.
		auto* baked = ImGui::GetFontBaked();
		const ImFontGlyph* glyph = baked->FindGlyph('H');
		const float inkCenter = glyph ? (glyph->Y0 + glyph->Y1) * 0.5f * ImGui::GetFontSize() / baked->Size : ImGui::GetFontSize() * 0.5f;
		return inkCenter;
	}

	float CenterTextTopY(float rowTop, float rowHeight)
	{
		return rowTop + rowHeight * 0.5f - GetTextInkCenterY();
	}

	// Draws the custom floating header: the title (also the drag handle), an optional
	// caller-supplied control cluster, and a close button pinned to the right edge. Runs
	// unconditionally (not gated on Begin()'s return value), same as a native title bar always
	// drawing regardless of what the body does - collapsing is disabled for these windows (see
	// BeginWithCustomHeader), so there is no risk of a hidden body stranding the header.
	static void DrawCustomHeaderRow(ImGuiWindow* window, const char* name, bool* p_open,
		const std::function<void()>& drawExtras,
		const std::function<void(ImVec2 iconMin, float iconSize)>& drawLeading)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float rowHeight = GetEditorChromeHeaderHeight();
		const float avail = ImGui::GetContentRegionAvail().x;
		const ImVec2 contentStart = ImGui::GetCursorScreenPos();
		const ImVec2 rowStart(contentStart.x, contentStart.y - style.WindowPadding.y);
		// AlwaysAutoResize sizes from CursorMaxPos. Drag/close chrome laid out to last-frame
		// `avail` would otherwise lock that width on scale-down (Palette grew but never shrank).
		const bool autoResize = (window->Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0;
		const ImVec2 cursorMaxBeforeHeader = window->DC.CursorMaxPos;
		if (window->Flags & ImGuiWindowFlags_NoBackground) {
			window->DrawList->PushClipRect(window->Rect().Min, window->Rect().Max, false);
			window->DrawList->AddRectFilled(window->Pos, ImVec2(window->Pos.x + window->Size.x, rowStart.y + rowHeight),
				ImGui::GetColorU32(ImGuiCol_WindowBg), window->WindowRounding, ImDrawFlags_RoundCornersTop);
			window->DrawList->PopClipRect();
		}

		ImGui::SetCursorScreenPos(rowStart);
		// AllowOverlap so title/extras/close drawn afterward still receive hover/click.
		ImGui::InvisibleButton("##CustomHeaderDrag", ImVec2(avail, rowHeight), ImGuiButtonFlags_AllowOverlap);
		// Drag only while actively dragging this item: a click on the close button must not start a move.
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			ImGui::StartMouseMovingWindow(window);

		float titleX = rowStart.x;
		if (drawLeading) {
			const float iconSize = ImGui::GetFontSize();
			const float iconY = rowStart.y + (rowHeight - iconSize) * 0.5f;
			drawLeading(ImVec2(rowStart.x, iconY), iconSize);
			titleX += iconSize + style.ItemInnerSpacing.x + style.FramePadding.x;
		}

		// Title: centre capital-letter ink in the row (see GetEditorChromeTextCursorOffsetY).
		ImGui::SetCursorScreenPos(ImVec2(titleX, rowStart.y + GetEditorChromeTextCursorOffsetY(rowHeight)));
		std::string_view displayTitle(name);
		if (const auto hash = displayTitle.find("##"); hash != std::string_view::npos)
			displayTitle = displayTitle.substr(0, hash);
		ImGui::TextUnformatted(displayTitle.data(), displayTitle.data() + displayTitle.size());

		if (drawExtras) {
			if (displayTitle.empty())
				ImGui::SetCursorScreenPos(ImVec2(titleX, rowStart.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f));
			else
				ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			drawExtras();
		}

		if (p_open) {
			// Square hit target centred in the row so top/bottom air matches the title glyphs.
			const float closeSize = ImGui::GetFontSize() + style.FramePadding.y * 2.0f;
			ImGui::SetCursorScreenPos(ImVec2(rowStart.x + avail - closeSize,
				rowStart.y + (rowHeight - closeSize) * 0.5f));
			auto _style = TransparentIconButtonStyle();
			const bool clicked = Util::Button("##CustomHeaderClose", ImVec2(closeSize, closeSize));
			DrawCustomHeaderCloseCross(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
			if (clicked)
				*p_open = false;
		}

		// Separator is the bottom edge of the bar; no ItemInnerSpacing above it (that was
		// doubling the bottom air vs the top). Zero ItemSpacing so Separator itself doesn't
		// insert another half-gap; restore WindowPadding below for the body, matching native
		// title-bar â†’ content layout.
		ImGui::SetCursorScreenPos(ImVec2(contentStart.x, rowStart.y + rowHeight));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
		ImGui::Separator();
		ImGui::PopStyleVar();
		ImGui::SetCursorScreenPos(ImVec2(contentStart.x, ImGui::GetCursorScreenPos().y + style.WindowPadding.y));

		if (autoResize) {
			// Keep the visual full-width header, but let the body alone decide auto-fit width.
			window->DC.CursorMaxPos.x = cursorMaxBeforeHeader.x;
			window->DC.IdealMaxPos.x = std::min(window->DC.IdealMaxPos.x, cursorMaxBeforeHeader.x);
		}
	}

	bool BeginWithCustomHeader(const char* name, bool* p_open,
		const std::function<void()>& drawExtras, ImGuiWindowFlags flags,
		const std::function<void(ImVec2 iconMin, float iconSize)>& drawLeading)
	{
		bool& wasDocked = s_customHeaderWasDocked[name];
		// Collapsing relies on the native title bar staying interactive while the body is skipped,
		// which the floating custom header can't reproduce safely - a window collapsed through it
		// would have nothing left on screen able to expand it again. Disabled unconditionally,
		// same as the main Community Shaders window's own custom header.
		ImGuiWindowFlags windowFlags = flags | ImGuiWindowFlags_NoCollapse;
		if (!wasDocked)
			windowFlags |= ImGuiWindowFlags_NoTitleBar;

		bool visible = false;
		{
			NativeTitleBarButtonHighlightGuard guard;
			visible = ImGui::Begin(name, p_open, windowFlags);
		}

		ImGuiWindow* window = ImGui::GetCurrentWindowRead();
		const bool isDocked = ImGui::IsWindowDocked();
		wasDocked = isDocked;

		if (isDocked) {
			// A shared dock tab bar already supplies the title and close x; only the rounded
			// highlight polish applies here, same as every other BeginWithRoundedClose window.
			DrawRoundedTitleBarButtonHighlights(window, p_open != nullptr, false);
		} else {
			// Drawn unconditionally (not gated on `visible`) so the drag handle and close button
			// are always reachable, matching a native title bar's always-on behavior.
			DrawCustomHeaderRow(window, name, p_open, drawExtras, drawLeading);
		}
		return visible;
	}

	// SectionWrapper implementation
	SectionWrapper::SectionWrapper(const char* title, const char* description, const ImVec4& titleColor, bool isVisible) :
		m_shouldDraw(isVisible)
	{
		if (!m_shouldDraw) {
			return;
		}

		// Title, description and content sit tight; sections are separated by the rule below
		ImGui::TextColored(titleColor, "%s", title);
		if (description && strlen(description) > 0)
			ImGui::TextWrapped("%s", description);
	}

	SectionWrapper::~SectionWrapper()
	{
		if (m_shouldDraw) {
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
		}
	}

	SectionWrapper::operator bool() const
	{
		return m_shouldDraw;
	}

	ID3D11ShaderResourceView* GetCategoryIcon(std::string_view category)
	{
		ID3D11ShaderResourceView* categoryIcon = nullptr;
		auto& icons = Util::IconLoader::GetIcons();

		if (category == "Characters") {
			categoryIcon = icons.characters.texture;
		} else if (category == "Display") {
			categoryIcon = icons.display.texture;
		} else if ((category == "Foliage" || category == "Grass")) {
			categoryIcon = icons.grass.texture;
		} else if (category == "Lighting") {
			categoryIcon = icons.lighting.texture;
		} else if (category == "Sky") {
			categoryIcon = icons.sky.texture;
		} else if (category == "Landscape & Textures") {
			categoryIcon = icons.landscape.texture;
		} else if (category == "Water") {
			categoryIcon = icons.water.texture;
		} else if (category == "Utility") {
			categoryIcon = icons.debug.texture;
		} else if (category == "Materials") {
			categoryIcon = icons.materials.texture;
		} else if (category == "Post-Processing") {
			categoryIcon = icons.postProcessing.texture;
		}

		return categoryIcon;
	}

	bool DrawCategoryHeader(const char* categoryKey, const char* displayName, bool& isExpanded)
	{
		// Get the appropriate icon for this category
		ID3D11ShaderResourceView* categoryIcon = GetCategoryIcon(categoryKey);

		// Icon lookup uses the stable category key; the translated label is drawn separately.
		// Draw category header with custom styling
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 pos = ImGui::GetCursorScreenPos();
		float availableWidth = ImGui::GetContentRegionAvail().x;

		// Calculate icon size based on current font size to match text scaling
		// This ensures icons scale consistently with text when the font scale changes
		const float currentFontSize = ImGui::GetFontSize();
		const float iconSize = currentFontSize * C::SECTION_ICON_SCALE;
		const float iconSpacing = currentFontSize * C::SECTION_ICON_SPACING_SCALE;
		const ImVec2 textSize = ImGui::CalcTextSize(displayName);

		// Left-aligned label row: icon, secondary-coloured name, then a faint rule to the right edge
		const float scale = Util::GetUIScale();

		ImGui::PushID(categoryKey);
		ImGui::SetCursorScreenPos(pos);
		const float pad = ThemeManager::Constants::CATEGORY_HEADER_PAD * scale;
		const bool clicked = ImGui::InvisibleButton("##CategoryHeader", ImVec2(availableWidth, textSize.y + pad));
		// The pad is split evenly above and below the label, so the gap to the row above matches the gap below
		pos.y += pad * 0.5f;
		const bool hovered = ImGui::IsItemHovered();

		ImVec4 color = Util::Colors::GetSecondary();
		if (!isExpanded)
			color.w *= ThemeManager::Constants::CATEGORY_HEADER_COLLAPSED_ALPHA;
		const float hover = Anim::ItemBlend(ImGui::GetItemID(), hovered, ImGui::IsItemActive(), false).hover;
		color = Anim::Lerp(color, ImGui::GetStyleColorVec4(ImGuiCol_Text), hover);
		const ImU32 headerColor = ImGui::GetColorU32(color);

		const float padX = ImGui::GetStyle().FramePadding.x;
		float currentX = pos.x + std::max(ImGui::GetStyle().ItemInnerSpacing.x, ThemeManager::Constants::ANIM_SELECTED_BAR_WIDTH * scale);
		if (categoryIcon) {
			const ImVec2 iconPos(currentX, pos.y + (textSize.y - iconSize) * 0.5f);
			drawList->AddImage(categoryIcon, iconPos, ImVec2(iconPos.x + iconSize, iconPos.y + iconSize), ImVec2(0, 0), ImVec2(1, 1), headerColor);
			currentX += iconSize + iconSpacing;
		}
		drawList->AddText(ImVec2(currentX, pos.y), headerColor, displayName);
		currentX += textSize.x + iconSpacing;

		const float lineY = pos.y + textSize.y * 0.5f;
		if (currentX < pos.x + availableWidth - padX) {
			ImVec4 line = ImGui::GetStyleColorVec4(ImGuiCol_Separator);
			line.w *= ThemeManager::Constants::CATEGORY_HEADER_LINE_ALPHA;
			drawList->AddLine(ImVec2(currentX, lineY), ImVec2(pos.x + availableWidth - padX, lineY), ImGui::GetColorU32(line), ThemeManager::Constants::SEPARATOR_THICKNESS * scale);
		}

		if (clicked)
			isExpanded = !isExpanded;

		ImGui::PopID();

		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + textSize.y + pad * 0.5f));
		ImGui::Dummy(ImVec2(availableWidth, 0.0f));
		return clicked;
	}

	bool DrawSectionHeader(const char* sectionName, bool useWhiteText, bool isCollapsible, bool* isExpanded, IconDrawCallback icon)
	{
		bool stateChanged = false;
		const float scale = GetUIScale();

		// Use Menu theme colors for consistent styling
		auto& theme = globals::menu->GetTheme().FeatureHeading;
		auto& palette = globals::menu->GetTheme().Palette;
		// When useWhiteText is true, use the theme's text color instead of hardcoded white
		ImVec4 color = useWhiteText ? palette.Text : theme.ColorDefault;

		ImU32 headerColor = ImGui::GetColorU32(color);

		if (isCollapsible && isExpanded) {
			// Use collapsible header similar to DrawCategoryHeader
			ImGui::PushID(sectionName);

			ImGui::PushStyleColor(ImGuiCol_Text, headerColor);

			if (ImGui::CollapsingHeader(sectionName, ImGuiTreeNodeFlags_DefaultOpen)) {
				if (!*isExpanded) {
					stateChanged = true;
				}
				*isExpanded = true;
			} else {
				if (*isExpanded) {
					stateChanged = true;
				}
				*isExpanded = false;
			}

			ImGui::PopStyleColor();
			ImGui::PopID();
		} else {
			// Non-collapsible header - use custom styled header similar to CategoryHeader
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			ImVec2 pos = ImGui::GetCursorScreenPos();
			float availableWidth = ImGui::GetContentRegionAvail().x;
			ImVec2 textSize = ImGui::CalcTextSize(sectionName);
			const float iconSize = icon ? ImGui::GetFontSize() : 0.0f;
			const float iconWidth = icon ? iconSize + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f;
			const float contentWidth = textSize.x + iconWidth;

			// Calculate line positions
			float lineY = pos.y + textSize.y * 0.5f;
			float lineLength = (availableWidth - contentWidth - ThemeManager::Constants::CATEGORY_LABEL_GAP * scale * 2.0f) * 0.5f;  // 20px for padding

			// Left line
			if (lineLength > 0) {
				drawList->AddLine(ImVec2(pos.x, lineY), ImVec2(pos.x + lineLength, lineY), headerColor, ThemeManager::Constants::SEPARATOR_THICKNESS * scale);
			}

			// Right line
			float rightLineStart = pos.x + lineLength + ThemeManager::Constants::CATEGORY_LABEL_GAP * scale + contentWidth + ThemeManager::Constants::CATEGORY_LABEL_GAP * scale;
			if (rightLineStart < pos.x + availableWidth) {
				drawList->AddLine(ImVec2(rightLineStart, lineY), ImVec2(pos.x + availableWidth, lineY), headerColor, ThemeManager::Constants::SEPARATOR_THICKNESS * scale);
			}

			// Center text
			ImVec2 textPos = ImVec2(pos.x + lineLength + ThemeManager::Constants::CATEGORY_LABEL_GAP * scale, pos.y + ThemeManager::Constants::CATEGORY_ICON_OFFSET * scale);
			if (icon) {
				const ImVec2 iconMin(textPos.x, textPos.y + (textSize.y - iconSize) * 0.5f);
				icon(drawList, iconMin, ImVec2(iconMin.x + iconSize, iconMin.y + iconSize), headerColor);
				textPos.x += iconWidth;
			}
			drawList->AddText(textPos, headerColor, sectionName);

			// Move cursor to next line
			ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + textSize.y + ThemeManager::Constants::CATEGORY_ROW_GAP * scale));
			ImGui::Dummy(ImVec2(availableWidth, 0.0f));
		}

		return stateChanged;
	}

	// ColorCodedValueConfig static helper implementations
	ColorCodedValueConfig ColorCodedValueConfig::HighIsBad(float low, float med, float high)
	{
		ColorCodedValueConfig config;
		const auto& theme = globals::menu->GetTheme().StatusPalette;
		config.thresholds = {
			{ low, theme.Disable },    // Very low - gray
			{ med, theme.InfoColor },  // Low - blue
			{ high, theme.Warning },   // Medium - orange
			{ FLT_MAX, theme.Error }   // High - red (bad)
		};
		return config;
	}

	ColorCodedValueConfig ColorCodedValueConfig::HighIsGood(float low, float med, float high)
	{
		ColorCodedValueConfig config;
		const auto& theme = globals::menu->GetTheme().StatusPalette;
		config.thresholds = {
			{ low, theme.Disable },          // Very low - gray
			{ med, theme.InfoColor },        // Low - blue
			{ high, theme.Warning },         // Medium - orange
			{ FLT_MAX, theme.SuccessColor }  // High - green (good)
		};
		return config;
	}

	void DrawColorCodedValue(
		const std::string& label,
		float valueToCheck,
		const std::string& valueStr,
		const ColorCodedValueConfig& config,
		bool useBullet)
	{
		// Display label
		if (useBullet) {
			ImGui::BulletText("%s", label.c_str());
		} else {
			ImGui::Text("%s", label.c_str());
		}
		if (config.sameLine) {
			ImGui::SameLine();
		}

		// Determine color based on thresholds
		ImVec4 valueColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // Default white
		for (const auto& tc : config.thresholds) {
			if (valueToCheck < tc.threshold) {
				valueColor = tc.color;
				break;
			}
		}

		// Display colored value (arbitrary string)
		ImGui::TextColored(valueColor, "%s", valueStr.c_str());

		// Add tooltip if provided
		if (config.tooltipText) {
			Util::HoverTip(config.tooltipText);
		}
	}

	void DrawMultiLineTooltip(const std::vector<std::string>& lines, const std::vector<ImVec4>& colors)
	{
		for (size_t i = 0; i < lines.size(); ++i) {
			const char* lineCStr = lines[i].c_str();
			if (!colors.empty() && i < colors.size()) {
				// Use provided color for this line
				ImGui::TextColored(colors[i], "%s", lineCStr);
			} else {
				// Use default color
				ImGui::Text("%s", lineCStr);
			}
		}
	}

	void DrawColoredMultiLineTooltip(const ColoredTextLines& lines)
	{
		for (const auto& line : lines) {
			ImGui::TextColored(line.color, "%s", line.text.c_str());
		}
	}

	void SortTableRowsByColumn(std::vector<std::vector<std::string>>& rows, size_t column, bool ascending)
	{
		std::sort(rows.begin(), rows.end(), [column, ascending](const auto& a, const auto& b) {
			if (column >= a.size() || column >= b.size())
				return false;
			return ascending ? (a[column] < b[column]) : (a[column] > b[column]);
		});
	}

	bool VersionStringLess(const std::string& a, const std::string& b, bool ascending)
	{
		const auto parsePart = [](const std::string& part) {
			try {
				return std::stoi(part);
			} catch (...) {
				return 0;
			}
		};
		const auto split = [&](const std::string& s) {
			std::vector<int> parts;
			size_t start = 0, end = 0;
			while ((end = s.find('.', start)) != std::string::npos) {
				parts.push_back(parsePart(s.substr(start, end - start)));
				start = end + 1;
			}
			if (start < s.size())
				parts.push_back(parsePart(s.substr(start)));
			return parts;
		};
		auto va = split(a), vb = split(b);
		for (size_t i = 0; i < std::max(va.size(), vb.size()); ++i) {
			int ai = i < va.size() ? va[i] : 0;
			int bi = i < vb.size() ? vb[i] : 0;
			if (ai != bi)
				return ascending ? (ai < bi) : (ai > bi);
		}
		return false;
	}

	bool VersionSortComparator(const std::string& a, const std::string& b, bool asc)
	{
		return VersionStringLess(a, b, asc);
	}

	bool StringSortComparator(const std::string& a, const std::string& b, bool ascending)
	{
		return ascending ? (a < b) : (b < a);
	}

	void RenderTextWithHighlights(const std::string& text, const std::string& searchTerm, ImVec4 highlightColor)
	{
		if (searchTerm.empty()) {
			ImGui::TextUnformatted(text.c_str());
			return;
		}

		const std::string lowerText = Util::ToLowerAscii(text);
		const std::string lowerSearch = Util::ToLowerAscii(searchTerm);

		size_t pos = 0;
		size_t lastPos = 0;

		while ((pos = lowerText.find(lowerSearch, lastPos)) != std::string::npos) {
			// Render text before highlight
			if (pos > lastPos) {
				ImGui::TextUnformatted(text.substr(lastPos, pos - lastPos).c_str());
				ImGui::SameLine(0, 0);
			}

			// Render highlighted text
			ImGui::PushStyleColor(ImGuiCol_Text, highlightColor);
			ImGui::TextUnformatted(text.substr(pos, searchTerm.length()).c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine(0, 0);

			lastPos = pos + searchTerm.length();
		}

		// Render remaining text
		if (lastPos < text.length()) {
			ImGui::TextUnformatted(text.substr(lastPos).c_str());
		}
	}

	ImVec4 GetThresholdColor(float value, float good, float warn, ImVec4 goodColor, ImVec4 warnColor, ImVec4 badColor)
	{
		if (value < good)
			return goodColor;
		else if (value < warn)
			return warnColor;
		else
			return badColor;
	}

	bool FeatureMatchesSearch(Feature* feat, const std::string& searchQuery)
	{
		if (searchQuery.empty())
			return true;

		return StringMatchesSearch(feat->GetShortName(), searchQuery) ||
		       StringMatchesSearch(feat->GetName(), searchQuery) ||
		       StringMatchesSearch(feat->GetDisplayName(), searchQuery) ||
		       StringMatchesSearch(std::string(feat->GetCategory()), searchQuery) ||
		       StringMatchesSearch(feat->GetDisplayCategory(), searchQuery);
	}

	bool StringMatchesSearch(std::string_view text, std::string_view searchQuery)
	{
		return searchQuery.empty() || FoldForSearch(text).find(FoldForSearch(searchQuery)) != std::string::npos;
	}

	void DrawModalBackground(uint8_t alpha)
	{
		auto& io = ImGui::GetIO();
		ImGui::GetBackgroundDrawList()->AddRectFilled(
			ImVec2(0, 0),
			io.DisplaySize,
			IM_COL32(0, 0, 0, alpha));
	}

	void DrawBreathingText(const char* text, std::optional<ImVec4> color, float speed, float minAlpha, float maxAlpha)
	{
		float alphaRange = maxAlpha - minAlpha;
		float breathe = minAlpha + alphaRange * 0.5f * (1.0f + sinf((float)ImGui::GetTime() * speed));
		const ImVec4 base = color.value_or(globals::menu->GetTheme().Palette.Text);
		ImGui::TextColored(ImVec4(base.x, base.y, base.z, breathe), "%s", text);
	}

	ImVec4 GetPulsingColor(const ImVec4& baseColor, float speed, float minBrightness, float maxBrightness)
	{
		float brightnessRange = maxBrightness - minBrightness;
		float pulse = minBrightness + brightnessRange * 0.5f * (1.0f + sinf((float)ImGui::GetTime() * speed));
		return ImVec4(
			baseColor.x * pulse,
			baseColor.y * pulse,
			baseColor.z * pulse,
			baseColor.w);
	}

	ImVec4 GetReleaseStageColor(Feature::ReleaseStage stage)
	{
		auto& statusPalette = globals::menu->GetTheme().StatusPalette;
		return stage == Feature::ReleaseStage::Alpha ? statusPalette.Error : statusPalette.Warning;
	}

	std::string AppendReleaseStageTag(std::string_view label, Feature::ReleaseStage stage)
	{
		const auto tag = Feature::GetReleaseStageTag(stage);
		if (tag.empty())
			return std::string(label);
		return std::format("{} {}", label, tag);
	}

	void PushTintedFrameStyle(const ImVec4& color)
	{
		using Constants = ThemeManager::Constants;
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(color.x, color.y, color.z, Surface::FillOpacity(Constants::TINTED_FRAME_BG_ALPHA)));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(color.x, color.y, color.z, Surface::FillOpacity(Constants::TINTED_FRAME_BG_HOVERED_ALPHA)));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(color.x, color.y, color.z, Surface::FillOpacity(Constants::TINTED_FRAME_BG_ACTIVE_ALPHA)));
		ImGui::PushStyleColor(ImGuiCol_Border, color);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, Constants::TINTED_FRAME_BORDER_SIZE * GetUIScale());
	}

	void PopTintedFrameStyle()
	{
		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar();
	}

	namespace detail
	{
		constexpr std::size_t kSearchableComboBufferSize = 256;
		constexpr int kSearchableComboPruneIntervalFrames = 600;
		constexpr int kSearchableComboStaleFrames = 3600;

		struct SearchableComboState
		{
			std::array<char, kSearchableComboBufferSize> filter{};
			std::uint64_t filterRevision = 0;
			bool open = false;
			int lastSeenFrame = 0;
			int lastOpenFrame = -1;

			const void* cachedUserData = nullptr;
			SearchableComboLabelGetter cachedGetter = nullptr;
			std::uint64_t cachedItemsRevision = 0;
			std::uint64_t cachedFilterRevision = std::numeric_limits<std::uint64_t>::max();
			int cachedItemCount = -1;
			std::vector<int> filteredIndices;
		};

		struct SearchableComboFrame
		{
			SearchableComboState* state = nullptr;
			ImGuiLastItemData openerItem;
			float* savedScrollY = nullptr;
			bool restoreScroll = false;
		};

		struct SearchableComboStorage
		{
			ImGuiContext* context = nullptr;
			std::unordered_map<ImGuiID, SearchableComboState> states;
			std::vector<SearchableComboFrame> frames;
			int lastPruneFrame = 0;
		};

		SearchableComboStorage& GetSearchableComboStorage()
		{
			static SearchableComboStorage storage;
			if (storage.context != GImGui) {
				storage = {};
				storage.context = GImGui;
			}
			return storage;
		}

		SearchableComboState& GetSearchableComboState(ImGuiID id)
		{
			auto& storage = GetSearchableComboStorage();
			const int frame = ImGui::GetFrameCount();
			if (frame < storage.lastPruneFrame ||
				frame - storage.lastPruneFrame >= kSearchableComboPruneIntervalFrames) {
				std::erase_if(storage.states, [frame](const auto& entry) {
					return frame >= entry.second.lastSeenFrame &&
					       frame - entry.second.lastSeenFrame >= kSearchableComboStaleFrames;
				});
				storage.lastPruneFrame = frame;
			}

			auto& state = storage.states[id];
			state.lastSeenFrame = frame;
			return state;
		}

		void ClearSearchableComboFilter(SearchableComboState& state)
		{
			if (state.filter.front() == '\0')
				return;
			state.filter.front() = '\0';
			++state.filterRevision;
		}

		constexpr unsigned char FoldAsciiCase(unsigned char value) noexcept
		{
			return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
		}

		bool ContainsCaseInsensitive(std::string_view text, std::string_view filter) noexcept
		{
			if (filter.empty())
				return true;
			if (filter.size() > text.size())
				return false;
			return std::search(text.begin(), text.end(), filter.begin(), filter.end(), [](char left, char right) {
				return FoldAsciiCase(static_cast<unsigned char>(left)) ==
				       FoldAsciiCase(static_cast<unsigned char>(right));
			}) != text.end();
		}

		int GetSearchableComboMaxItemCount(ImGuiComboFlags flags) noexcept
		{
			if ((flags & ImGuiComboFlags_HeightSmall) != 0)
				return 4;
			if ((flags & ImGuiComboFlags_HeightLarge) != 0)
				return 20;
			if ((flags & ImGuiComboFlags_HeightLargest) != 0)
				return -1;
			return 8;
		}

		float GetSearchableComboPopupHeight(ImGuiComboFlags flags, int maxVisibleItems)
		{
			const int itemCount = maxVisibleItems > 0 ?
			                          maxVisibleItems :
			                          GetSearchableComboMaxItemCount(flags);
			if (itemCount < 0)
				return FLT_MAX;

			const auto& style = ImGui::GetStyle();
			const float listHeight = ImGui::GetTextLineHeightWithSpacing() * itemCount - style.ItemSpacing.y;
			const float searchHeight = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
			return searchHeight + listHeight + style.WindowPadding.y * 2.0f;
		}

		void SetSearchableComboPopupConstraints(
			const char* previewValue, ImGuiComboFlags flags, int maxVisibleItems)
		{
			if ((GImGui->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSizeConstraint) != 0)
				return;

			const auto& style = ImGui::GetStyle();
			float popupWidth = ImGui::CalcItemWidth();
			if ((flags & ImGuiComboFlags_NoPreview) != 0) {
				popupWidth = ImGui::GetFrameHeight();
			} else if ((flags & ImGuiComboFlags_WidthFitPreview) != 0) {
				const float arrowWidth = (flags & ImGuiComboFlags_NoArrowButton) != 0 ? 0.0f : ImGui::GetFrameHeight();
				const float previewWidth = previewValue ? ImGui::CalcTextSize(previewValue, nullptr, true).x : 0.0f;
				popupWidth = arrowWidth + previewWidth + style.FramePadding.x * 2.0f;
			}

			const float searchWidth = ImGui::CalcTextSize(T("ui.search", "Search...")).x +
			                          Icons::CalcGlyphSize(Icons::TI(MenuIcons::kSearch)).x +
			                          style.FramePadding.x * 4.0f;
			popupWidth = std::max(popupWidth, searchWidth);
			ImGui::SetNextWindowSizeConstraints(
				ImVec2(popupWidth, 0.0f),
				ImVec2(popupWidth, GetSearchableComboPopupHeight(flags, maxVisibleItems)));
		}

		struct ComboSearchState
		{
			char buffer[256] = {};
			bool needsFocus = true;
		};

		static std::unordered_map<std::string, ComboSearchState>& GetComboSearchStates()
		{
			static std::unordered_map<std::string, ComboSearchState> states;
			return states;
		}
	}

	bool BeginSearchableCombo(
		const char* label, const char* previewValue, ImGuiComboFlags flags,
		const void* storageAddress, int maxVisibleItems, float* savedScrollY)
	{
		const ImGuiID id = ImGui::GetID(label);
		auto& state = detail::GetSearchableComboState(id);
		detail::SetSearchableComboPopupConstraints(previewValue, flags, maxVisibleItems);

		bool open = false;
		if (storageAddress) {
			ActiveControlStorageGuard storageGuard(storageAddress);
			open = ImGui::BeginCombo(label, previewValue, flags);
		} else {
			open = ImGui::BeginCombo(label, previewValue, flags);
		}
		const ImGuiLastItemData openerItem = GImGui->LastItemData;
		if (!open) {
			if (state.open)
				detail::ClearSearchableComboFilter(state);
			state.open = false;
			return false;
		}

		const int frame = ImGui::GetFrameCount();
		const bool popupAppearing = ImGui::IsWindowAppearing();
		const bool continuingOpen = state.open && state.lastOpenFrame == frame - 1 && !popupAppearing;
		if (state.open && !continuingOpen)
			detail::ClearSearchableComboFilter(state);
		const bool focusSearch = !continuingOpen && (!savedScrollY || *savedScrollY == 0.0f);
		state.open = true;
		state.lastOpenFrame = frame;
		auto& comboStorage = detail::GetSearchableComboStorage();
		comboStorage.frames.push_back({ &state, openerItem, savedScrollY, !continuingOpen });

		ImGui::PushID(id);
		if (focusSearch)
			ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(-FLT_MIN);
		const float scale = GetSearchUIScale();
		const float iconSize = Icons::CalcGlyphSize(Icons::TI(MenuIcons::kSearch)).x;
		const float iconOffsetX = ThemeManager::Constants::COMBO_SEARCH_ICON_OFFSET_X * scale;
		const float paddingLeft = iconOffsetX + iconSize + ImGui::GetStyle().ItemInnerSpacing.x;
		ImGui::PushStyleVar(
			ImGuiStyleVar_FramePadding, ImVec2(paddingLeft, ImGui::GetStyle().FramePadding.y));
		bool filterChanged = false;
		{
			ActiveControlStorageGuard storageGuard(nullptr);
			filterChanged = ImGui::InputTextWithHint("##search", T("ui.search", "Search..."),
				state.filter.data(), state.filter.size());
		}
		ImGui::PopStyleVar();
		if (filterChanged)
			++state.filterRevision;

		const ImVec2 iconPosition(
			ImGui::GetItemRectMin().x + iconOffsetX,
			ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - iconSize) * 0.5f);
		DrawSearchIcon(iconPosition, iconSize, ThemeManager::Constants::COMBO_SEARCH_ICON_ALPHA);
		ImGui::Separator();
		ImGui::PopID();
		GImGui->LastItemData = openerItem;
		return true;
	}

	void EndSearchableCombo()
	{
		auto& storage = detail::GetSearchableComboStorage();
		IM_ASSERT(!storage.frames.empty() && "EndSearchableCombo called without BeginSearchableCombo");
		if (storage.frames.empty())
			return;

		const ImGuiLastItemData openerItem = storage.frames.back().openerItem;
		if (auto* savedScrollY = storage.frames.back().savedScrollY) {
			if (storage.frames.back().restoreScroll)
				ImGui::SetScrollY(*savedScrollY);
			else
				*savedScrollY = ImGui::GetScrollY();
		}
		storage.frames.pop_back();
		ImGui::EndCombo();
		GImGui->LastItemData = openerItem;
	}

	std::string_view GetSearchableComboFilter()
	{
		const auto& frames = detail::GetSearchableComboStorage().frames;
		return frames.empty() ? std::string_view{} : std::string_view(frames.back().state->filter.data());
	}

	bool SearchableComboMatches(std::string_view text)
	{
		return detail::ContainsCaseInsensitive(text, GetSearchableComboFilter());
	}

	void SearchableComboClipper::Begin(int itemCount, SearchableComboLabelGetter getter,
		const void* userData, std::uint64_t itemsRevision, float itemHeight)
	{
		filteredIndices = nullptr;
		itemCount = std::max(itemCount, 0);
		auto& frames = detail::GetSearchableComboStorage().frames;
		if (frames.empty() || !getter || GetSearchableComboFilter().empty()) {
			clipper.Begin(itemCount, itemHeight);
			return;
		}

		auto& state = *frames.back().state;
		if (state.cachedUserData != userData || state.cachedGetter != getter ||
			state.cachedItemsRevision != itemsRevision || state.cachedItemCount != itemCount ||
			state.cachedFilterRevision != state.filterRevision) {
			state.filteredIndices.clear();
			state.filteredIndices.reserve(static_cast<std::size_t>(itemCount));
			const std::string_view filter(state.filter.data());
			for (int itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
				const char* label = getter(userData, itemIndex);
				if (label && detail::ContainsCaseInsensitive(label, filter))
					state.filteredIndices.push_back(itemIndex);
			}
			state.cachedUserData = userData;
			state.cachedGetter = getter;
			state.cachedItemsRevision = itemsRevision;
			state.cachedItemCount = itemCount;
			state.cachedFilterRevision = state.filterRevision;
		}

		filteredIndices = &state.filteredIndices;
		clipper.Begin(static_cast<int>(filteredIndices->size()), itemHeight);
	}

	void SearchableComboClipper::IncludeItemByIndex(int itemIndex)
	{
		if (!filteredIndices) {
			if (itemIndex >= 0 && itemIndex < clipper.ItemsCount)
				clipper.IncludeItemByIndex(itemIndex);
			return;
		}

		const auto found = std::lower_bound(filteredIndices->begin(), filteredIndices->end(), itemIndex);
		if (found != filteredIndices->end() && *found == itemIndex)
			clipper.IncludeItemByIndex(static_cast<int>(found - filteredIndices->begin()));
	}

	bool SearchableComboClipper::Step()
	{
		return clipper.Step();
	}

	int SearchableComboClipper::GetDisplayStart() const noexcept
	{
		return clipper.DisplayStart;
	}

	int SearchableComboClipper::GetDisplayEnd() const noexcept
	{
		return clipper.DisplayEnd;
	}

	int SearchableComboClipper::GetItemIndex(int displayIndex) const noexcept
	{
		if (!filteredIndices)
			return displayIndex;
		if (displayIndex < 0 || displayIndex >= static_cast<int>(filteredIndices->size()))
			return -1;
		return (*filteredIndices)[displayIndex];
	}

	bool SearchableCombo(const char* label, int* currentItem, int itemCount,
		SearchableComboLabelGetter getter, const void* userData,
		ImGuiComboFlags flags, std::uint64_t itemsRevision)
	{
		if (!currentItem || !getter)
			return false;

		const char* previewValue = *currentItem >= 0 && *currentItem < itemCount ?
		                               getter(userData, *currentItem) :
		                               nullptr;
		if (!BeginSearchableCombo(label, previewValue, flags, currentItem))
			return false;

		bool changed = false;
		const bool scrollToSelection = ImGui::IsWindowAppearing() && GetSearchableComboFilter().empty();
		SearchableComboClipper itemClipper;
		itemClipper.Begin(itemCount, getter, userData, itemsRevision);
		itemClipper.IncludeItemByIndex(*currentItem);
		while (itemClipper.Step()) {
			for (int displayIndex = itemClipper.GetDisplayStart();
				displayIndex < itemClipper.GetDisplayEnd(); ++displayIndex) {
				const int itemIndex = itemClipper.GetItemIndex(displayIndex);
				const char* itemLabel = getter(userData, itemIndex);
				if (!itemLabel)
					continue;
				const bool selected = itemIndex == *currentItem;
				ImGui::PushID(itemIndex);
				if (RoundedSelectable(itemLabel, selected)) {
					changed = !selected;
					*currentItem = itemIndex;
				}
				if (selected && scrollToSelection)
					ImGui::SetScrollHereY(0.5f);
				ImGui::PopID();
			}
		}
		EndSearchableCombo();
		if (changed)
			ImGui::MarkItemEdited(GImGui->LastItemData.ID);
		return changed;
	}

	std::string DrawComboSearchInput(const char* id)
	{
		auto& state = detail::GetComboSearchStates()[id];

		if (state.needsFocus) {
			ImGui::SetKeyboardFocusHere();
			state.needsFocus = false;
		}

		const float scale = GetSearchUIScale();
		constexpr float iconAlpha = ThemeManager::Constants::COMBO_SEARCH_ICON_ALPHA;
		const float iconOffsetX = ThemeManager::Constants::COMBO_SEARCH_ICON_OFFSET_X * scale;
		const float iconSize = Icons::CalcGlyphSize(Icons::TI(MenuIcons::kSearch)).x;
		const float paddingLeft = iconOffsetX + iconSize + ImGui::GetStyle().ItemInnerSpacing.x;

		char widgetId[128];
		snprintf(widgetId, sizeof(widgetId), "##%s_search", id);

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(paddingLeft, ImGui::GetStyle().FramePadding.y));
		ImGui::InputTextWithHint(widgetId, T("ui.search", "Search..."), state.buffer, IM_ARRAYSIZE(state.buffer));
		ImGui::PopStyleVar();

		ImVec4 iconColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
		iconColor.w *= iconAlpha;
		Icons::DrawCenteredGlyph(ImGui::GetWindowDrawList(), ImVec2(ImGui::GetItemRectMin().x + iconOffsetX, ImGui::GetItemRectMin().y),
			ImVec2(iconSize, ImGui::GetItemRectSize().y), Icons::TI(MenuIcons::kSearch), ImGui::GetColorU32(iconColor));

		ImGui::Separator();

		return state.buffer;
	}

	void ClearComboSearch(const char* id)
	{
		auto& state = detail::GetComboSearchStates()[id];
		state.buffer[0] = '\0';
		state.needsFocus = true;
	}

	void DrawSearchIcon(const ImVec2& position, float size, float alpha)
	{
		const auto icon = Icons::TI(ICON_TI_SEARCH);
		Icons::FontGuard font(icon);
		const ImVec2 extent(size, size);
		auto color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
		color.w *= alpha;
		ImGui::GetWindowDrawList()->AddText(Icons::GetCenteredGlyphPos(position, extent, icon.utf8), ImGui::GetColorU32(color), icon.utf8);
	}

	void DrawFeatureSearchBar(std::string& searchString, float availableWidth)
	{
		MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);
		ImGui::PushID("FeatureSearchBar");

		const float iconColumnWidth = ImGui::GetTextLineHeight();
		const float iconSize = std::min(iconColumnWidth, ThemeManager::Constants::SEARCH_ICON_SIZE * GetSearchUIScale());
		const float iconSpace = iconColumnWidth + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(" ").x;

		if (availableWidth <= 0.0f) {
			availableWidth = ImGui::GetContentRegionAvail().x;
		}

		// Custom style - always transparent background to avoid click blocking
		ImVec4 bgColor = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		ImVec4 bgColorActive = C::FEATURE_TOGGLE_ACTIVE_BG;
		// Use theme text color instead of hardcoded color
		auto& palette = globals::menu->GetTheme().Palette;
		ImVec4 textColor = palette.Text;

		ImGui::PushStyleColor(ImGuiCol_FrameBg, bgColor);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, bgColor);
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, bgColorActive);
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Text, textColor);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(iconSpace, 0.0f));

		// Draw the input field
		ImGui::SetNextItemWidth(availableWidth);
		char buffer[256];
		strncpy_s(buffer, searchString.c_str(), sizeof(buffer) - 1);
		buffer[sizeof(buffer) - 1] = '\0';

		if (ImGui::InputTextWithHint("##feature_search", T("ui.search_features", "Search Features..."), buffer, sizeof(buffer))) {
			searchString = buffer;
		}

		// Draw search icon using the reusable function
		const ImVec2 inputMin = ImGui::GetItemRectMin();
		const ImVec2 iconPos(inputMin.x + (iconColumnWidth - iconSize) * 0.5f, inputMin.y + (ImGui::GetItemRectSize().y - iconSize) * 0.5f);
		ImVec4 iconColor = textColor;
		iconColor.w *= ThemeManager::Constants::SEARCH_ICON_ALPHA;
		Icons::DrawCenteredGlyph(ImGui::GetWindowDrawList(), iconPos, ImVec2(iconSize, iconSize), Icons::TI(MenuIcons::kSearch), ImGui::GetColorU32(iconColor));

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(5);
		ImGui::PopID();
	}

	TableSortSpec ReadTableSortSpec(int defaultColumn, bool defaultAscending)
	{
		TableSortSpec spec{ defaultColumn, defaultAscending };
		if (const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0) {
			spec.column = sortSpecs->Specs->ColumnIndex;
			spec.ascending = sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending;
		}
		return spec;
	}

	void ShowSortedStringTableStrings(
		const char* table_id,
		const std::vector<std::string>& headers,
		const std::vector<std::vector<std::string>>& rows,
		size_t sortColumn,
		bool ascending,
		const std::vector<TableSortFunc>& customSorts,
		TableCellRenderFunc cellRender)
	{
		ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable;
		if (ImGui::BeginTable(table_id, static_cast<int>(headers.size()), flags)) {
			for (const auto& header : headers)
				ImGui::TableSetupColumn(header.c_str());
			ImGui::TableHeadersRow();

			const TableSortSpec spec = ReadTableSortSpec(static_cast<int>(sortColumn), ascending);

			// Make a copy if sorting is needed
			std::vector<std::vector<std::string>> sortedRows = rows;
			if (spec.column >= 0 && static_cast<size_t>(spec.column) < headers.size()) {
				// Fallback to default string sort if no custom sort is provided
				auto cmp = (spec.column < static_cast<int>(customSorts.size()) && customSorts[spec.column]) ? customSorts[spec.column] : StringSortComparator;
				const size_t sortCol = static_cast<size_t>(spec.column);
				SortTableRowsWith<std::vector<std::string>>(sortedRows, spec, [sortCol, &cmp](const std::vector<std::string>& a, const std::vector<std::string>& b, bool sortAsc) {
					const std::string& aVal = (sortCol < a.size()) ? a[sortCol] : std::string();
					const std::string& bVal = (sortCol < b.size()) ? b[sortCol] : std::string();
					return cmp(aVal, bVal, sortAsc);
				});
			}

			// Render rows
			for (size_t rowIdx = 0; rowIdx < sortedRows.size(); ++rowIdx) {
				const auto& row = sortedRows[rowIdx];
				ImGui::TableNextRow();
				for (size_t col = 0; col < headers.size(); ++col) {
					ImGui::TableSetColumnIndex(static_cast<int>(col));
					if (cellRender) {
						const std::string& value = (col < row.size()) ? row[col] : std::string();
						cellRender(static_cast<int>(rowIdx), static_cast<int>(col), value);
					} else {
						if (col < row.size())
							ImGui::TextUnformatted(row[col].c_str());
					}
				}
			}
			ImGui::EndTable();
		}
	}

	bool SelectTabByLabel(std::string_view label)
	{
		ImGuiContext& context = *ImGui::GetCurrentContext();
		for (auto& [id, bar] : g_pillTabBars) {
			if (bar.lastFrameVisible < context.FrameCount - 1 || bar.lastFrameVisible > context.FrameCount)
				continue;
			const auto selectTab = [&](const std::vector<PillTabItemState>& tabs) {
				for (const auto& tab : tabs) {
					if (TabLabelMatches(tab.label, label)) {
						bar.selectedId = tab.id;
						bar.nextSelectedId = 0;
						return true;
					}
				}
				return false;
			};
			if (bar.lastFrameVisible == context.FrameCount && std::ranges::find(g_pillTabStack, id) != g_pillTabStack.end() &&
				selectTab(bar.itemsThisFrame))
				return true;
			if (selectTab(bar.itemsForStrip))
				return true;
		}
		for (int i = 0; i < context.TabBars.GetMapSize(); ++i) {
			ImGuiTabBar* bar = context.TabBars.TryGetMapData(i);
			if (!bar || bar->PrevFrameVisible < context.FrameCount - 1)
				continue;
			for (ImGuiTabItem& tab : bar->Tabs) {
				if (TabLabelMatches(ImGui::TabBarGetTabName(bar, &tab), label)) {
					bar->NextSelectedTabId = tab.ID;
					return true;
				}
			}
		}
		return false;
	}

	void DrawOpenFolderSelectable(const char* label, const wchar_t* pluginDir)
	{
		if (RoundedSelectable(label, false)) {
			auto realPath = Util::PathHelpers::GetRealPathFromDataRelative(pluginDir);
			ShellExecuteW(nullptr, L"open", realPath.empty() ? pluginDir : realPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
	}

	void DrawDllVersionTable(
		const char* label,
		const wchar_t* pluginDir,
		const std::vector<std::pair<std::string, std::string>>& dllVersions,
		const char* tableId)
	{
		DrawOpenFolderSelectable(label, pluginDir);
		std::vector<std::string> headers = { T("ui.table.dll_name", "DLL Name"), T("ui.table.version", "Version") };
		std::vector<std::vector<std::string>> rows;
		rows.reserve(dllVersions.size());
		for (const auto& [name, version] : dllVersions)
			rows.push_back({ name, version });
		std::vector<TableSortFunc> sorters = { nullptr, VersionSortComparator };
		ShowSortedStringTableStrings(tableId, headers, rows, 0, true, sorters);
	}

	void DropdownArrowButton(const char* popupId, const char* tooltip, bool disabled, const char* disabledTooltip)
	{
		const ImVec2 fieldMin = ImGui::GetItemRectMin();
		ImGui::SameLine(0.0f, 0.0f);
		{
			const DisableGuard disableGuard(disabled);
			if (ImGui::ArrowButton(std::format("##Arrow{}", popupId).c_str(), ImGuiDir_Down))
				ImGui::OpenPopup(popupId);
		}
		AddTooltip(disabled && disabledTooltip ? disabledTooltip : tooltip, kTooltipWhenDisabled);

		const ImVec2 buttonMax = ImGui::GetItemRectMax();
		ImGui::SetNextWindowPos(ImVec2(fieldMin.x, buttonMax.y + ImGui::GetStyle().ItemInnerSpacing.y));
		ImGui::SetNextWindowSizeConstraints(ImVec2(buttonMax.x - fieldMin.x, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
	}

	// Theme-aware color accessor functions
	namespace Colors
	{
		ImVec4 GetTimerCritical()
		{
			return globals::menu->GetTheme().StatusPalette.Error;
		}

		ImVec4 GetDefault()
		{
			return globals::menu->GetTheme().Palette.Text;
		}

		ImVec4 GetSuccess()
		{
			return globals::menu->GetTheme().StatusPalette.SuccessColor;
		}

		ImVec4 GetWarning()
		{
			return globals::menu->GetTheme().StatusPalette.Warning;
		}

		ImVec4 GetError()
		{
			return globals::menu->GetTheme().StatusPalette.Error;
		}

		ImVec4 GetUnsaved()
		{
			auto color = GetError();
			color.w = C::UNSAVED_ALPHA;  // muted: a dirty save affordance without a separate label
			return color;
		}

		ImVec4 GetInfo()
		{
			return globals::menu->GetTheme().StatusPalette.InfoColor;
		}

		ImVec4 GetDisabled()
		{
			return globals::menu->GetTheme().StatusPalette.Disable;
		}

		ImVec4 GetRestartNeeded()
		{
			return globals::menu->GetTheme().StatusPalette.RestartNeeded;
		}

		ImVec4 GetSecondary()
		{
			// Keeps well above AA contrast on the theme's dark panels while staying clearly below primary.
			constexpr float kSecondaryTextAlpha = 0.72f;
			auto color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
			color.w *= kSecondaryTextAlpha;
			return color;
		}

		ImVec4 GetAccent()
		{
			auto color = ImGui::GetStyleColorVec4(ImGuiCol_Header);
			color.w = 1.0f;
			return color;
		}

	}

	namespace Text
	{
		static void ColoredTextV(ImVec4 color, const char* fmt, va_list args)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::TextV(fmt, args);
			ImGui::PopStyleColor();
		}

		static void ColoredTextWrappedV(ImVec4 color, const char* fmt, va_list args)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::TextWrappedV(fmt, args);
			ImGui::PopStyleColor();
		}

#define UTIL_TEXT(Name, ColorFn)                    \
	void Name(const char* fmt, ...)                 \
	{                                               \
		va_list args;                               \
		va_start(args, fmt);                        \
		ColoredTextV(Colors::ColorFn(), fmt, args); \
		va_end(args);                               \
	}
#define UTIL_TEXT_WRAPPED(Name, ColorFn)                   \
	void Name(const char* fmt, ...)                        \
	{                                                      \
		va_list args;                                      \
		va_start(args, fmt);                               \
		ColoredTextWrappedV(Colors::ColorFn(), fmt, args); \
		va_end(args);                                      \
	}

		UTIL_TEXT(Warning, GetWarning)
		UTIL_TEXT_WRAPPED(WrappedWarning, GetWarning)
		UTIL_TEXT(Error, GetError)
		UTIL_TEXT_WRAPPED(WrappedError, GetError)
		UTIL_TEXT(Success, GetSuccess)
		UTIL_TEXT(Info, GetInfo)
		UTIL_TEXT_WRAPPED(WrappedInfo, GetInfo)
		UTIL_TEXT(Disabled, GetDisabled)
		UTIL_TEXT_WRAPPED(WrappedDisabled, GetDisabled)
		UTIL_TEXT(RestartNeeded, GetRestartNeeded)
		UTIL_TEXT_WRAPPED(WrappedRestartNeeded, GetRestartNeeded)
		UTIL_TEXT(Secondary, GetSecondary)
		UTIL_TEXT_WRAPPED(WrappedSecondary, GetSecondary)

#undef UTIL_TEXT
#undef UTIL_TEXT_WRAPPED
	}

	namespace Input
	{
#define IM_VK_KEYPAD_ENTER (VK_RETURN + 256)

		ImGuiKey VirtualKeyToImGuiKey(WPARAM vkKey)
		{
			switch (vkKey) {
			case VK_TAB:
				return ImGuiKey_Tab;
			case VK_LEFT:
				return ImGuiKey_LeftArrow;
			case VK_RIGHT:
				return ImGuiKey_RightArrow;
			case VK_UP:
				return ImGuiKey_UpArrow;
			case VK_DOWN:
				return ImGuiKey_DownArrow;
			case VK_PRIOR:
				return ImGuiKey_PageUp;
			case VK_NEXT:
				return ImGuiKey_PageDown;
			case VK_HOME:
				return ImGuiKey_Home;
			case VK_END:
				return ImGuiKey_End;
			case VK_INSERT:
				return ImGuiKey_Insert;
			case VK_DELETE:
				return ImGuiKey_Delete;
			case VK_BACK:
				return ImGuiKey_Backspace;
			case VK_SPACE:
				return ImGuiKey_Space;
			case VK_RETURN:
				return ImGuiKey_Enter;
			case VK_ESCAPE:
				return ImGuiKey_Escape;
			case VK_OEM_7:
				return ImGuiKey_Apostrophe;
			case VK_OEM_COMMA:
				return ImGuiKey_Comma;
			case VK_OEM_MINUS:
				return ImGuiKey_Minus;
			case VK_OEM_PERIOD:
				return ImGuiKey_Period;
			case VK_OEM_2:
				return ImGuiKey_Slash;
			case VK_OEM_1:
				return ImGuiKey_Semicolon;
			case VK_OEM_PLUS:
				return ImGuiKey_Equal;
			case VK_OEM_4:
				return ImGuiKey_LeftBracket;
			case VK_OEM_5:
				return ImGuiKey_Backslash;
			case VK_OEM_6:
				return ImGuiKey_RightBracket;
			case VK_OEM_3:
				return ImGuiKey_GraveAccent;
			case VK_CAPITAL:
				return ImGuiKey_CapsLock;
			case VK_SCROLL:
				return ImGuiKey_ScrollLock;
			case VK_NUMLOCK:
				return ImGuiKey_NumLock;
			case VK_SNAPSHOT:
				return ImGuiKey_PrintScreen;
			case VK_PAUSE:
				return ImGuiKey_Pause;
			case VK_NUMPAD0:
				return ImGuiKey_Keypad0;
			case VK_NUMPAD1:
				return ImGuiKey_Keypad1;
			case VK_NUMPAD2:
				return ImGuiKey_Keypad2;
			case VK_NUMPAD3:
				return ImGuiKey_Keypad3;
			case VK_NUMPAD4:
				return ImGuiKey_Keypad4;
			case VK_NUMPAD5:
				return ImGuiKey_Keypad5;
			case VK_NUMPAD6:
				return ImGuiKey_Keypad6;
			case VK_NUMPAD7:
				return ImGuiKey_Keypad7;
			case VK_NUMPAD8:
				return ImGuiKey_Keypad8;
			case VK_NUMPAD9:
				return ImGuiKey_Keypad9;
			case VK_DECIMAL:
				return ImGuiKey_KeypadDecimal;
			case VK_DIVIDE:
				return ImGuiKey_KeypadDivide;
			case VK_MULTIPLY:
				return ImGuiKey_KeypadMultiply;
			case VK_SUBTRACT:
				return ImGuiKey_KeypadSubtract;
			case VK_ADD:
				return ImGuiKey_KeypadAdd;
			case IM_VK_KEYPAD_ENTER:
				return ImGuiKey_KeypadEnter;
			case VK_LSHIFT:
				return ImGuiKey_LeftShift;
			case VK_LCONTROL:
				return ImGuiKey_LeftCtrl;
			case VK_LMENU:
				return ImGuiKey_LeftAlt;
			case VK_LWIN:
				return ImGuiKey_LeftSuper;
			case VK_RSHIFT:
				return ImGuiKey_RightShift;
			case VK_RCONTROL:
				return ImGuiKey_RightCtrl;
			case VK_RMENU:
				return ImGuiKey_RightAlt;
			case VK_RWIN:
				return ImGuiKey_RightSuper;
			case VK_APPS:
				return ImGuiKey_Menu;
			case '0':
				return ImGuiKey_0;
			case '1':
				return ImGuiKey_1;
			case '2':
				return ImGuiKey_2;
			case '3':
				return ImGuiKey_3;
			case '4':
				return ImGuiKey_4;
			case '5':
				return ImGuiKey_5;
			case '6':
				return ImGuiKey_6;
			case '7':
				return ImGuiKey_7;
			case '8':
				return ImGuiKey_8;
			case '9':
				return ImGuiKey_9;
			case 'A':
				return ImGuiKey_A;
			case 'B':
				return ImGuiKey_B;
			case 'C':
				return ImGuiKey_C;
			case 'D':
				return ImGuiKey_D;
			case 'E':
				return ImGuiKey_E;
			case 'F':
				return ImGuiKey_F;
			case 'G':
				return ImGuiKey_G;
			case 'H':
				return ImGuiKey_H;
			case 'I':
				return ImGuiKey_I;
			case 'J':
				return ImGuiKey_J;
			case 'K':
				return ImGuiKey_K;
			case 'L':
				return ImGuiKey_L;
			case 'M':
				return ImGuiKey_M;
			case 'N':
				return ImGuiKey_N;
			case 'O':
				return ImGuiKey_O;
			case 'P':
				return ImGuiKey_P;
			case 'Q':
				return ImGuiKey_Q;
			case 'R':
				return ImGuiKey_R;
			case 'S':
				return ImGuiKey_S;
			case 'T':
				return ImGuiKey_T;
			case 'U':
				return ImGuiKey_U;
			case 'V':
				return ImGuiKey_V;
			case 'W':
				return ImGuiKey_W;
			case 'X':
				return ImGuiKey_X;
			case 'Y':
				return ImGuiKey_Y;
			case 'Z':
				return ImGuiKey_Z;
			case VK_F1:
				return ImGuiKey_F1;
			case VK_F2:
				return ImGuiKey_F2;
			case VK_F3:
				return ImGuiKey_F3;
			case VK_F4:
				return ImGuiKey_F4;
			case VK_F5:
				return ImGuiKey_F5;
			case VK_F6:
				return ImGuiKey_F6;
			case VK_F7:
				return ImGuiKey_F7;
			case VK_F8:
				return ImGuiKey_F8;
			case VK_F9:
				return ImGuiKey_F9;
			case VK_F10:
				return ImGuiKey_F10;
			case VK_F11:
				return ImGuiKey_F11;
			case VK_F12:
				return ImGuiKey_F12;
			default:
				return ImGuiKey_None;
			};
		}

		bool IsRejectedByNumericTextEntry(uint32_t character)
		{
			const ImGuiContext& context = *ImGui::GetCurrentContext();
			if (context.TempInputId == 0 || context.ActiveId != context.TempInputId)
				return false;
			return !(character >= '0' && character <= '9') && character != '.' && character != '-';
		}

		uint32_t DIKToVK(uint32_t dikKey)
		{
			switch (dikKey) {
			case DIK_LEFTARROW:
				return VK_LEFT;
			case DIK_RIGHTARROW:
				return VK_RIGHT;
			case DIK_UPARROW:
				return VK_UP;
			case DIK_DOWNARROW:
				return VK_DOWN;
			case DIK_DELETE:
				return VK_DELETE;
			case DIK_END:
				return VK_END;
			case DIK_HOME:
				return VK_HOME;  // pos1
			case DIK_PRIOR:
				return VK_PRIOR;  // page up
			case DIK_NEXT:
				return VK_NEXT;  // page down
			case DIK_INSERT:
				return VK_INSERT;
			case DIK_NUMPAD0:
				return VK_NUMPAD0;
			case DIK_NUMPAD1:
				return VK_NUMPAD1;
			case DIK_NUMPAD2:
				return VK_NUMPAD2;
			case DIK_NUMPAD3:
				return VK_NUMPAD3;
			case DIK_NUMPAD4:
				return VK_NUMPAD4;
			case DIK_NUMPAD5:
				return VK_NUMPAD5;
			case DIK_NUMPAD6:
				return VK_NUMPAD6;
			case DIK_NUMPAD7:
				return VK_NUMPAD7;
			case DIK_NUMPAD8:
				return VK_NUMPAD8;
			case DIK_NUMPAD9:
				return VK_NUMPAD9;
			case DIK_DECIMAL:
				return VK_DECIMAL;
			case DIK_NUMPADENTER:
				return IM_VK_KEYPAD_ENTER;
			case DIK_RMENU:
				return VK_RMENU;  // right alt
			case DIK_RCONTROL:
				return VK_RCONTROL;  // right control
			case DIK_LWIN:
				return VK_LWIN;  // left win
			case DIK_RWIN:
				return VK_RWIN;  // right win
			case DIK_APPS:
				return VK_APPS;
			case DIK_SYSRQ:
				return VK_SNAPSHOT;
			default:
				return dikKey;
			}
		}

		const char* KeyIdToString(uint32_t key)
		{
			if (key >= 256)
				return "";

			static const char* keyboard_keys_international[256] = {
				"", "Left Mouse", "Right Mouse", "Cancel", "Middle Mouse", "X1 Mouse", "X2 Mouse", "", "Backspace", "Tab", "", "", "Clear", "Enter", "", "",
				"Shift", "Control", "Alt", "Pause", "Caps Lock", "", "", "", "", "", "", "Escape", "", "", "", "",
				"Space", "Page Up", "Page Down", "End", "Home", "Left Arrow", "Up Arrow", "Right Arrow", "Down Arrow", "Select", "", "", "Print Screen", "Insert", "Delete", "Help",
				"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "", "", "", "", "", "",
				"", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
				"P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "Left Windows", "Right Windows", "Apps", "", "Sleep",
				"Numpad 0", "Numpad 1", "Numpad 2", "Numpad 3", "Numpad 4", "Numpad 5", "Numpad 6", "Numpad 7", "Numpad 8", "Numpad 9", "Numpad *", "Numpad +", "", "Numpad -", "Numpad Decimal", "Numpad /",
				"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15", "F16",
				"F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "", "", "", "", "", "", "", "",
				"Num Lock", "Scroll Lock", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
				"Left Shift", "Right Shift", "Left Control", "Right Control", "Left Menu", "Right Menu", "Browser Back", "Browser Forward", "Browser Refresh", "Browser Stop", "Browser Search", "Browser Favorites", "Browser Home", "Volume Mute", "Volume Down", "Volume Up",
				"Next Track", "Previous Track", "Media Stop", "Media Play/Pause", "Mail", "Media Select", "Launch App 1", "Launch App 2", "", "", "OEM ;", "OEM +", "OEM ,", "OEM -", "OEM .", "OEM /",
				"OEM ~", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
				"", "", "", "", "", "", "", "", "", "", "", "OEM [", "OEM \\", "OEM ]", "OEM '", "OEM 8",
				"", "", "OEM <", "", "", "", "", "", "", "", "", "", "", "", "", "",
				"", "", "", "", "", "", "Attn", "CrSel", "ExSel", "Erase EOF", "Play", "Zoom", "", "PA1", "OEM Clear", ""
			};

			return keyboard_keys_international[key];
		}

		std::string KeyIdToString(const std::vector<InputCombo>& combo)
		{
			if (combo.empty())
				return "None";

			bool hasVRInput = false;
			for (const auto& input : combo) {
				if (input.GetDevice() != InputDeviceType::Keyboard && input.GetDevice() != InputDeviceType::Mouse) {
					hasVRInput = true;
					break;
				}
			}

			if (hasVRInput && globals::game::isVR) {
				return InputCombo::GetVRString(combo);
			}

			std::string result;
			for (size_t i = 0; i < combo.size(); ++i) {
				if (i > 0)
					result += " + ";
				result += KeyIdToString(combo[i].GetKey());
			}
			return result;
		}
	}  // namespace Input

	namespace
	{
		/** @brief Runs a button draw with the flash look while its timer (keyed by key) is live, and starts the timer on a click. */
		bool DrawWithFlash(const std::string& key, int flashDurationMs, const std::function<bool()>& draw)
		{
			static std::unordered_map<std::string, std::chrono::steady_clock::time_point> flashTimers;
			static std::mutex flashTimersMutex;

			auto now = std::chrono::steady_clock::now();

			// Check if this button has active flash (thread-safe)
			bool hasActiveFlash = false;
			{
				std::lock_guard<std::mutex> lock(flashTimersMutex);
				auto it = flashTimers.find(key);
				if (it != flashTimers.end()) {
					auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
					if (elapsed.count() < flashDurationMs) {
						hasActiveFlash = true;
					} else {
						// Flash expired, remove it
						flashTimers.erase(it);
					}
				}
			}

			// Style the button with flash effect if active.
			bool styleChanged = false;
			if (hasActiveFlash) {
				// Use subtle white overlay similar to action icon hover effect
				ImVec4 normalButton = ImGui::GetStyleColorVec4(ImGuiCol_Button);
				ImVec4 flashColor = ImVec4(
					normalButton.x + C::FLASH_BRIGHTEN,
					normalButton.y + C::FLASH_BRIGHTEN,
					normalButton.z + C::FLASH_BRIGHTEN,
					normalButton.w);
				ImVec4 flashHovered = ImVec4(flashColor.x * C::FLASH_HOVER_SCALE, flashColor.y * C::FLASH_HOVER_SCALE, flashColor.z * C::FLASH_HOVER_SCALE, flashColor.w);
				ImVec4 flashActive = ImVec4(flashColor.x * C::FLASH_ACTIVE_SCALE, flashColor.y * C::FLASH_ACTIVE_SCALE, flashColor.z * C::FLASH_ACTIVE_SCALE, flashColor.w);

				ImGui::PushStyleColor(ImGuiCol_Button, flashColor);
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, flashHovered);
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, flashActive);
				styleChanged = true;
			}

			bool clicked = draw();

			if (styleChanged) {
				ImGui::PopStyleColor(3);
			}

			// If clicked, start the flash timer (thread-safe)
			if (clicked) {
				std::lock_guard<std::mutex> lock(flashTimersMutex);
				flashTimers[key] = now;
			}

			return clicked;
		}
	}

	bool ButtonWithFlash(const char* label, const ImVec2& size, int flashDurationMs)
	{
		return DrawWithFlash(label, flashDurationMs, [&] { return Util::Button(label, size); });
	}

	bool ButtonWithFlash(const char* id, const char* tablerIcon, const char* label, int flashDurationMs)
	{
		return DrawWithFlash(id, flashDurationMs, [&] { return Icons::LabeledButton(id, Icons::TI(tablerIcon), label); });
	}

	bool LoadDDSTextureFromFile(ID3D11Device* device,
		const char* filename,
		ID3D11ShaderResourceView** out_srv,
		ImVec2& out_size,
		const char* resourceName)
	{
		if (!device || !out_srv) {
			logger::warn("LoadDDSTextureFromFile: Invalid parameters");
			return false;
		}

		*out_srv = nullptr;

		DirectX::ScratchImage image;
		try {
			DX::ThrowIfFailed(LoadDDSImageFromResource(filename, image));
		} catch (const DX::com_exception& e) {
			logger::warn("LoadDDSTextureFromFile: Failed to load DDS data from {}: {}", filename, e.what());
			return false;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(DirectX::CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::warn("LoadDDSTextureFromFile: Failed to create texture: {}", e.what());
			return false;
		}

		ID3D11Texture2D* pTexture = reinterpret_cast<ID3D11Texture2D*>(pResource);
		D3D11_TEXTURE2D_DESC desc;
		pTexture->GetDesc(&desc);
		if (resourceName)
			SetResourceName(pTexture, "%s", resourceName);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = desc.MipLevels }
		};

		HRESULT hr = device->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
		if (SUCCEEDED(hr) && *out_srv) {
			if (resourceName)
				SetResourceName(*out_srv, "%s SRV", resourceName);
			else
				Util::SetResourceName(*out_srv, "UI::DDS:%s", filename);
		}
		pTexture->Release();

		if (FAILED(hr) || !*out_srv) {
			logger::warn("LoadDDSTextureFromFile: Failed to create SRV, HRESULT: 0x{:08X}", static_cast<uint32_t>(hr));
			return false;
		}

		out_size = ImVec2((float)desc.Width, (float)desc.Height);
		logger::debug("LoadDDSTextureFromFile: Successfully loaded {} ({}x{})", filename, desc.Width, desc.Height);
		return true;
	}

	bool FeatureToggle(const char* label, bool* enabled, const ImVec2& size)
	{
		if (!enabled)
			return false;
		using Constants = ThemeManager::Constants;
		ImVec2 toggleSize = size;
		if (toggleSize.x <= 0)
			toggleSize.x = ImGui::GetFrameHeight() * C::FEATURE_TOGGLE_WIDTH_RATIO;
		if (toggleSize.y <= 0)
			toggleSize.y = ImGui::GetFrameHeight() * C::FEATURE_TOGGLE_HEIGHT_RATIO;

		ImGui::PushID(label);
		const ImGuiID id = ImGui::GetID("toggle");
		const bool clicked = ImGui::InvisibleButton("##toggle", toggleSize);
		const bool hovered = ImGui::IsItemHovered();
		if (clicked)
			*enabled = !*enabled;
		ImGui::PopID();

		// Material style: a thin pill track with a larger round knob that overhangs both its ends. The knob slides
		// (eased, and snapped while UI animations are off), and its colours follow the theme's accent.
		const float on = Anim::Approach(id, *enabled ? 1.0f : 0.0f, Constants::ANIM_TOGGLE_RATE, *enabled ? 1.0f : 0.0f);
		const float hover = Anim::Approach(id + 1, hovered ? 1.0f : 0.0f, Constants::ANIM_HOVER_RATE, 0.0f);

		const ImVec2 min = ImGui::GetItemRectMin();
		const float knobRadius = toggleSize.y * 0.5f;
		const float trackHeight = toggleSize.y * Constants::TOGGLE_TRACK_HEIGHT_RATIO;
		const float trackOverhang = knobRadius * Constants::TOGGLE_TRACK_INSET_RATIO;
		const float centerY = min.y + toggleSize.y * 0.5f;
		const ImVec2 trackMin(min.x + trackOverhang, centerY - trackHeight * 0.5f);
		const ImVec2 trackMax(min.x + toggleSize.x - trackOverhang, centerY + trackHeight * 0.5f);
		const float knobX = min.x + knobRadius + (toggleSize.x - knobRadius * 2.0f) * on;

		const ImVec4 accent = Colors::GetAccent();
		const ImVec4 trackOff = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
		const ImVec4 trackOn = Anim::ScaleAlpha(accent, Constants::TOGGLE_TRACK_ON_ALPHA);
		const ImVec4 knobOff = Anim::Lerp(Colors::GetSecondary(), ImGui::GetStyleColorVec4(ImGuiCol_Text), hover);
		const ImVec4 knobOn = Anim::Lerp(accent, ImVec4(1.0f, 1.0f, 1.0f, accent.w), hover * Constants::TOGGLE_KNOB_HOVER_LIFT);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(trackMin, trackMax, ImGui::GetColorU32(Anim::Lerp(trackOff, trackOn, on)), trackHeight * 0.5f);
		// A soft shadow lifts the knob off the track, as in the Material design
		drawList->AddCircleFilled(ImVec2(knobX, centerY + Constants::TOGGLE_SHADOW_OFFSET_RATIO * knobRadius), knobRadius,
			ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, Constants::TOGGLE_SHADOW_ALPHA)));
		drawList->AddCircleFilled(ImVec2(knobX, centerY), knobRadius, ImGui::GetColorU32(Anim::Lerp(knobOff, knobOn, on)));

		return clicked;
	}

	bool InputComboWidget(
		const char* label,
		std::vector<InputCombo>& combo,
		bool& isRecording,
		const char* recordingLabel)
	{
		bool changed = false;
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%s", label);
		ImGui::SameLine();

		// Use theme colors for consistent styling
		auto& theme = globals::menu->GetTheme().StatusPalette;

		if (isRecording) {
			// Recording state visual
			ImGui::PushStyleColor(ImGuiCol_Button, theme.CurrentHotkey);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.CurrentHotkey);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.CurrentHotkey);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));  // Black text on recording color

			// Show current pending combo if available, otherwise prompt
			std::string buttonText;
			if (!combo.empty()) {
				buttonText = Util::Input::KeyIdToString(combo) + "...";  // Indicate it's still capturing
			} else {
				buttonText = T("ui.input.recording", "Recording... (Esc to cancel)");
			}

			if (Util::Button(buttonText.c_str(), ImVec2(0, 0))) {
				isRecording = false;
			}

			ImGui::PopStyleColor(4);

			// Add tooltip explaining how to record
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", T("ui.input.record_hint", "Press any key combination.\nModifiers (Ctrl, Shift, Alt) are supported.\nPress Escape to cancel."));
			}
		} else {
			// Display current binding with unique button ID
			std::string keyString = Util::Input::KeyIdToString(combo);
			std::string btnLabel = keyString + "##" + recordingLabel;
			if (Util::Button(btnLabel.c_str(), ImVec2(0, 0))) {
				isRecording = true;
			}

			// Context menu for clearing
			if (ImGui::BeginPopupContextItem()) {
				if (RoundedSelectable(T("ui.input.clear_binding", "Clear Binding"), false)) {
					combo.clear();
					changed = true;
				}
				ImGui::EndPopup();
			}

			// First run / empty state hint
			if (combo.empty()) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s", T("ui.input.click_to_bind", "(Click to bind)"));
			}
		}

		// Draw VR-specific color coding if applicable
		if (globals::game::isVR && !combo.empty()) {
			ImGui::SameLine();

			// Check if we have mixed devices
			bool hasPrimary = false;
			bool hasSecondary = false;
			bool hasBoth = false;

			for (const auto& input : combo) {
				switch (input.GetDevice()) {
				case InputDeviceType::Primary:
					hasPrimary = true;
					break;
				case InputDeviceType::Secondary:
					hasSecondary = true;
					break;
				case InputDeviceType::Both:
					hasBoth = true;
					break;
				default:
					break;
				}
			}

			ImVec4 indicatorColor = GetControllerDefaultColor();
			const char* indicatorText = "";

			if (hasBoth || (hasPrimary && hasSecondary)) {
				indicatorColor = GetControllerBothColor();
				indicatorText = hasBoth ? T("ui.input.indicator_both", "(Both)") : T("ui.input.indicator_mixed", "(Mixed)");
			} else if (hasPrimary) {
				indicatorColor = GetControllerPrimaryColor();
				indicatorText = T("ui.input.indicator_primary", "(Primary)");
			} else if (hasSecondary) {
				indicatorColor = GetControllerSecondaryColor();
				indicatorText = T("ui.input.indicator_secondary", "(Secondary)");
			}

			if (indicatorText[0] != '\0') {
				ImGui::TextColored(indicatorColor, "%s", indicatorText);
			}
		}

		return changed;
	}

	namespace ConstrainedUI
	{
		namespace
		{
			// Helper to render constraint tooltip
			void RenderConstraintTooltip(const FeatureConstraints::ConstraintResult& constraint)
			{
				if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					return;

				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
				Util::Text::Warning("%s", T("ui.constraint.setting_constrained", "Setting Constrained"));
				ImGui::Text("%s", T("ui.constraint.constrained_by", "This setting is constrained by:"));
				ImGui::Spacing();
				for (const auto& src : constraint.sources) {
					ImGui::BulletText("%s", src.featureName.c_str());
					ImGui::Indent();
					ImGui::TextWrapped("%s", src.reason.c_str());
					if (src.recommendDisableAtBoot) {
						Util::Text::WrappedError("%s", T("ui.constraint.consider_disable", "Consider disabling this feature at boot for best compatibility."));
					}
					ImGui::Unindent();
				}
				ImGui::Separator();
				ImGui::Text(T("ui.constraint.forced_value", "Forced value: %s"), FeatureConstraints::FormatConstraintValue(constraint.forcedValue).c_str());
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
		}

		bool Checkbox(const char* label, bool* value, const FeatureConstraints::SettingId& settingId)
		{
			auto constraint = FeatureConstraints::GetConstraints(settingId);

			if (constraint.isConstrained) {
				// Display the forced value instead of the stored value
				if (auto* forcedBool = std::get_if<bool>(&constraint.forcedValue)) {
					bool displayValue = *forcedBool;
					ImGui::BeginDisabled();
					ImGui::Checkbox(label, &displayValue);
					ImGui::EndDisabled();
				} else {
					// Fallback: wrong type, show disabled with stored value
					ImGui::BeginDisabled();
					ImGui::Checkbox(label, value);
					ImGui::EndDisabled();
				}
				RenderConstraintTooltip(constraint);
				return false;
			}

			return ImGui::Checkbox(label, value);
		}

		bool SliderFloat(const char* label, float* value, float min, float max,
			const FeatureConstraints::SettingId& settingId, const char* format)
		{
			auto constraint = FeatureConstraints::GetConstraints(settingId);

			if (constraint.isConstrained) {
				// Display the forced value instead of the stored value
				if (auto* forcedFloat = std::get_if<float>(&constraint.forcedValue)) {
					float displayValue = *forcedFloat;
					ImGui::BeginDisabled();
					ImGui::SliderFloat(label, &displayValue, min, max, format);
					ImGui::EndDisabled();
				} else {
					// Fallback: wrong type, show disabled with stored value
					ImGui::BeginDisabled();
					ImGui::SliderFloat(label, value, min, max, format);
					ImGui::EndDisabled();
				}
				RenderConstraintTooltip(constraint);
				return false;
			}

			return ImGui::SliderFloat(label, value, min, max, format);
		}

		bool SliderInt(const char* label, int* value, int min, int max,
			const FeatureConstraints::SettingId& settingId, const char* format)
		{
			auto constraint = FeatureConstraints::GetConstraints(settingId);

			if (constraint.isConstrained) {
				// Display the forced value instead of the stored value
				if (auto* forcedInt = std::get_if<int>(&constraint.forcedValue)) {
					int displayValue = *forcedInt;
					ImGui::BeginDisabled();
					ImGui::SliderInt(label, &displayValue, min, max, format);
					ImGui::EndDisabled();
				} else {
					// Fallback: wrong type, show disabled with stored value
					ImGui::BeginDisabled();
					ImGui::SliderInt(label, value, min, max, format);
					ImGui::EndDisabled();
				}
				RenderConstraintTooltip(constraint);
				return false;
			}

			return ImGui::SliderInt(label, value, min, max, format);
		}
	}

	namespace
	{
		constexpr float kFlyoutCloseDelay = 0.06f;
		constexpr float kFlyoutSlideOpenSpeed = 1.0f / ThemeManager::Constants::FEATURE_ACTION_HINT_FADE_SECONDS;
		constexpr float kFlyoutSlideCloseSpeed = kFlyoutSlideOpenSpeed;
		constexpr float kFlyoutSlideDistance = 6.0f;
		constexpr std::string_view kFlyoutWindowPrefix = "##flyout_";
		constexpr std::string_view kBlurredFlyoutWindowPrefix = "##blurred_flyout_";

		void ResetFlyout(FlyoutState& state, bool preserveHoverBlock = false) noexcept
		{
			const ImGuiID blockedHoverId = preserveHoverBlock ? state.blockedHoverId : 0;
			state.isOpen = false;
			state.closing = false;
			state.flyoutHovered = false;
			state.activeId = 0;
			state.blockedHoverId = blockedHoverId;
			state.closeTimer = 0.0f;
			state.openProgress = 0.0f;
			state.keepOpenForNavigation = false;
			state.draggedFromFlyout = false;
		}

		bool ContainsPoint(const ImVec2& min, const ImVec2& max, const ImVec2& point)
		{
			return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
		}

		ImRect GetFlyoutWindowRect()
		{
			auto* viewport = ImGui::GetMainViewport();
			auto* rootWindow = ImGui::GetCurrentWindow()->RootWindow;
			return ImRect(
				ImVec2(std::max(viewport->Pos.x, rootWindow->Pos.x), std::max(viewport->Pos.y, rootWindow->Pos.y)),
				ImVec2(std::min(viewport->Pos.x + viewport->Size.x, rootWindow->Pos.x + rootWindow->Size.x),
					std::min(viewport->Pos.y + viewport->Size.y, rootWindow->Pos.y + rootWindow->Size.y)));
		}

		bool IsFlyoutSourceHovered(const ImVec2& min, const ImVec2& max)
		{
			constexpr auto popupFlags = ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel;
			return ContainsPoint(min, max, ImGui::GetIO().MousePos) &&
			       !ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
			       !ImGui::IsPopupOpen(nullptr, popupFlags) &&
			       ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		}

		ImVec2 GetFlyoutPos(FlyoutState& state, const ImVec2& sourceMin, const ImVec2& sourceMax,
			float slideOffset, float scale, const ImRect& visible, const FlyoutStyle& style, bool& canOpen)
		{
			const float gap = style.verticalGap * scale;
			const float horizontalPivot = style.centerOnSource ? 0.5f : 1.0f;
			ImVec2 pos(
				style.centerOnSource ? (sourceMin.x + sourceMax.x) * 0.5f : sourceMax.x,
				sourceMax.y + gap - slideOffset);

			canOpen = state.lastSize.y <= 0.0f || sourceMax.y + gap + state.lastSize.y <= visible.Max.y;
			if (state.lastSize.x > 0.0f) {
				const float minimumAnchor = visible.Min.x + state.lastSize.x * horizontalPivot;
				const float maximumAnchor = visible.Max.x - state.lastSize.x * (1.0f - horizontalPivot);
				pos.x = minimumAnchor <= maximumAnchor ?
				            std::clamp(pos.x, minimumAnchor, maximumAnchor) :
				            (visible.Min.x + visible.Max.x) * 0.5f;
			}

			pos.x = std::floor(pos.x + 0.5f);
			pos.y = std::floor(pos.y + 0.5f);
			return pos;
		}

		bool BeginFlyoutImpl(FlyoutState& state, ImGuiID itemId, bool sourcePressed,
			const FlyoutStyle& flyoutStyle, const ImVec2* explicitSourceMin = nullptr,
			const ImVec2* explicitSourceMax = nullptr)
		{
			const ImGuiID submittedItemId = ImGui::GetItemID();
			const ImVec2 sourceMin = explicitSourceMin ? *explicitSourceMin : ImGui::GetItemRectMin();
			const ImVec2 sourceMax = explicitSourceMax ? *explicitSourceMax : ImGui::GetItemRectMax();

			const int currentFrame = ImGui::GetFrameCount();
			if (state.lastFrame >= 0 && currentFrame > state.lastFrame + 1)
				ResetFlyout(state);
			state.lastFrame = currentFrame;

			if (state.pendingFocusReturnId == itemId) {
				if (submittedItemId == itemId) {
					ImGui::FocusItem();
					ImGui::SetNavCursorVisible(true);
				}
				state.pendingFocusReturnId = 0;
			}

			const float deltaTime = ImGui::GetIO().DeltaTime;
			const bool hovered = IsFlyoutSourceHovered(sourceMin, sourceMax);
			const bool navigationPressed = sourcePressed && GImGui->NavActivatePressedId == itemId;
			const bool active = state.isOpen && state.activeId == itemId;
			bool focusOnOpen = false;

			if (state.blockedHoverId == itemId && !hovered)
				state.blockedHoverId = 0;

			if (sourcePressed && active) {
				if (state.closing) {
					state.closing = false;
					state.blockedHoverId = 0;
					state.closeTimer = 0.0f;
					state.keepOpenForNavigation = navigationPressed;
					focusOnOpen = navigationPressed;
				} else if (!flyoutStyle.keepOpenOnSourcePress) {
					RequestCloseFlyout(state);
				} else {
					state.closeTimer = 0.0f;
				}
			} else {
				const bool wantsOpen = (hovered && state.blockedHoverId != itemId) || sourcePressed;
				if (wantsOpen && (!state.isOpen || (state.activeId != itemId && !state.flyoutHovered))) {
					const bool freshOpen = !state.isOpen || state.activeId != itemId;
					state.activeId = itemId;
					state.isOpen = true;
					state.closing = false;
					state.closeTimer = 0.0f;
					if (freshOpen) {
						state.openProgress = 0.0f;
						state.keepOpenForNavigation = navigationPressed;
						state.windowName.clear();
						std::format_to(std::back_inserter(state.windowName), "{}{}",
							flyoutStyle.blurBackground ? kBlurredFlyoutWindowPrefix : kFlyoutWindowPrefix, itemId);
					}
				}

				if (wantsOpen && state.closing && state.activeId == itemId) {
					state.closing = false;
					state.closeTimer = 0.0f;
				}
				focusOnOpen = navigationPressed;
			}

			if (!state.isOpen || state.activeId != itemId)
				return false;

			if (!state.closing)
				state.openProgress = std::min(state.openProgress + kFlyoutSlideOpenSpeed * deltaTime, 1.0f);

			state.sourceMin = sourceMin;
			state.sourceMax = sourceMax;

			const float scale = GetUIScale();
			const float easedProgress = GetFlyoutEasedProgress(state);
			const float slideOffset = (1.0f - easedProgress) * kFlyoutSlideDistance * scale;
			const float alpha = easedProgress;
			bool canOpen = true;
			const ImVec2 flyoutPos = GetFlyoutPos(
				state, sourceMin, sourceMax, slideOffset, scale, GetFlyoutWindowRect(), flyoutStyle, canOpen);
			if (!canOpen) {
				state.blockedHoverId = itemId;
				ResetFlyout(state, true);
				return false;
			}

			ImGui::SetNextWindowPos(
				flyoutPos, ImGuiCond_Always, ImVec2(flyoutStyle.centerOnSource ? 0.5f : 1.0f, 0.0f));
			ImGui::SetNextWindowBgAlpha(flyoutStyle.windowBackgroundAlpha);
			if (focusOnOpen)
				ImGui::SetNextWindowFocus();

			constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
			                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
			                                   ImGuiWindowFlags_NoFocusOnAppearing;

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, flyoutStyle.windowPadding);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, flyoutStyle.windowRounding);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, flyoutStyle.contentAlpha * alpha);
			SKSE::stl::scope_exit restoreStyle([]() noexcept { ImGui::PopStyleVar(3); });

			const bool visible = ImGui::Begin(state.windowName.c_str(), nullptr, flags);
			state.lastSize = ImGui::GetWindowSize();
			state.flyoutMin = ImGui::GetWindowPos();
			state.flyoutMax = ImVec2(state.flyoutMin.x + state.lastSize.x, state.flyoutMin.y + state.lastSize.y);
			constexpr auto popupFlags = ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel;
			if (visible && !ImGui::IsPopupOpen(nullptr, popupFlags))
				ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

			if (!visible) {
				ImGui::End();
				return false;
			}

			restoreStyle.release();
			return true;
		}

		void EndFlyoutImpl(FlyoutState& state)
		{
			const bool flyoutHovered = ImGui::IsWindowHovered(
				ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_RootAndChildWindows);
			const bool flyoutFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			const bool dismissRequested = flyoutFocused && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
			const bool navigationActionClosed =
				state.closing && flyoutFocused && GImGui->NavActivatePressedId != 0;
			if (dismissRequested)
				RequestCloseFlyout(state);
			if ((dismissRequested || navigationActionClosed) && state.activeId != 0)
				state.pendingFocusReturnId = state.activeId;

			state.flyoutHovered = flyoutHovered;
			if (flyoutHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				state.keepOpenForNavigation = false;
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && flyoutHovered)
				state.draggedFromFlyout = true;
			else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
				state.draggedFromFlyout = false;

			ImGui::End();
			ImGui::PopStyleVar(3);

			const ImVec2 mousePos = ImGui::GetIO().MousePos;
			const bool overSource = ContainsPoint(state.sourceMin, state.sourceMax, mousePos);
			if (!dismissRequested && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !flyoutHovered && !overSource) {
				ResetFlyout(state);
				return;
			}

			const float deltaTime = ImGui::GetIO().DeltaTime;
			const bool mouseDragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left);
			const ImVec2 bridgeMin(
				std::min(state.sourceMin.x, state.flyoutMin.x), std::min(state.sourceMax.y, state.flyoutMin.y));
			const ImVec2 bridgeMax(
				std::max(state.sourceMax.x, state.flyoutMax.x), std::max(state.sourceMax.y, state.flyoutMin.y));
			const bool sourceHovered = overSource || ContainsPoint(bridgeMin, bridgeMax, mousePos);

			if (state.closing) {
				state.openProgress = std::max(state.openProgress - kFlyoutSlideCloseSpeed * deltaTime, 0.0f);
				if (state.openProgress <= 0.0f)
					ResetFlyout(state, true);
			} else if (flyoutHovered || (flyoutFocused && state.keepOpenForNavigation) || sourceHovered || state.draggedFromFlyout || mouseDragging) {
				state.closeTimer = 0.0f;
			} else {
				state.closeTimer += deltaTime;
				if (state.closeTimer >= kFlyoutCloseDelay)
					state.closing = true;
			}
		}
	}

	FlyoutScope::FlyoutScope(
		FlyoutState& flyoutState, ImGuiID itemId, bool sourcePressed, const FlyoutStyle& flyoutStyle)
	{
		if (BeginFlyoutImpl(flyoutState, itemId, sourcePressed, flyoutStyle))
			state = &flyoutState;
	}

	FlyoutScope::FlyoutScope(FlyoutState& flyoutState, ImGuiID itemId, bool sourcePressed,
		const ImVec2& sourceMin, const ImVec2& sourceMax, const FlyoutStyle& flyoutStyle)
	{
		if (BeginFlyoutImpl(flyoutState, itemId, sourcePressed, flyoutStyle, &sourceMin, &sourceMax))
			state = &flyoutState;
	}

	FlyoutScope::~FlyoutScope()
	{
		if (state)
			EndFlyoutImpl(*state);
	}

	bool IsFlyoutWindowName(const char* name) noexcept
	{
		return IsUnblurredFlyoutWindowName(name) ||
		       (name && std::string_view(name).starts_with(kBlurredFlyoutWindowPrefix));
	}

	bool IsUnblurredFlyoutWindowName(const char* name) noexcept
	{
		return name && std::string_view(name).starts_with(kFlyoutWindowPrefix);
	}

	void RequestCloseFlyout(FlyoutState& state) noexcept
	{
		if (!state.isOpen)
			return;

		state.closing = true;
		state.closeTimer = 0.0f;
		state.blockedHoverId = state.activeId;
	}

	void CloseFlyout(FlyoutState& state) noexcept
	{
		ResetFlyout(state);
		state.pendingFocusReturnId = 0;
	}

	float GetFlyoutEasedProgress(const FlyoutState& state) noexcept
	{
		const float progress = std::clamp(state.openProgress, 0.0f, 1.0f);
		return 1.0f - (1.0f - progress) * (1.0f - progress);
	}

	ImU32 GetColorChannelMarker(ColorChannel channel)
	{
		constexpr std::array markers{
			IM_COL32(240, 20, 20, 255),
			IM_COL32(20, 240, 20, 255),
			IM_COL32(20, 20, 240, 255),
		};
		return markers[static_cast<size_t>(channel)];
	}

	void SetNextItemColorMarker(ColorChannel channel)
	{
		ImGui::SetNextItemColorMarker(GetColorChannelMarker(channel));
	}
}  // namespace Util
