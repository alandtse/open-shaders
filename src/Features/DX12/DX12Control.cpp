#include "DX12Control.h"
#include "DX12NativeScene.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "State.h"
#include "Utils/UI.h"

#include <algorithm>

void DX12Control::DrawSettings()
{
	// Keep pipeline mode directly below the DX12 heading and above subfeature tabs.
	const char* modes[] = {
		T("menu.dx12.mode.shadow_offload", "Shadow offloading"),
		T("menu.dx12.mode.raster_comparison", "Raster comparison"),
		T("menu.dx12.mode.full_shadow", "Ray-Traced Static Shadows")
	};
	int mode = settings.Mode;
	if (ImGui::Combo(T("menu.dx12.mode.label", "Mode"), &mode, modes, int(std::size(modes)))) {
		settings.Mode = mode;
		selectedMode.store(static_cast<Mode>(mode), std::memory_order_relaxed);
		legacyModeConfig = false;
	}
	const char* description = nullptr;
	switch (static_cast<Mode>(settings.Mode)) {
	case Mode::ShadowOffload:
		description = T("menu.dx12.mode.shadow_offload_description", "Native shadows replace matched raster static shadows.");
		break;
	case Mode::RasterComparison:
		description = T("menu.dx12.mode.raster_comparison_description", "Raster shadows remain unchanged; native shadow dispatch is paused for comparison.");
		break;
	case Mode::FullShadow:
		description = T("menu.dx12.mode.full_shadow_description", "Independent native static shadows for supported point lights; SCM remains unchanged.");
		break;
	}
	ImGui::TextDisabled("%s", description);

	if (!IsActive()) {
		ImGui::Checkbox(T("menu.dx12.master.enable", "Enable native DX12 features (restart required)"), &settings.Enabled);
		if (settings.Enabled)
			ImGui::TextDisabled(T("menu.dx12.master.restart_resources", "Restart the game to initialize DX12 Resources."));
		return;
	}
	if (!Util::BeginPillTabBar("##DX12Tabs"))
		return;
	const char* labels[] = { T("menu.dx12.tab.resources", "Resources"), T("menu.dx12.tab.native_scene", "Native Scene"), T("menu.dx12.tab.native_composite", "Native Composite"), T("menu.dx12.tab.master_switch", "Master Switch"), T("menu.dx12.tab.dev", "Dev") };
	const int tabCount = globals::state && globals::state->IsDeveloperMode() ? int(std::size(labels)) : int(std::size(labels)) - 1;
	for (int index = 0; index < tabCount; ++index) {
		if (!Util::BeginPillTabItem(labels[index]))
			continue;
		switch (index) {
		case 0:
			globals::features::dx12Resources.DrawSettings();
			break;
		case 1:
			globals::features::dx12NativeScene.DrawSettings();
			break;
		case 2:
			ImGui::TextDisabled("%s", T("menu.dx12.composite.mode_controlled", "Shadow reconstruction follows the selected quality mode."));
			break;
		case 3:
			ImGui::Checkbox(T("menu.dx12.master.enable", "Enable native DX12 features (restart required)"), &settings.Enabled);
			if (settings.Enabled != bootActive.load(std::memory_order_acquire))
				ImGui::TextDisabled(T("menu.dx12.master.restart_boot_state", "A game restart is required; DX12 Resources retains its boot state until then."));
			break;
		case 4:
			ImGui::SeparatorText(T("menu.dx12.tab.resources", "Resources"));
			globals::features::dx12Resources.DrawDevSettings();
			ImGui::SeparatorText(T("menu.dx12.tab.native_scene", "Native Scene"));
			globals::features::dx12NativeScene.DrawDevSettings();
			ImGui::SeparatorText(T("menu.dx12.tab.native_composite", "Native Composite"));
			DrawCompositeDev();
			break;
		}
		Util::EndPillTabItem();
	}
	Util::EndPillTabBar();
}

void DX12Control::DrawCompositeDev()
{
	if (!IsFullShadowMode())
		return;
	auto& profile = rayTracedComposite;
	bool filtering = profile.spatialReconstruction.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.composite.spatial_reconstruction", "Additional spatial filtering"), &filtering))
		profile.spatialReconstruction.store(filtering, std::memory_order_relaxed);
	int gridIndex = int((profile.reconstructionGrid.load(std::memory_order_relaxed) - 3u) / 2u);
	if (ImGui::Combo(T("menu.dx12.composite.reconstruction_grid", "Reconstruction grid"), &gridIndex,
			"3x3\0"
			"5x5\0"
			"7x7\0"
			"9x9\0"))
		profile.reconstructionGrid.store(uint32_t(3 + gridIndex * 2), std::memory_order_relaxed);
}

void DX12Control::LoadComposite(CompositeProfile& profile, const json& input, float defaultBlend)
{
	profile.spatialReconstruction.store(input.value("SpatialReconstruction", true), std::memory_order_relaxed);
	const uint32_t requestedGrid = std::clamp(input.value("ReconstructionGridSize", 5u), 3u, 9u);
	profile.reconstructionGrid.store(requestedGrid - ((requestedGrid & 1u) == 0u ? 1u : 0u), std::memory_order_relaxed);
	profile.shadowStrength.store(std::clamp(input.value("ShadowStrength", 1.0f), 0.0f, 4.0f), std::memory_order_relaxed);
	profile.shadowBlend.store(std::clamp(input.value("ShadowBlend", defaultBlend), 0.0f, 1.0f), std::memory_order_relaxed);
}

json DX12Control::SaveComposite(const CompositeProfile& profile)
{
	return { { "SpatialReconstruction", profile.spatialReconstruction.load(std::memory_order_relaxed) },
		{ "ReconstructionGridSize", profile.reconstructionGrid.load(std::memory_order_relaxed) },
		{ "ShadowStrength", profile.shadowStrength.load(std::memory_order_relaxed) },
		{ "ShadowBlend", profile.shadowBlend.load(std::memory_order_relaxed) } };
}

void DX12Control::LoadSettings(json& input)
{
	settings.Enabled = input.value("Enabled", false);
	settings.Mode = std::clamp(input.value("Mode", int(Mode::ShadowOffload)), int(Mode::ShadowOffload), int(Mode::FullShadow));
	legacyModeConfig = !input.contains("FullShadowModeSelection");
	const bool rayTraced = settings.Mode == int(Mode::FullShadow);
	LoadComposite(rayTracedComposite, DX12::ReadModeProfile(input, true, rayTraced), kRayTracedDefaultBlend);
	selectedMode.store(static_cast<Mode>(settings.Mode), std::memory_order_release);
	if (!bootCaptured) {
		bootActive.store(settings.Enabled, std::memory_order_release);
		bootCaptured = true;
	}
}

void DX12Control::SaveSettings(json& output)
{
	output = { { "Enabled", settings.Enabled }, { "Mode", settings.Mode }, { "FullShadowModeSelection", true },
		{ DX12::kRayTracedProfile, SaveComposite(rayTracedComposite) } };
}

void DX12Control::RestoreDefaultSettings()
{
	settings = {};
	legacyModeConfig = false;
	LoadComposite(rayTracedComposite, json::object(), kRayTracedDefaultBlend);
	selectedMode.store(static_cast<Mode>(settings.Mode), std::memory_order_release);
}

void DX12Control::MigrateLegacyFullShadowMode(bool enabled)
{
	if (!legacyModeConfig)
		return;
	legacyModeConfig = false;
	if (enabled && settings.Mode == int(Mode::ShadowOffload)) {
		settings.Mode = int(Mode::FullShadow);
		selectedMode.store(Mode::FullShadow, std::memory_order_relaxed);
	}
}

void DX12Control::OnRuntimeDisabled()
{
	if (!bootActive.exchange(false, std::memory_order_acq_rel))
		return;
	globals::features::dx12NativeScene.OnRuntimeDisabled();
	globals::features::dx12Resources.OnRuntimeDisabled();
}
