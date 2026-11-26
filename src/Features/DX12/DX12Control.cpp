#include "DX12Control.h"
#include "DX12NativeScene.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "State.h"
#include "Utils/SettingsControl.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DX12Control::Settings,
	Enabled,
	Mode,
	SpatialReconstruction,
	ReconstructionGridSize,
	ShadowStrength,
	ShadowBlend)

namespace
{
	constexpr std::array<const char*, 4> kReconstructionGridLabels = { "3x3", "5x5", "7x7", "9x9" };
	constexpr std::array<uint32_t, 4> kReconstructionGridSizes = { 3, 5, 7, 9 };
}

void DX12Control::SyncRuntimeSettings() noexcept
{
	settings.Mode = std::clamp(settings.Mode, 0, 1);
	const uint32_t requestedGrid = std::clamp(settings.ReconstructionGridSize, 3u, 9u);
	settings.ReconstructionGridSize = requestedGrid - ((requestedGrid & 1u) == 0u ? 1u : 0u);
	settings.ShadowStrength = std::clamp(settings.ShadowStrength, 0.0f, 4.0f);
	settings.ShadowBlend = std::clamp(settings.ShadowBlend, 0.0f, 1.0f);
	selectedMode.store(settings.Mode, std::memory_order_relaxed);
	spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
	reconstructionGrid.store(settings.ReconstructionGridSize, std::memory_order_relaxed);
	shadowStrength.store(settings.ShadowStrength, std::memory_order_relaxed);
	shadowBlend.store(settings.ShadowBlend, std::memory_order_relaxed);
}

void DX12Control::DrawMasterEnableCheckbox()
{
	ImGui::Checkbox(T("menu.dx12.master.enable", "Enable native DX12 features (restart required)"), &settings.Enabled);
}

void DX12Control::DrawSettings()
{
	// Keep pipeline mode directly below the DX12 heading and above subfeature tabs.
	const char* modes[] = { T("menu.dx12.mode.shadow_offload", "Shadow offload"), T("menu.dx12.mode.raster_comparison", "Raster comparison") };
	if (Util::Combo(T("menu.dx12.mode.label", "Mode"), &settings.Mode, modes) && !SettingsControl::IsReplicating())
		SyncRuntimeSettings();
	ImGui::TextDisabled(settings.Mode == 0 ?
							T("menu.dx12.mode.shadow_offload_description", "Native shadows replace matched raster static shadows.") :
							T("menu.dx12.mode.raster_comparison_description", "Raster shadows remain unchanged; native shadow dispatch is paused for comparison."));

	if (!IsActive()) {
		DrawMasterEnableCheckbox();
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
			Util::DrawEmbeddedFeatureSettings(globals::features::dx12Resources);
			break;
		case 1:
			Util::DrawEmbeddedFeatureSettings(globals::features::dx12NativeScene);
			break;
		case 2:
			{
				if (ImGui::Checkbox(T("menu.dx12.composite.spatial_reconstruction", "Spatial shadow reconstruction"), &settings.SpatialReconstruction) &&
					!SettingsControl::IsReplicating())
					SyncRuntimeSettings();
				ImGui::BeginDisabled(!settings.SpatialReconstruction);
				if (Util::Combo(T("menu.dx12.composite.reconstruction_grid", "Reconstruction grid"), &settings.ReconstructionGridSize,
						kReconstructionGridLabels.data(), kReconstructionGridSizes, 1) &&
					!SettingsControl::IsReplicating())
					SyncRuntimeSettings();
				ImGui::EndDisabled();
				if (ImGui::SliderFloat(T("menu.dx12.composite.shadow_strength", "Shadow strength"), &settings.ShadowStrength, 0.0f, 4.0f, "%.2f") &&
					!SettingsControl::IsReplicating())
					SyncRuntimeSettings();
				if (ImGui::SliderFloat(T("menu.dx12.composite.shadow_blend", "Shadow blend"), &settings.ShadowBlend, 0.0f, 1.0f, "%.2f") &&
					!SettingsControl::IsReplicating())
					SyncRuntimeSettings();
				ImGui::TextDisabled(T("menu.dx12.composite.before_upscaling", "Reconstruction and shadow application run before native upscaling."));
				break;
			}
		case 3:
			DrawMasterEnableCheckbox();
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
	settings = input;
	SyncRuntimeSettings();
	bootSnapshot.LatchIfNeeded(settings);
	if (!bootCaptured) {
		bootActive.store(settings.Enabled, std::memory_order_release);
		bootCaptured = true;
	}
}

void DX12Control::SaveSettings(json& output)
{
	output = settings;
}

void DX12Control::RestoreDefaultSettings()
{
	settings = {};
	SyncRuntimeSettings();
}

void DX12Control::OnRuntimeDisabled()
{
	if (!bootActive.exchange(false, std::memory_order_acq_rel))
		return;
	globals::features::dx12NativeScene.OnRuntimeDisabled();
	globals::features::dx12Resources.OnRuntimeDisabled();
}
