#include "OverlayRenderer.h"
#include "BackgroundBlur.h"
#include "HomePageRenderer.h"
#include "ThemeManager.h"

#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <winrt/base.h>

#include "CSEditor/EditorWindow.h"
#include "Feature.h"
#include "FeatureIssues.h"
#include "Features/RenderDoc.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "ImGuiVRHelperClient.h"
#include "Menu.h"
#include "Menu/CursorLoader.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

#include "Features/PerformanceOverlay.h"
#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"
#include "Features/VR.h"

namespace
{
	std::unordered_map<ImGuiID, float> s_windowOverlapAlpha;

	constexpr ImGuiWindowFlags SKIP_WINDOW_FLAGS = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove;
	// Prefix-match against the display title set by Menu.cpp ("Open Shaders <ver>").
	// Must track that title — if the display name changes the prefix here must
	// change too, or IsMainWindow() will silently start returning false and
	// overlay-alpha logic loses its anchor.
	constexpr const char* MAIN_WINDOW_PREFIX = "Open Shaders";

	bool IsMainWindow(ImGuiWindow* win) { return win->Name && strncmp(win->Name, MAIN_WINDOW_PREFIX, strlen(MAIN_WINDOW_PREFIX)) == 0; }

	void DrawShaderCompilationFailures(uint64_t failed, const Menu::ThemeSettings& themeSettings)
	{
		ImGui::TextColored(themeSettings.StatusPalette.Error,
			T("overlay.shaders_failed", "ERROR: %llu shaders failed to compile. Check installation and CommunityShaders.log"),
			static_cast<unsigned long long>(failed));

		if (FeatureIssues::HasPotentialShaderModifyingFeatures()) {
			ImGui::TextColored(themeSettings.StatusPalette.Error, "%s", T("overlay.modified_features", "Features that may have modified shaders detected. Check Feature Issues in the Menu."));
		}
	}

	bool IsVisibleRootWindow(ImGuiWindow* win)
	{
		if (!win || !win->WasActive || win->Hidden)
			return false;
		return !(win->ParentWindow && !win->DockIsActive) && !(win->Flags & SKIP_WINDOW_FLAGS);
	}

	// Patches DrawList background vertices for windows involved in overlap.
	void PatchOverlappingWindowBackgrounds()
	{
		auto* ctx = ImGui::GetCurrentContext();
		if (!ctx)
			return;

		using C = ThemeManager::Constants;
		const float dt = ImGui::GetIO().DeltaTime;

		struct WinInfo
		{
			ImGuiWindow* win;
			ImRect rect;
		};
		std::vector<WinInfo> windows;
		for (int i = 0; i < ctx->Windows.Size; i++) {
			auto* win = ctx->Windows[i];
			if (IsVisibleRootWindow(win))
				windows.push_back({ win, win->Rect() });
		}

		std::unordered_set<ImGuiID> overlapping;
		for (size_t i = 0; i < windows.size(); i++)
			for (size_t j = i + 1; j < windows.size(); j++)
				if (windows[i].rect.Overlaps(windows[j].rect)) {
					auto* a = windows[i].win;
					auto* b = windows[j].win;
					// Main CS window never dims; other windows yield to it
					if (IsMainWindow(a))
						overlapping.insert(b->ID);
					else if (IsMainWindow(b))
						overlapping.insert(a->ID);
					else
						overlapping.insert(a->FocusOrder > b->FocusOrder ? a->ID : b->ID);
				}

		const ImU32 bgRGB = ImGui::GetColorU32(ImGuiCol_WindowBg) & ~IM_COL32_A_MASK;

		for (auto& [win, rect] : windows) {
			const float target = overlapping.count(win->ID) ? C::OVERLAP_MIN_ALPHA : 0.0f;
			float& alpha = s_windowOverlapAlpha[win->ID];
			const float speed = (target > alpha) ? C::OVERLAP_FADEIN_SPEED : C::OVERLAP_FADEOUT_SPEED;
			alpha += (target - alpha) * (std::min)(1.0f, dt * speed);

			if (alpha < C::OVERLAP_ALPHA_EPSILON) {
				alpha = 0.0f;
				continue;
			}

			auto* dl = win->DrawList;
			if (!dl || dl->VtxBuffer.Size == 0)
				continue;

			// Clamp background rect vertex alpha (contiguous bgRGB block at start of DrawList)
			const ImU32 minA = static_cast<ImU32>(alpha * 255.0f);
			for (int v = 0; v < dl->VtxBuffer.Size; v++) {
				auto& vtx = dl->VtxBuffer[v];
				if ((vtx.col & ~IM_COL32_A_MASK) != bgRGB)
					break;
				ImU32 a = (vtx.col >> IM_COL32_A_SHIFT) & 0xFF;
				if (a > 0 && a < minA)
					vtx.col = bgRGB | (minA << IM_COL32_A_SHIFT);
			}
		}

		// Prune stale entries
		for (auto it = s_windowOverlapAlpha.begin(); it != s_windowOverlapAlpha.end();)
			it->second < C::OVERLAP_ALPHA_EPSILON ? it = s_windowOverlapAlpha.erase(it) : ++it;
	}
}  // namespace

void OverlayRenderer::RenderOverlay(
	Menu& menu,
	const std::function<void()>& processInputEventQueue,
	const std::function<void()>& drawSettings,
	const std::function<const char*(std::vector<InputCombo>)>& keyIdToString,
	float& cachedFontSize,
	float currentFontSize)
{
	processInputEventQueue();

	// VR overlay textures and controller→ImGui input are owned by the helper
	// (helper-required); SCS no longer recreates overlay textures or pumps
	// controller input itself.

	if (ShouldSkipRendering()) {
		auto& io = ImGui::GetIO();
		io.ClearInputKeys();
		io.ClearEventsQueue();
		s_windowOverlapAlpha.clear();
		return;
	}

	HandleFontReload(menu, cachedFontSize, currentFontSize);

	if (ImGuiVRHelperClient::IsRegistered()) {
		// VR + helper present: render two independent ImGui passes into two
		// helper-owned panels. The MENU pass runs FIRST so its NewFrame()
		// consumes the wand/keyboard input that ImGuiVRHelperClient::Update()
		// (called from processInputEventQueue above) queued into io — the HUD
		// pass is non-interactive and must not eat it.
		RenderMenuPass(menu, drawSettings, keyIdToString);
		RenderHudPass(keyIdToString);
		return;
	}

	// Flat (non-VR / helper-absent) path: the original single combined frame
	// to the backbuffer. Unchanged behavior; no RenderToPanel.
	InitializeImGuiFrame(menu);

	RenderShaderCompilationStatus(keyIdToString);
	RenderShaderBlockingStatus();

	auto* editorWindow = EditorWindow::GetSingleton();
	if (editorWindow->open && !EditorWindow::CanBeOpen()) {
		editorWindow->open = false;
		if (editorWindow->IsInPreviewMode())
			editorWindow->ExitPreviewMode();
	}
	editorWindow->UpdateOpenState();
	if (editorWindow->open) {
		bool flying = editorWindow->IsPreviewFlying();
		auto& io = ImGui::GetIO();
		io.MouseDrawCursor = !flying;
		if (flying)
			io.MousePos = { -FLT_MAX, -FLT_MAX };  // prevent hover/tooltips during active flying
		editorWindow->Draw();
	} else if (menu.IsEnabled || HomePageRenderer::ShouldShowFirstTimeSetup() ||
			   ImGuiVRHelperClient::HelperRequestsRender()) {
		ImGui::GetIO().MouseDrawCursor = true;
		// Helper-requested render: the helper's in-scene focus model routed
		// focus here. Draw the settings UI even though Menu::IsEnabled hasn't
		// been flipped by the local TAB hotkey. This honors the
		// kClientFlag_RendersOnFocus contract we advertised at registration.
		if (menu.IsEnabled || ImGuiVRHelperClient::HelperRequestsRender()) {
			drawSettings();
		}
	} else {
		ImGui::GetIO().MouseDrawCursor = false;
	}

	RenderFeatureOverlays();
	RenderFirstTimeSetupOverlay();
	HandleABTesting();
	PatchOverlappingWindowBackgrounds();
	FinalizeImGuiFrame();
}

// ---- Two-pass VR rendering -------------------------------------------------

void OverlayRenderer::RenderMenuPass(
	Menu& menu,
	const std::function<void()>& drawSettings,
	const std::function<const char*(std::vector<InputCombo>)>& /*keyIdToString*/)
{
	BeginMenuPass(menu);

	// Interactive content only — the status overlays move to the HUD pass.
	auto* editorWindow = EditorWindow::GetSingleton();
	if (editorWindow->open && !EditorWindow::CanBeOpen()) {
		editorWindow->open = false;
		if (editorWindow->IsInPreviewMode())
			editorWindow->ExitPreviewMode();
	}
	editorWindow->UpdateOpenState();
	if (editorWindow->open) {
		bool flying = editorWindow->IsPreviewFlying();
		auto& io = ImGui::GetIO();
		io.MouseDrawCursor = !flying;
		if (flying)
			io.MousePos = { -FLT_MAX, -FLT_MAX };  // prevent hover/tooltips during active flying
		editorWindow->Draw();
	} else if (menu.IsEnabled || HomePageRenderer::ShouldShowFirstTimeSetup() ||
			   ImGuiVRHelperClient::HelperRequestsRender()) {
		ImGui::GetIO().MouseDrawCursor = true;
		if (menu.IsEnabled || ImGuiVRHelperClient::HelperRequestsRender()) {
			drawSettings();
		}
	} else {
		ImGui::GetIO().MouseDrawCursor = false;
	}

	RenderFirstTimeSetupOverlay();
	HandleABTesting();
	PatchOverlappingWindowBackgrounds();

	EndMenuPass(ImGuiVRHelperClient::Surface::Menu);
}

void OverlayRenderer::RenderHudPass(
	const std::function<const char*(std::vector<InputCombo>)>& keyIdToString)
{
	BeginHudPass();

	// Always-on, non-interactive status content.
	RenderShaderCompilationStatus(keyIdToString);
	RenderShaderBlockingStatus();
	RenderFeatureOverlays();
	RenderWelcomeOverlay();

	EndHudPass(ImGuiVRHelperClient::Surface::Hud);
}

bool OverlayRenderer::ShouldSkipRendering()
{
	auto shaderCache = globals::shaderCache;
	auto failed = shaderCache->GetCurrentFailedCount();
	auto hide = shaderCache->IsHideErrors();
	auto* abTestingManager = ABTestingManager::GetSingleton();
	auto* renderDoc = RenderDoc::GetSingleton();

	return !(shaderCache->IsCompiling() ||
			 Menu::GetSingleton()->IsEnabled ||
			 EditorWindow::GetSingleton()->open ||
			 abTestingManager->IsEnabled() ||
			 (failed && !hide) ||
			 globals::features::performanceOverlay.settings.ShowInOverlay ||
			 renderDoc->IsAvailable());
}

void OverlayRenderer::HandleFontReload(Menu& menu, float& cachedFontSize, float currentFontSize)
{
	bool fontSizeChanged = std::abs(cachedFontSize - currentFontSize) > ThemeManager::Constants::FONT_CACHE_EPSILON;
	std::string desiredSignature = menu.BuildFontSignature(currentFontSize);
	bool signatureChanged = desiredSignature != menu.cachedFontSignature;

	if (fontSizeChanged || signatureChanged) {
		if (!ThemeManager::ReloadFont(menu, cachedFontSize)) {
			logger::warn("OverlayRenderer::HandleFontReload() - Font reload failed");
		}
	}
}

namespace
{
	// Swapchain size + DeltaTime captured by the menu pass (or the flat
	// InitializeImGuiFrame) for reuse by the lighter HUD pass, which doesn't
	// re-run ImGui_ImplWin32_NewFrame.
	ImVec2 s_lastDisplaySize{ 0.0f, 0.0f };
	float s_lastDeltaTime = 1.0f / 60.0f;
}

void OverlayRenderer::InitializeImGuiFrame(Menu& menu)
{
	BeginMenuPass(menu);
}

void OverlayRenderer::BeginMenuPass(Menu& menu)
{
	// Start the Dear ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();

	DXGI_SWAP_CHAIN_DESC desc{};
	globals::d3d::swapChain->GetDesc(&desc);

	const float displayW = static_cast<float>(desc.BufferDesc.Width);
	const float displayH = static_cast<float>(desc.BufferDesc.Height);
	Util::UpdateImGuiInput(desc.OutputWindow, displayW, displayH);

	ImGui::NewFrame();

	// Capture for the HUD pass (same swapchain size; DeltaTime as advanced by
	// ImGui_ImplWin32_NewFrame this frame).
	s_lastDisplaySize = ImVec2(displayW, displayH);
	s_lastDeltaTime = ImGui::GetIO().DeltaTime;

	// Detect display size change (cross-session via ini handler, mid-session via member)
	const float2 currentDisplaySize{ displayW, displayH };
	if (menu.lastDisplaySize.x > 0.f && menu.lastDisplaySize != currentDisplaySize) {
		logger::info("Display size changed: {}x{} -> {}x{}, resetting window layout",
			menu.lastDisplaySize.x, menu.lastDisplaySize.y, currentDisplaySize.x, currentDisplaySize.y);
		menu.resetLayout = true;
		EditorWindow::GetSingleton()->resetLayout = true;
	}
	menu.lastDisplaySize = currentDisplaySize;

	ThemeManager::SetupImGuiStyle(menu);
}

void OverlayRenderer::BeginHudPass()
{
	// Lighter NewFrame: advance the DX11 backend and start a frame, but do
	// NOT run ImGui_ImplWin32_NewFrame (avoids double-advancing time and
	// re-reading the desktop cursor) and feed NO input. The HUD pass is
	// non-interactive.
	ImGui_ImplDX11_NewFrame();

	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = (s_lastDisplaySize.x > 0.0f) ? s_lastDisplaySize : ImVec2(1920.0f, 1080.0f);
	io.DeltaTime = (s_lastDeltaTime > 0.0f) ? s_lastDeltaTime : (1.0f / 60.0f);

	ImGui::NewFrame();
}

void OverlayRenderer::EndMenuPass(ImGuiVRHelperClient::Surface surface)
{
	// Custom cursor for the interactive surface only.
	if (auto* menu = Menu::GetSingleton();
		menu && menu->GetSettings().Theme.UseCustomCursor && Util::CursorLoader::GetLoadedCount() > 0) {
		Util::CursorLoader::DrawCustomCursor(*menu);
	}

	ImGui::Render();

	// Background blur belongs to the menu surface only.
	BackgroundBlur::RenderBackgroundBlur();

	// Desktop mirror.
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	// And into the helper's panel for this surface.
	ImGuiVRHelperClient::RenderToPanel(surface);
}

void OverlayRenderer::EndHudPass(ImGuiVRHelperClient::Surface surface)
{
	// No custom cursor, no background blur for the non-interactive HUD.
	ImGui::Render();

	// Desktop mirror.
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	// And into the helper's HUD panel.
	ImGuiVRHelperClient::RenderToPanel(surface);
}

void OverlayRenderer::RenderShaderCompilationStatus(const std::function<const char*(std::vector<InputCombo>)>& keyIdToString)
{
	auto shaderCache = globals::shaderCache;
	auto failed = shaderCache->GetCurrentFailedCount();
	auto hide = shaderCache->IsHideErrors();

	const float scale = Util::GetUIScale();
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;

	uint64_t totalShaders = shaderCache->GetTotalTasks();
	uint64_t compiledShaders = shaderCache->GetCompletedTasks();

	auto state = globals::state;
	auto& themeSettings = Menu::GetSingleton()->GetTheme();
	auto* renderDoc = RenderDoc::GetSingleton();
	bool renderDocAvailable = renderDoc->IsAvailable();
	const auto renderDocInformation = renderDoc->GetOverlayWarningMessage();

	std::string compilePrefix = shaderCache->backgroundCompilation ? T("overlay.background_prefix", "Background ") : "";
	std::string shaderStats = shaderCache->GetShaderStatsString(!state->IsDeveloperMode());
	auto progressTitle = std::vformat(T("overlay.compiling_shaders", "{}Compiling Shaders: {}"),
		std::make_format_args(compilePrefix, shaderStats));
	auto percent = (float)compiledShaders / (float)totalShaders;
	auto progressOverlay = fmt::format("{}/{} ({:2.1f}%)", compiledShaders, totalShaders, 100 * percent);

	if (shaderCache->IsCompiling()) {
		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}
		ImGui::TextUnformatted(progressTitle.c_str());
		ImGui::ProgressBar(percent, ImVec2(0.0f, 0.0f), progressOverlay.c_str());
		if (shaderCache->IsDiskCacheHeld()) {
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
				T("overlay.cache_held",
					"Feature set changed: your previous shader cache is preserved on disk.\n"
					"Open the Open Shaders menu home page to rebuild it or fix your setup and restart."));
		}
		// Surface bad-install feature problems during the compile itself; otherwise
		// the user only learns from the Feature Issues tab after baking the wrong set.
		if (FeatureIssues::HasFeatureIssues()) {
			const size_t issueCount = FeatureIssues::GetFeatureIssues().size();
			ImGui::TextColored(themeSettings.StatusPalette.Error,
				T("overlay.feature_issues_compiling",
					"WARNING: %zu feature(s) failed to load (bad install or version mismatch).\n"
					"Quit, fix them in the menu's Feature Issues tab, and restart - compiling now\n"
					"bakes the wrong shaders and you will have to recompile after fixing."),
				issueCount);
		}
		if (state->IsDeveloperMode()) {
			int32_t threadLimit = shaderCache->backgroundCompilation ? shaderCache->backgroundCompilationThreadCount : shaderCache->compilationThreadCount;
			int compilationRunning = (int)shaderCache->compilationPool.get_tasks_running();
			int heavyInFlight = shaderCache->GetHeavyTasksInFlight();
			int heavyLimit = static_cast<int>(Util::GetPerformanceCoreCount());
			uint64_t slow = shaderCache->GetSlowTasks();
			uint64_t verySlow = shaderCache->GetVerySlowTasks();
			ImGui::Text(T("overlay.threads_status", "Threads: %d / %d limit | Heavy: %d / %d P-cores | %d workers"),
				compilationRunning,
				threadLimit,
				heavyInFlight,
				heavyLimit,
				(int)shaderCache->compilationPool.get_thread_count());
			if (slow > 0) {
				ImGui::Text(T("overlay.slow_shaders", "Slow shaders: %llu (very slow: %llu)"), slow, verySlow);
			}
		}
		if (!shaderCache->backgroundCompilation && shaderCache->menuLoaded) {
			const char* skipKeyName = keyIdToString(Menu::GetSingleton()->GetSettings().SkipCompilationKey);
			auto skipShadersText = std::vformat(
				T("overlay.skip_compilation", "Press {} to proceed without completing shader compilation. "),
				std::make_format_args(skipKeyName));
			ImGui::TextUnformatted(skipShadersText.c_str());
			ImGui::TextUnformatted(T("overlay.uncompiled_warning", "WARNING: Uncompiled shaders will have visual errors or cause stuttering when loading."));
		}
		if (failed && !hide) {
			DrawShaderCompilationFailures(failed, themeSettings);
		}

		if (renderDocAvailable)
			ImGui::TextColored(themeSettings.StatusPalette.Warning, renderDocInformation.c_str());

		ImGui::End();
		return;
	}

	if (failed) {
		if (!hide) {
			ImGui::SetNextWindowPos(ImVec2(pos, pos));
			if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
				ImGui::End();
				return;
			}

			DrawShaderCompilationFailures(failed, themeSettings);

			if (renderDocAvailable)
				ImGui::TextColored(themeSettings.StatusPalette.Warning, renderDocInformation.c_str());

			ImGui::End();
		}
	} else if (renderDocAvailable) {
		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}
		ImGui::TextColored(themeSettings.StatusPalette.Warning, renderDocInformation.c_str());
		ImGui::End();
	}
}

void OverlayRenderer::RenderFeatureOverlays()
{
	// load overlays
	for (Feature* feat : Feature::GetFeatureList()) {
		if (feat && feat->loaded) {
			if (auto* overlay = dynamic_cast<OverlayFeature*>(feat)) {
				overlay->DrawOverlay();
			}
		}
	}
}

void OverlayRenderer::RenderWelcomeOverlay()
{
	// Onboarding banner shown in the HUD pass so it appears at launch / main
	// menu without the settings menu being open. Auto-hides after
	// kAutoHideSeconds, and stops for the session once the menu has been
	// opened (locally or via the helper's focus) at least once.
	const int autoHideSeconds = globals::features::vr.settings.kAutoHideSeconds;
	if (autoHideSeconds <= 0) {
		return;  // disabled
	}

	static bool s_menuEverOpened = false;
	if (!s_menuEverOpened &&
		(Menu::GetSingleton()->IsEnabled || ImGuiVRHelperClient::HelperRequestsRender())) {
		s_menuEverOpened = true;
	}
	if (s_menuEverOpened) {
		return;
	}

	// Steady countdown via QueryPerformanceCounter (started on first draw).
	static LARGE_INTEGER s_freq{};
	static LARGE_INTEGER s_start{};
	static bool s_started = false;
	if (!s_started) {
		QueryPerformanceFrequency(&s_freq);
		QueryPerformanceCounter(&s_start);
		s_started = true;
	}
	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);
	const double elapsed = (s_freq.QuadPart > 0) ?
	                           static_cast<double>(now.QuadPart - s_start.QuadPart) / static_cast<double>(s_freq.QuadPart) :
	                           0.0;
	if (elapsed >= static_cast<double>(autoHideSeconds)) {
		return;
	}

	const float scale = Util::GetUIScale();
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 40.0f * scale),
		ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.65f);
	if (ImGui::Begin("CSWelcomeOverlay", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
				ImGuiWindowFlags_NoNav)) {
		ImGui::TextUnformatted(T("overlay.welcome_title", "Community Shaders (VR)"));
		ImGui::Separator();
		ImGui::TextUnformatted(T("overlay.welcome_open",
			"Pause the game, then press Shift+F4 - or your bound controller combo - to open the menu."));
		ImGui::TextUnformatted(T("overlay.welcome_bind",
			"(Bind a controller combo in the menu's Interaction settings.)"));
	}
	ImGui::End();
}

void OverlayRenderer::HandleABTesting()
{
	// A/B Testing management
	auto* abTestingManager = ABTestingManager::GetSingleton();
	abTestingManager->Update();

	// Always update test data during TEST phase, regardless of overlay visibility
	if (abTestingManager->IsEnabled()) {
		globals::features::performanceOverlay.UpdateAllShaderTestData();

		// Add A/B test aggregator data collection here
		auto& overlay = globals::features::performanceOverlay;
		auto [mainRows, summaryRows] = overlay.BuildDrawCallRows();
		std::vector<DrawCallRow> allRows = mainRows;
		allRows.insert(allRows.end(), summaryRows.begin(), summaryRows.end());

		// Update the A/B test aggregator with current frame data
		abTestingManager->GetAggregator().OnFrame(allRows);
	}

	// Draw A/B testing overlay
	abTestingManager->DrawOverlayUI();
}

void OverlayRenderer::FinalizeImGuiFrame()
{
	if (auto* menu = Menu::GetSingleton();
		menu && menu->GetSettings().Theme.UseCustomCursor && Util::CursorLoader::GetLoadedCount() > 0) {
		Util::CursorLoader::DrawCustomCursor(*menu);
	}

	ImGui::Render();

	// Apply background blur behind ImGui windows before rendering them
	BackgroundBlur::RenderBackgroundBlur();

	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	// Flat path only (helper not registered): no RenderToPanel. The two-pass
	// VR path (RenderMenuPass/RenderHudPass) targets the helper panels itself.
}

void OverlayRenderer::RenderFirstTimeSetupOverlay()
{
	if (HomePageRenderer::ShouldShowFirstTimeSetup()) {
		HomePageRenderer::RenderFirstTimeSetupDialog();
	}
}

void OverlayRenderer::RenderShaderBlockingStatus()
{
	auto shaderCache = globals::shaderCache;
	auto state = globals::state;

	if (!state->IsDeveloperMode() || shaderCache->blockedKey.empty()) {
		return;
	}

	const float scale = Util::GetUIScale();
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;

	// Stack below shader compilation window if visible
	float yPos = pos;
	if (auto* shaderWin = ImGui::FindWindowByName("ShaderCompilationInfo")) {
		if (shaderWin->Active) {
			yPos = shaderWin->Pos.y + shaderWin->Size.y + ImGui::GetStyle().ItemSpacing.y;
		}
	}
	// Also stack below water cache overlay if visible
	if (auto* waterWin = ImGui::FindWindowByName("UWCacheCreationInfo")) {
		if (waterWin->Active && waterWin->Pos.y + waterWin->Size.y > yPos) {
			yPos = waterWin->Pos.y + waterWin->Size.y + ImGui::GetStyle().ItemSpacing.y;
		}
	}
	ImGui::SetNextWindowPos(ImVec2(pos, yPos));
	if (!ImGui::Begin("ShaderBlockingInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::End();
		return;
	}

	Util::Text::Error(T("overlay.shader_blocking_active", "Shader Blocking Active"));
	ImGui::Text(T("overlay.blocked_key", "Blocked: %s"), shaderCache->blockedKey.c_str());

	// Try to get more details from active shaders
	auto activeShaders = shaderCache->GetActiveShaders();

	// Find the index of the blocked shader in the active list (or show N/A if not found)
	size_t blockedIndex = 0;
	bool foundBlocked = false;
	for (size_t i = 0; i < activeShaders.size(); ++i) {
		if (activeShaders[i].key == shaderCache->blockedKey) {
			blockedIndex = i + 1;  // 1-based indexing for display
			foundBlocked = true;
			break;
		}
	}

	if (foundBlocked) {
		ImGui::Text(T("overlay.blocked_index", "Index: %zu/%zu"), blockedIndex, activeShaders.size());
	} else {
		ImGui::Text(T("overlay.blocked_index_na", "Index: N/A (%zu active)"), activeShaders.size());
	}

	for (const auto& shader : activeShaders) {
		if (shader.key == shaderCache->blockedKey) {
			ImGui::Text(T("overlay.blocked_shader_detail", "Type: %s | Class: %s | Descriptor: 0x%X"),
				magic_enum::enum_name(shader.shaderType).data(),
				magic_enum::enum_name(shader.shaderClass).data(),
				shader.descriptor);
			break;
		}
	}

	ImGui::End();
}
