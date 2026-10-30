#include "OverlayRenderer.h"
#include "BackgroundBlur.h"
#include "HomePageRenderer.h"
#include "ThemeManager.h"

#include <array>
#include <cmath>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <winrt/base.h>

#include "CSEditor/EditorWindow.h"
#include "Feature.h"
#include "FeatureIssues.h"
#include "Menu/DesktopMirrorDrawData.h"
#if defined(ENABLE_EFFECTS11)
#	include "Features/Effects11/EffectManager.h"
#	include "Features/Effects11/Editor/Effects11Editor.h"
#endif
#include "Features/HDRDisplay.h"
#include "Features/RenderDoc.h"
#include "Features/SceneSelector.h"
#include "Features/Upscaling.h"
#include "Features/VR.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/CursorLoader.h"
#include "Menu/ImGuiSurfaceSync.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

#include "Features/PerformanceOverlay.h"
#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"

namespace
{
	DesktopMirrorDrawData pendingDesktopMirror;
	ImDrawData* pendingDesktopDrawData = nullptr;
	ImGuiSurfaceSync g_surfaceSync;

	bool IsSupportedDesktopMirrorFormat(const DXGI_FORMAT format)
	{
		return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
		       format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	}

	void UpdateVRPanelDeltaTime()
	{
		static INT64 lastVrTicks = 0;
		LARGE_INTEGER ticksPerSecond{};
		LARGE_INTEGER currentTicks{};
		if (QueryPerformanceFrequency(&ticksPerSecond) && QueryPerformanceCounter(&currentTicks) && ticksPerSecond.QuadPart > 0) {
			if (lastVrTicks > 0 && currentTicks.QuadPart > lastVrTicks) {
				ImGui::GetIO().DeltaTime = static_cast<float>(
					static_cast<double>(currentTicks.QuadPart - lastVrTicks) / static_cast<double>(ticksPerSecond.QuadPart));
			}
			lastVrTicks = currentTicks.QuadPart;
		}
	}

	void DrawShaderCompilationFailures(uint64_t failed, const Menu::ThemeSettings& themeSettings)
	{
		if (failed) {
			ImGui::TextColored(themeSettings.StatusPalette.Error,
				T("overlay.shaders_failed", "ERROR: %llu shaders failed to compile. Check installation and OpenShaders.log"),
				static_cast<unsigned long long>(failed));

			if (FeatureIssues::HasPotentialShaderModifyingFeatures()) {
				ImGui::TextColored(themeSettings.StatusPalette.Error, "%s", T("overlay.modified_features", "Features that may have modified shaders detected. Check Feature Issues in the Menu."));
			}
		}
	}

#if defined(ENABLE_EFFECTS11)
	void DrawEffects11Errors(const Menu::ThemeSettings& themeSettings)
	{
		auto& effectManager = EffectManager::GetSingleton();
		if (!effectManager.IsInitialized())
			return;

		uint32_t effectFailed = effectManager.GetFailedEffectCount();
		if (effectFailed) {
			ImGui::TextColored(themeSettings.StatusPalette.Error,
				"ERROR: %u effect(s) failed to compile",
				effectFailed);
			for (const auto& err : effectManager.GetAllErrors())
				ImGui::TextColored(themeSettings.StatusPalette.Error, "  %s", err.c_str());
		}
	}
#endif

}  // namespace

void OverlayRenderer::RenderOverlay(
	Menu& menu,
	const std::function<void()>& processInputEventQueue,
	const std::function<void()>& drawSettings,
	const std::function<const char*(std::vector<InputCombo>)>& keyIdToString,
	float& cachedFontSize,
	float currentFontSize)
{
	BackgroundBlur::RestoreRetainedBuffers();

	uint32_t panelW = 0, panelH = 0;
	const bool useDualVRSurfaces = globals::game::isVR &&
	                               globals::features::vr.GetHelperPanelSize(panelW, panelH) &&
	                               menu.EnsureVRPanelContext();
	if (useDualVRSurfaces) {
		ScopedImGuiContext vrScope(menu.GetVRPanelContext());
		ApplyVRPanelDisplaySize();
	}
	processInputEventQueue();

	const bool acceptsInput = !menu.IsPreviewFlying() &&
	                          (menu.ShouldSwallowInput() || globals::features::vr.HelperRequestsRender());
	const auto applyInputAcceptance = [acceptsInput](ImGuiContext* ctx) {
		if (!ctx)
			return;
		ScopedImGuiContext scope(ctx);
		auto& surfaceIO = ImGui::GetIO();
		surfaceIO.SetAppAcceptingEvents(acceptsInput);
		if (!acceptsInput) {
			surfaceIO.ClearEventsQueue();
			surfaceIO.ClearInputKeys();
			surfaceIO.ClearInputMouse();
			surfaceIO.WantSetMousePos = false;
			ImGui::ClearActiveID();
		}
	};
	ImGuiContext* desktopCtx = menu.GetDesktopContext() ? menu.GetDesktopContext() : ImGui::GetCurrentContext();
	applyInputAcceptance(desktopCtx);
	if (useDualVRSurfaces)
		applyInputAcceptance(menu.GetVRPanelContext());

	auto* editorWindow = EditorWindow::GetSingleton();
	if (ShouldSkipRendering()) {
		editorWindow->FinishGameHourSliderFrame(false);
		const auto clearIdleSurface = [](ImGuiContext* ctx) {
			if (!ctx)
				return;
			ScopedImGuiContext scope(ctx);
			auto& surfaceIO = ImGui::GetIO();
			surfaceIO.ClearInputKeys();
			surfaceIO.ClearEventsQueue();
		};
		clearIdleSurface(desktopCtx);
		if (useDualVRSurfaces)
			clearIdleSurface(menu.GetVRPanelContext());
		editorWindow->UpdateOpenState();
		return;
	}

	if (editorWindow->open && !EditorWindow::CanBeOpen()) {
		editorWindow->open = false;
		if (editorWindow->IsInPreviewMode())
			editorWindow->ExitPreviewMode();
	}
	editorWindow->UpdateOpenState();
#if defined(ENABLE_EFFECTS11)
	auto& effects11Editor = Effects11Editor::GetSingleton();
	if (effects11Editor.IsOpen() && (editorWindow->open || menu.IsEnabled))
		effects11Editor.Close(false);
#endif
	UpdateABTesting();

	if (useDualVRSurfaces) {
		ImGuiContext* vrCtx = menu.GetVRPanelContext();
		{
			ScopedImGuiContext vrScope(vrCtx);
			g_surfaceSync.ApplyBeforeNewFrame(vrCtx);
			HandleFontReload(menu, menu.vrCachedFontSize, ThemeManager::ResolveFontSize(menu));
			InitializeImGuiFrame(menu, true);
			DrawOverlaySurfaceContent(menu, drawSettings, keyIdToString);
			ImGui::Render();
			globals::features::vr.RenderHelperToPanel();
			g_surfaceSync.OnFrameCompleted(vrCtx, desktopCtx);
		}
		{
			ScopedImGuiContext desktopScope(desktopCtx);
			g_surfaceSync.ApplyBeforeNewFrame(desktopCtx);
			HandleFontReload(menu, cachedFontSize, ThemeManager::ResolveFontSize(menu));
			InitializeImGuiFrame(menu, false);
			DrawOverlaySurfaceContent(menu, drawSettings, keyIdToString);
			ImGui::Render();
			g_surfaceSync.OnFrameCompleted(desktopCtx, vrCtx);
			auto* desktopDrawData = ImGui::GetDrawData();
			if (!StageDesktopMirror(desktopDrawData) && !BackgroundBlur::RenderDrawData(desktopDrawData))
				ImGui_ImplDX11_RenderDrawData(desktopDrawData);
		}
	} else {
		g_surfaceSync.Reset();
		HandleFontReload(menu, cachedFontSize, currentFontSize);
		InitializeImGuiFrame(menu, false);
		DrawOverlaySurfaceContent(menu, drawSettings, keyIdToString);
		ImGui::Render();
		auto* drawData = ImGui::GetDrawData();
		if (!BackgroundBlur::RenderDrawData(drawData))
			ImGui_ImplDX11_RenderDrawData(drawData);
	}

	editorWindow->FinishGameHourSliderFrame(true);
	menu.FinishPendingTabFrame();
}

void OverlayRenderer::DrawOverlaySurfaceContent(
	Menu& menu,
	const std::function<void()>& drawSettings,
	const std::function<const char*(std::vector<InputCombo>)>& keyIdToString)
{
	auto& io = ImGui::GetIO();
	auto* editorWindow = EditorWindow::GetSingleton();
#if defined(ENABLE_EFFECTS11)
	auto& effects11Editor = Effects11Editor::GetSingleton();
#endif

	RenderShaderCompilationStatus(keyIdToString);
	RenderShaderBlockingStatus();

	if (editorWindow->open) {
		bool flying = editorWindow->IsPreviewFlying();
		io.MouseDrawCursor = !flying;
		if (flying)
			io.MousePos = { -FLT_MAX, -FLT_MAX };
		editorWindow->Draw();
#if defined(ENABLE_EFFECTS11)
	} else if (effects11Editor.IsOpen()) {
		io.MouseDrawCursor = true;
		effects11Editor.Draw();
#endif
	} else if (menu.IsEnabled || HomePageRenderer::ShouldShowFirstTimeSetup() ||
			   globals::features::vr.HelperRequestsRender()) {
		io.MouseDrawCursor = true;
		if (menu.IsEnabled || globals::features::vr.HelperRequestsRender()) {
			drawSettings();
		}
	} else {
		io.MouseDrawCursor = false;
	}

	RenderFeatureOverlays();
	RenderFirstTimeSetupOverlay();
	ABTestingManager::GetSingleton()->DrawOverlayUI();

	if (menu.GetSettings().Theme.UseCustomCursor && Util::CursorLoader::GetLoadedCount() > 0) {
		Util::CursorLoader::DrawCustomCursor(menu);
	}
}

bool OverlayRenderer::ShouldSkipRendering()
{
#if defined(ENABLE_EFFECTS11)
	if (Effects11Editor::GetSingleton().IsOpen())
		return false;
#endif
	auto shaderCache = globals::shaderCache;
	auto failed = shaderCache->GetCurrentFailedCount();
	auto hide = shaderCache->IsHideErrors();
	auto* abTestingManager = ABTestingManager::GetSingleton();
	auto* renderDoc = RenderDoc::GetSingleton();

#if defined(ENABLE_EFFECTS11)
	uint32_t effectFailed = EffectManager::GetSingleton().IsInitialized() ? EffectManager::GetSingleton().GetFailedEffectCount() : 0;
#else
	uint32_t effectFailed = 0;
#endif

	return !(shaderCache->IsCompiling() ||
			 Menu::GetSingleton()->IsEnabled ||
			 HomePageRenderer::ShouldShowFirstTimeSetup() ||
			 EditorWindow::GetSingleton()->open ||
			 abTestingManager->IsEnabled() ||
			 (failed && !hide) ||
			 effectFailed ||
			 globals::features::performanceOverlay.settings.ShowInOverlay ||
			 globals::features::sceneSelector.IsOverlayVisible() ||
			 renderDoc->IsAvailable() ||
			 globals::features::vr.HelperRequestsRender());
}

void OverlayRenderer::HandleFontReload(Menu& menu, float& cachedFontSize, float currentFontSize)
{
	bool fontSizeChanged = std::abs(cachedFontSize - currentFontSize) > ThemeManager::Constants::FONT_CACHE_EPSILON;
	std::string desiredSignature = menu.BuildFontSignature(currentFontSize);
	bool signatureChanged = desiredSignature != menu.ActiveFontSignature();
	bool previewChanged = menu.ActiveWantsFontPreviewAtlas() != menu.ActivePreviewAtlasLoaded();

	if (fontSizeChanged || signatureChanged || previewChanged) {
		if (!ThemeManager::ReloadFont(menu, cachedFontSize)) {
			logger::warn("OverlayRenderer::HandleFontReload() - Font reload failed");
		}
	}
}

bool OverlayRenderer::ApplyVRPanelDisplaySize()
{
	uint32_t panelW = 0, panelH = 0;
	if (!globals::game::isVR || !globals::features::vr.GetHelperPanelSize(panelW, panelH))
		return false;

	auto& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(static_cast<float>(panelW), static_cast<float>(panelH));
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	return true;
}

void OverlayRenderer::InitializeImGuiFrame(Menu& menu, bool isVRPanelPass)
{
	ImGui_ImplDX11_NewFrame();
	if (isVRPanelPass) {
		UpdateVRPanelDeltaTime();
		ApplyVRPanelDisplaySize();
	} else {
		ImGui_ImplWin32_NewFrame();
		DXGI_SWAP_CHAIN_DESC desc{};
		if (globals::d3d::swapChain && SUCCEEDED(globals::d3d::swapChain->GetDesc(&desc))) {
			const float displayW = static_cast<float>(desc.BufferDesc.Width);
			const float displayH = static_cast<float>(desc.BufferDesc.Height);
			Util::UpdateImGuiInput(desc.OutputWindow, displayW, displayH);
		}
	}

	ImGui::NewFrame();

	auto& lastDisplaySize = menu.ActiveLastDisplaySize();
	const float2 currentDisplaySize{ ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y };
	if (lastDisplaySize.x > 0.f && lastDisplaySize != currentDisplaySize) {
		logger::info("Display size changed: {}x{} -> {}x{}, resetting window layout",
			lastDisplaySize.x, lastDisplaySize.y, currentDisplaySize.x, currentDisplaySize.y);
		menu.ActiveResetLayout() = true;
		EditorWindow::GetSingleton()->resetLayout = true;
#if defined(ENABLE_EFFECTS11)
		Effects11Editor::GetSingleton().RequestLayoutReset();
#endif
		globals::features::performanceOverlay.ResetWindowLayout();
		globals::features::sceneSelector.ResetWindowLayout();
	}
	lastDisplaySize = currentDisplaySize;

	ThemeManager::SetupImGuiStyle(menu);
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

#if defined(ENABLE_EFFECTS11)
	uint32_t effectFailed = EffectManager::GetSingleton().IsInitialized() ? EffectManager::GetSingleton().GetFailedEffectCount() : 0;
#else
	uint32_t effectFailed = 0;
#endif

	if (shaderCache->IsCompiling()) {
		// VR immersion: suppress only the routine background-compile readout; the
		// blocking-compile path and anything exceptional still show below.
		const bool hideRoutineHud = globals::game::isVR && shaderCache->backgroundCompilation &&
		                            Menu::GetSingleton()->GetSettings().HideCompilationHUDInVR;
		const bool hasExceptionalInfo = shaderCache->IsDiskCacheHeld() || shaderCache->HasFeatureSetChanges() || FeatureIssues::CountLoadFailures() > 0 ||
		                                (failed && !hide) || effectFailed || renderDocAvailable || state->IsDeveloperMode();
		if (hideRoutineHud && !hasExceptionalInfo)
			return;

		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}
		if (!hideRoutineHud) {
			ImGui::TextUnformatted(progressTitle.c_str());
			ImGui::ProgressBar(percent, ImVec2(0.0f, 0.0f), progressOverlay.c_str());
		}
		if (shaderCache->HasFeatureSetRevertPending()) {
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
				T("overlay.revert_pending",
					"Previous cache restored.\n"
					"Restart to use it."));
		} else if (shaderCache->HasFeatureSetChanges()) {
			if (shaderCache->HasFeatureSetCacheBackup()) {
				ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
					T("overlay.feature_changed_backup",
						"Feature setup changed. Building a new shader cache for this setup.\n"
						"Previous cache saved."));
			} else {
				ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
					T("overlay.feature_changed_no_backup",
						"Feature setup changed. Building shaders in memory until the cache can be rebuilt.\n"
						"Previous cache is not available for restore."));
			}
		} else if (shaderCache->IsDiskCacheHeld()) {
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s",
				T("overlay.cache_held",
					"Saved shader cache cannot be used.\n"
					"A required feature is missing or failed to load."));
		}
		// Surface bad-install feature problems during the compile itself; otherwise
		// the user only learns from the Feature Issues tab after baking the wrong set.
		if (const size_t issueCount = FeatureIssues::CountLoadFailures()) {
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
		if (failed && !hide)
			DrawShaderCompilationFailures(failed, themeSettings);
#if defined(ENABLE_EFFECTS11)
		DrawEffects11Errors(themeSettings);
#endif

		if (renderDocAvailable)
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s", renderDocInformation.c_str());

		ImGui::End();
		return;
	}

	if ((failed && !hide) || effectFailed) {
		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}

		DrawShaderCompilationFailures(failed, themeSettings);
#if defined(ENABLE_EFFECTS11)
		DrawEffects11Errors(themeSettings);
#endif

		if (renderDocAvailable)
			ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s", renderDocInformation.c_str());

		ImGui::End();
	} else if (renderDocAvailable) {
		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}
		ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s", renderDocInformation.c_str());
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

void OverlayRenderer::UpdateABTesting()
{
	auto* abTestingManager = ABTestingManager::GetSingleton();
	abTestingManager->Update();

	if (abTestingManager->IsEnabled()) {
		globals::features::performanceOverlay.UpdateAllShaderTestData();

		auto& overlay = globals::features::performanceOverlay;
		auto [mainRows, summaryRows] = overlay.BuildDrawCallRows();
		std::vector<DrawCallRow> allRows = mainRows;
		allRows.insert(allRows.end(), summaryRows.begin(), summaryRows.end());

		abTestingManager->RecordFrame(allRows);
	}
}

bool OverlayRenderer::StageDesktopMirror(ImDrawData* drawData)
{
	pendingDesktopMirror.Reset();
	pendingDesktopDrawData = nullptr;
	uint32_t panelWidth = 0;
	uint32_t panelHeight = 0;
	const bool helperPanelActive = globals::game::isVR && globals::features::vr.GetHelperPanelSize(panelWidth, panelHeight);
	if (!helperPanelActive || !drawData || !globals::d3d::swapChain)
		return false;
	if (globals::features::upscaling.d3d12SwapChainActive ||
		(globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.settings.enableHDR) ||
		BackgroundBlur::HasHDRSceneImageCallback(drawData))
		return false;

	DXGI_SWAP_CHAIN_DESC swapChainDesc{};
	if (FAILED(globals::d3d::swapChain->GetDesc(&swapChainDesc)))
		return false;
	if (!IsSupportedDesktopMirrorFormat(swapChainDesc.BufferDesc.Format))
		return false;

	pendingDesktopDrawData = drawData;
	return true;
}

void OverlayRenderer::RenderDesktopMirrorAtPresent(IDXGISwapChain* swapChain, const bool hdrReady, const bool frameGenActive)
{
	const auto clearPending = []() {
		pendingDesktopMirror.Reset();
		pendingDesktopDrawData = nullptr;
	};
	if (!pendingDesktopDrawData)
		return;
	const SKSE::stl::scope_exit consumePending([&]() noexcept {
		pendingDesktopMirror.Reset();
		pendingDesktopDrawData = nullptr;
	});
	if (!swapChain || hdrReady || frameGenActive || globals::features::upscaling.d3d12SwapChainActive ||
		(globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.settings.enableHDR)) {
		clearPending();
		return;
	}
	if (!globals::d3d::device || !globals::d3d::context) {
		clearPending();
		return;
	}

	auto* menu = Menu::GetSingleton();
	ScopedImGuiContext desktopScope(menu && menu->GetDesktopContext() ? menu->GetDesktopContext() : ImGui::GetCurrentContext());

	DXGI_SWAP_CHAIN_DESC swapChainDesc{};
	if (FAILED(swapChain->GetDesc(&swapChainDesc))) {
		clearPending();
		return;
	}
	UINT bufferIndex = 0;
	winrt::com_ptr<IDXGISwapChain3> swapChain3;
	if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(swapChain3.put())))) {
		bufferIndex = swapChain3->GetCurrentBackBufferIndex();
	} else if (swapChainDesc.BufferCount > 1 && swapChainDesc.SwapEffect != DXGI_SWAP_EFFECT_DISCARD) {
		clearPending();
		return;
	}

	winrt::com_ptr<ID3D11Texture2D> backBuffer;
	if (FAILED(swapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(backBuffer.put()))) || !backBuffer) {
		clearPending();
		return;
	}
	D3D11_TEXTURE2D_DESC backBufferDesc{};
	backBuffer->GetDesc(&backBufferDesc);

	winrt::com_ptr<ID3D11RenderTargetView> backBufferRTV;
	if (FAILED(globals::d3d::device->CreateRenderTargetView(backBuffer.get(), nullptr, backBufferRTV.put()))) {
		clearPending();
		return;
	}
	Util::SetResourceName(backBufferRTV.get(), "OverlayRenderer::DesktopMirror RTV");

	auto context = globals::d3d::context;
	std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rawPreviousRTVs{};
	std::array<winrt::com_ptr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> previousRTVs{};
	winrt::com_ptr<ID3D11DepthStencilView> previousDSV;
	context->OMGetRenderTargets(static_cast<UINT>(rawPreviousRTVs.size()), rawPreviousRTVs.data(), previousDSV.put());
	for (size_t i = 0; i < rawPreviousRTVs.size(); ++i)
		previousRTVs[i].attach(rawPreviousRTVs[i]);
	D3D11_VIEWPORT previousViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT previousViewportCount = ARRAYSIZE(previousViewports);
	context->RSGetViewports(&previousViewportCount, previousViewports);
	D3D11_RECT previousScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT previousScissorCount = ARRAYSIZE(previousScissors);
	context->RSGetScissorRects(&previousScissorCount, previousScissors);
	const SKSE::stl::scope_exit restoreOutput([&]() noexcept {
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
		for (size_t i = 0; i < previousRTVs.size(); ++i)
			targets[i] = previousRTVs[i].get();
		context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), previousDSV.get());
		context->RSSetViewports(previousViewportCount, previousViewports);
		context->RSSetScissorRects(previousScissorCount, previousScissors);
	});

	auto* target = backBufferRTV.get();
	context->OMSetRenderTargets(1, &target, nullptr);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(backBufferDesc.Width), static_cast<float>(backBufferDesc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	const D3D11_RECT scissor{ 0, 0, static_cast<LONG>(backBufferDesc.Width), static_cast<LONG>(backBufferDesc.Height) };
	context->RSSetScissorRects(1, &scissor);

	CS_GPU_PASS("OverlayRenderer::DesktopMirror");
	const bool supportedBackBuffer = IsSupportedDesktopMirrorFormat(backBufferDesc.Format) && backBufferDesc.SampleDesc.Count == 1;
	const bool matchesBackBuffer = supportedBackBuffer &&
	                               std::abs(static_cast<float>(backBufferDesc.Width) - pendingDesktopDrawData->DisplaySize.x) <= 0.5f &&
	                               std::abs(static_cast<float>(backBufferDesc.Height) - pendingDesktopDrawData->DisplaySize.y) <= 0.5f;
	if (matchesBackBuffer) {
		if (!BackgroundBlur::RenderDrawDataForDesktopMirror(pendingDesktopDrawData, *pendingDesktopDrawData))
			ImGui_ImplDX11_RenderDrawData(pendingDesktopDrawData);
		clearPending();
		return;
	}
	if (supportedBackBuffer &&
		pendingDesktopMirror.Build(*pendingDesktopDrawData,
			ImVec2(static_cast<float>(backBufferDesc.Width), static_cast<float>(backBufferDesc.Height)))) {
		auto* mirrorDrawData = pendingDesktopMirror.Get();
		if (!BackgroundBlur::RenderDrawDataForDesktopMirror(mirrorDrawData, *pendingDesktopDrawData))
			ImGui_ImplDX11_RenderDrawData(mirrorDrawData);
		clearPending();
		return;
	}
	ImGui_ImplDX11_RenderDrawData(pendingDesktopDrawData);
	clearPending();
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

	Util::Text::Error("%s", T("overlay.shader_blocking_active", "Shader Blocking Active"));
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
