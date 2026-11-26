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
	const char* modes[] = { T("menu.dx12.mode.shadow_offload", "Shadow offload"), T("menu.dx12.mode.raster_comparison", "Raster comparison") };
	int mode = settings.Mode;
	if (ImGui::Combo(T("menu.dx12.mode.label", "Mode"), &mode, modes, int(std::size(modes)))) {
		settings.Mode = mode;
		selectedMode.store(mode, std::memory_order_relaxed);
	}
	ImGui::TextDisabled(settings.Mode == 0 ?
							T("menu.dx12.mode.shadow_offload_description", "Native shadows replace matched raster static shadows.") :
							T("menu.dx12.mode.raster_comparison_description", "Raster shadows remain unchanged; native shadow dispatch is paused for comparison."));

	if (!IsActive()) {
		ImGui::Checkbox(T("menu.dx12.master.enable", "Enable native DX12 features (restart required)"), &settings.Enabled);
		if (settings.Enabled)
			ImGui::TextDisabled(T("menu.dx12.master.restart_resources", "Restart the game to initialize DX12 Resources."));
		return;
	}
	if (!Util::BeginPillTabBar("##DX12Tabs"))
		return;
	const char* labels[] = { T("menu.dx12.tab.resources", "Resources"), T("menu.dx12.tab.native_scene", "Native Scene"), T("menu.dx12.tab.native_composite", "Native Composite"), T("menu.dx12.tab.master_switch", "Master Switch") };
	for (int index = 0; index < 4; ++index) {
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
			{
				ImGui::Checkbox(T("menu.dx12.composite.spatial_reconstruction", "Additional spatial filtering"), &settings.SpatialReconstruction);
				spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
				int gridIndex = int((settings.ReconstructionGridSize - 3u) / 2u);
				if (ImGui::Combo(T("menu.dx12.composite.reconstruction_grid", "Reconstruction grid"), &gridIndex,
						"3x3\0"
						"5x5\0"
						"7x7\0"
						"9x9\0")) {
					settings.ReconstructionGridSize = uint32_t(3 + gridIndex * 2);
					reconstructionGrid.store(settings.ReconstructionGridSize, std::memory_order_relaxed);
				}
				if (ImGui::SliderFloat(T("menu.dx12.composite.shadow_strength", "Shadow strength"), &settings.ShadowStrength, 0.0f, 4.0f, "%.2f"))
					shadowStrength.store(settings.ShadowStrength, std::memory_order_relaxed);
				if (ImGui::SliderFloat(T("menu.dx12.composite.shadow_blend", "Shadow blend"), &settings.ShadowBlend, 0.0f, 1.0f, "%.2f"))
					shadowBlend.store(settings.ShadowBlend, std::memory_order_relaxed);
				ImGui::TextDisabled(T("menu.dx12.composite.before_upscaling", "Reconstruction and shadow application run before native upscaling."));
				break;
			}
		case 3:
			ImGui::Checkbox(T("menu.dx12.master.enable", "Enable native DX12 features (restart required)"), &settings.Enabled);
			if (settings.Enabled != bootActive.load(std::memory_order_acquire))
				ImGui::TextDisabled(T("menu.dx12.master.restart_boot_state", "A game restart is required; DX12 Resources retains its boot state until then."));
			break;
		}
		Util::EndPillTabItem();
	}
	Util::EndPillTabBar();
}

void DX12Control::LoadSettings(json& input)
{
	settings.Enabled = input.value("Enabled", false);
	settings.Mode = std::clamp(input.value("Mode", 0), 0, 1);
	settings.SpatialReconstruction = input.value("SpatialReconstruction", true);
	const uint32_t requestedGrid = std::clamp(input.value("ReconstructionGridSize", 5u), 3u, 9u);
	settings.ReconstructionGridSize = requestedGrid - ((requestedGrid & 1u) == 0u ? 1u : 0u);
	settings.ShadowStrength = std::clamp(input.value("ShadowStrength", 1.0f), 0.0f, 4.0f);
	settings.ShadowBlend = std::clamp(input.value("ShadowBlend", 0.70f), 0.0f, 1.0f);
	selectedMode.store(settings.Mode, std::memory_order_relaxed);
	spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
	reconstructionGrid.store(settings.ReconstructionGridSize, std::memory_order_relaxed);
	shadowStrength.store(settings.ShadowStrength, std::memory_order_relaxed);
	shadowBlend.store(settings.ShadowBlend, std::memory_order_relaxed);
	if (!bootCaptured) {
		bootActive.store(settings.Enabled, std::memory_order_release);
		bootCaptured = true;
	}
}

void DX12Control::SaveSettings(json& output)
{
	output = { { "Enabled", settings.Enabled }, { "Mode", settings.Mode },
		{ "SpatialReconstruction", settings.SpatialReconstruction },
		{ "ReconstructionGridSize", settings.ReconstructionGridSize },
		{ "ShadowStrength", settings.ShadowStrength }, { "ShadowBlend", settings.ShadowBlend } };
}

void DX12Control::RestoreDefaultSettings()
{
	settings = {};
	selectedMode.store(settings.Mode, std::memory_order_relaxed);
	spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
	reconstructionGrid.store(settings.ReconstructionGridSize, std::memory_order_relaxed);
	shadowStrength.store(settings.ShadowStrength, std::memory_order_relaxed);
	shadowBlend.store(settings.ShadowBlend, std::memory_order_relaxed);
}

void DX12Control::OnRuntimeDisabled()
{
	if (!bootActive.exchange(false, std::memory_order_acq_rel))
		return;
	globals::features::dx12NativeScene.OnRuntimeDisabled();
	globals::features::dx12Resources.OnRuntimeDisabled();
}
