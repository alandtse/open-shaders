#include "PCH.h"

#include "CursorLoader.h"
#include "Menu.h"

#include <stb_image.h>

namespace Util::CursorLoader
{
	namespace
	{
		struct LoadedCursor
		{
			ID3D11ShaderResourceView* texture = nullptr;
			ImVec2 size{};
			ImVec2 hotspot{};
			HCURSOR native = nullptr;
			HCURSOR resized = nullptr;
			ImVec2 nativeSize{};

			void Release()
			{
				if (resized)
					DestroyCursor(resized);
				if (native)
					DestroyCursor(native);
				resized = native = nullptr;
				nativeSize = {};
				if (texture) {
					texture->Release();
					texture = nullptr;
				}
				size = {};
				hotspot = {};
			}
		};

		eastl::array<LoadedCursor, ImGuiMouseCursor_COUNT> g_cursors = {};
		HCURSOR ownedCursor = nullptr;
		HCURSOR previousCursor = nullptr;
		int visibilityIncrements = 0;

		bool IsLoading()
		{
			return globals::game::ui && globals::game::ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		HCURSOR LoadNativeCursor(const std::filesystem::path& path, const ImVec2& hotspot)
		{
			int width = 0, height = 0, channels = 0;
			auto* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
			if (!pixels)
				return nullptr;
			const SKSE::stl::scope_exit freePixels([pixels] { stbi_image_free(pixels); });
			BITMAPINFO bitmap{};
			bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bitmap.bmiHeader.biWidth = width;
			bitmap.bmiHeader.biHeight = -height;
			bitmap.bmiHeader.biPlanes = 1;
			bitmap.bmiHeader.biBitCount = 32;
			void* bits = nullptr;
			const auto color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (!color)
				return nullptr;
			const SKSE::stl::scope_exit freeColor([color] { DeleteObject(color); });
			auto* destination = static_cast<std::uint8_t*>(bits);
			constexpr unsigned int opaque = 255;
			for (size_t offset = 0; offset < static_cast<size_t>(width) * height * STBI_rgb_alpha; offset += STBI_rgb_alpha) {
				const unsigned int alpha = pixels[offset + 3];
				destination[offset] = static_cast<std::uint8_t>(pixels[offset + 2] * alpha / opaque);
				destination[offset + 1] = static_cast<std::uint8_t>(pixels[offset + 1] * alpha / opaque);
				destination[offset + 2] = static_cast<std::uint8_t>(pixels[offset] * alpha / opaque);
				destination[offset + 3] = static_cast<std::uint8_t>(alpha);
			}
			const auto mask = CreateBitmap(width, height, 1, 1, nullptr);
			if (!mask)
				return nullptr;
			const SKSE::stl::scope_exit freeMask([mask] { DeleteObject(mask); });
			ICONINFO info{};
			info.xHotspot = static_cast<DWORD>(std::clamp(hotspot.x, 0.0f, static_cast<float>(width - 1)));
			info.yHotspot = static_cast<DWORD>(std::clamp(hotspot.y, 0.0f, static_cast<float>(height - 1)));
			info.hbmColor = color;
			info.hbmMask = mask;
			return static_cast<HCURSOR>(CreateIconIndirect(&info));
		}

		void ForEachSlot(const Menu::ThemeSettings& theme, auto&& fn)
		{
			static constexpr struct
			{
				ImGuiMouseCursor cursor;
				const char* defaultFile;
			} kSlots[] = {
				{ ImGuiMouseCursor_Arrow, "cursor.png" },
				{ ImGuiMouseCursor_TextInput, "cursor_text.png" },
				{ ImGuiMouseCursor_ResizeAll, "cursor_resize_all.png" },
				{ ImGuiMouseCursor_ResizeNS, "cursor_resize_ns.png" },
				{ ImGuiMouseCursor_ResizeEW, "cursor_resize_ew.png" },
				{ ImGuiMouseCursor_ResizeNESW, "cursor_resize_nesw.png" },
				{ ImGuiMouseCursor_ResizeNWSE, "cursor_resize_nwse.png" },
				{ ImGuiMouseCursor_Hand, "cursor_hand.png" },
				{ ImGuiMouseCursor_NotAllowed, "cursor_not_allowed.png" },
			};
			for (const auto& slot : kSlots) {
				fn(slot.cursor, slot.defaultFile, theme.Cursor.Types[static_cast<size_t>(slot.cursor)]);
			}
		}

		std::string EffectiveFile(const Menu::ThemeSettings::CursorImageSettings& settings, const char* defaultFile)
		{
			return !settings.File.empty() ? settings.File : defaultFile;
		}

		std::filesystem::path ResolvePath(const Menu& menu, const std::string& fileName)
		{
			if (fileName.empty()) {
				return {};
			}
			const auto& preset = menu.GetSettings().SelectedThemePreset;
			if (!preset.empty()) {
				auto themePath = Util::PathHelpers::GetThemesPath() / preset / fileName;
				if (std::filesystem::exists(themePath)) {
					return themePath;
				}
			}
			auto sharedPath = Util::PathHelpers::GetCursorsPath() / fileName;
			return std::filesystem::exists(sharedPath) ? sharedPath : std::filesystem::path{};
		}

		bool IsPathAllowed(const std::filesystem::path& path)
		{
			return Util::IsPathWithinDirectory(Util::PathHelpers::GetThemesPath(), path) ||
			       Util::IsPathWithinDirectory(Util::PathHelpers::GetCursorsPath(), path);
		}
	}

	void MigrateLegacyCursorSettings(Menu::ThemeSettings& theme)
	{
		auto& types = theme.Cursor.Types;
		auto& arrow = types[ImGuiMouseCursor_Arrow];
		if (arrow.File.empty() && !theme.Cursor.File.empty()) {
			arrow.File = theme.Cursor.File;
			arrow.HotspotX = theme.Cursor.HotspotX;
			arrow.HotspotY = theme.Cursor.HotspotY;
		}
	}

	int GetLoadedCount()
	{
		int count = 0;
		for (const auto& cursor : g_cursors) {
			if (cursor.texture) {
				++count;
			}
		}
		return count;
	}

	void Shutdown()
	{
		ReleaseNativeCursor();
		for (auto& cursor : g_cursors) {
			cursor.Release();
		}
	}

	bool Reload(Menu* menu)
	{
		if (!menu) {
			return false;
		}

		Shutdown();

		if (!menu->GetSettings().Theme.UseCustomCursor) {
			return true;
		}

		auto* device = globals::d3d::device;
		static bool loggedMissingDevice = false;
		if (!device) {
			if (!loggedMissingDevice) {
				logger::warn("CursorLoader::Reload: D3D device is null; will retry when available");
				loggedMissingDevice = true;
			}
			return false;
		}
		loggedMissingDevice = false;

		MigrateLegacyCursorSettings(menu->GetSettings().Theme);
		const auto& theme = menu->GetSettings().Theme;

		int loadedCount = 0;
		int failedCount = 0;
		ForEachSlot(theme, [&](ImGuiMouseCursor cursor, const char* defaultFile, const Menu::ThemeSettings::CursorImageSettings& settings) {
			const auto fileName = EffectiveFile(settings, defaultFile);
			const auto path = ResolvePath(*menu, fileName);
			if (path.empty() || !IsPathAllowed(path)) {
				return;
			}

			ID3D11ShaderResourceView* srv = nullptr;
			ImVec2 size{};
			if (!Util::LoadTextureFromFile(device, path.string().c_str(), &srv, size)) {
				++failedCount;
				return;
			}

			auto& loaded = g_cursors[static_cast<size_t>(cursor)];
			loaded.texture = srv;
			loaded.size = size;
			loaded.hotspot = ImVec2(settings.HotspotX, settings.HotspotY);
			if (!globals::game::isVR)
				loaded.native = LoadNativeCursor(path, loaded.hotspot);
			++loadedCount;
		});

		if (loadedCount == 0) {
			logger::warn("CursorLoader::Reload: No cursor images found under Themes/<preset>/ or Interface/CommunityShaders/Cursors/");
		} else {
			if (failedCount > 0) {
				logger::warn("CursorLoader::Reload: Loaded {} custom cursor image(s); {} file(s) failed to decode", loadedCount, failedCount);
			} else {
				logger::info("CursorLoader::Reload: Loaded {} custom cursor image(s)", loadedCount);
			}
		}
		return true;
	}

	void ReleaseNativeCursor()
	{
		if (ownedCursor && GetCursor() == ownedCursor)
			SetCursor(previousCursor);
		while (visibilityIncrements > 0) {
			ShowCursor(FALSE);
			--visibilityIncrements;
		}
		ownedCursor = previousCursor = nullptr;
	}

	bool UpdateNativeCursor(const Menu& menu)
	{
		auto& io = ImGui::GetIO();
		const auto& theme = menu.GetSettings().Theme;
		const auto active = ImGui::GetMouseCursor();
		DXGI_SWAP_CHAIN_DESC desc{};
		if (globals::game::isVR || IsLoading() || !theme.UseCustomCursor || !io.MouseDrawCursor ||
			active < ImGuiMouseCursor_Arrow || active >= ImGuiMouseCursor_COUNT ||
			!globals::d3d::swapChain || FAILED(globals::d3d::swapChain->GetDesc(&desc)) ||
			GetForegroundWindow() != desc.OutputWindow) {
			ReleaseNativeCursor();
			return false;
		}
		auto& loaded = g_cursors[static_cast<size_t>(active)];
		RECT client{};
		if (!loaded.native || !GetClientRect(desc.OutputWindow, &client) || client.right <= 0 || client.bottom <= 0 ||
			io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) {
			ReleaseNativeCursor();
			return false;
		}
		const float scale = (theme.Cursor.Scale > 0.0f ? theme.Cursor.Scale : 1.0f) * ImGui::GetStyle().MouseCursorScale;
		const ImVec2 size(std::max(1.0f, std::round(loaded.size.x * scale * client.right / io.DisplaySize.x)),
			std::max(1.0f, std::round(loaded.size.y * scale * client.bottom / io.DisplaySize.y)));
		if (size.x != loaded.nativeSize.x || size.y != loaded.nativeSize.y) {
			const auto resized = static_cast<HCURSOR>(CopyImage(loaded.native, IMAGE_CURSOR, static_cast<int>(size.x), static_cast<int>(size.y), 0));
			if (!resized) {
				ReleaseNativeCursor();
				return false;
			}
			ReleaseNativeCursor();
			if (loaded.resized)
				DestroyCursor(loaded.resized);
			loaded.resized = resized;
			loaded.nativeSize = size;
		}
		if (!ownedCursor)
			previousCursor = GetCursor();
		ownedCursor = loaded.resized;
		SetCursor(ownedCursor);
		CURSORINFO info{ sizeof(CURSORINFO) };
		if (GetCursorInfo(&info) && !(info.flags & CURSOR_SHOWING)) {
			int count;
			do {
				count = ShowCursor(TRUE);
				++visibilityIncrements;
			} while (count < 0);
		}
		info.cbSize = sizeof(info);
		if (!GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING) || info.hCursor != ownedCursor) {
			ReleaseNativeCursor();
			return false;
		}
		io.MouseDrawCursor = false;
		return true;
	}

	nlohmann::json GetDiagnostics()
	{
		CURSORINFO info{ sizeof(CURSORINFO) };
		const bool inspected = GetCursorInfo(&info) != FALSE;
		const bool showing = inspected && (info.flags & CURSOR_SHOWING);
		const auto& io = ImGui::GetIO();
		return {
			{ "mode", ownedCursor ? "native" : io.MouseDrawCursor ? "rendered" :
																	"hidden" },
			{ "nativeVisible", ownedCursor && showing && info.hCursor == ownedCursor },
			{ "systemCursorShowing", showing },
			{ "visibilityIncrements", visibilityIncrements },
			{ "loadingScreen", IsLoading() },
			{ "screenPosition", { info.ptScreenPos.x, info.ptScreenPos.y } },
			{ "uiPosition", { io.MousePos.x, io.MousePos.y } },
			{ "displaySize", { io.DisplaySize.x, io.DisplaySize.y } }
		};
	}

	void DrawCustomCursor(const Menu& menu)
	{
		const auto& theme = menu.GetSettings().Theme;
		if (!theme.UseCustomCursor) {
			return;
		}

		auto& io = ImGui::GetIO();
		if (!io.MouseDrawCursor) {
			return;
		}

		const auto active = ImGui::GetMouseCursor();
		if (active <= ImGuiMouseCursor_None || active >= ImGuiMouseCursor_COUNT) {
			return;
		}

		const auto& loaded = g_cursors[static_cast<size_t>(active)];
		if (!loaded.texture) {
			return;
		}

		ImGui::SetMouseCursor(ImGuiMouseCursor_None);

		const float scale = (theme.Cursor.Scale > 0.0f ? theme.Cursor.Scale : 1.0f) * ImGui::GetStyle().MouseCursorScale;
		const ImVec2 drawSize{ loaded.size.x * scale, loaded.size.y * scale };
		const ImVec2 hotspot{ loaded.hotspot.x * scale, loaded.hotspot.y * scale };
		const ImVec2 pos{ io.MousePos.x - hotspot.x, io.MousePos.y - hotspot.y };

		ImGui::GetForegroundDrawList()->AddImage(
			reinterpret_cast<ImTextureID>(loaded.texture),
			pos,
			{ pos.x + drawSize.x, pos.y + drawSize.y },
			{},
			{ 1.0f, 1.0f },
			IM_COL32_WHITE);
	}
}
