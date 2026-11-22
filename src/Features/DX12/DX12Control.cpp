#include "DX12Control.h"
#include "../Upscaling.h"
#include "DX12NativeScene.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "State.h"
#include "Utils/UI.h"

#include <algorithm>

void DX12Control::DrawSettings()
{
	// Keep pipeline mode directly below the DX12 heading and above subfeature tabs.
	static constexpr const char* modes[] = { "Shadow offload", "Raster comparison" };
	int mode = settings.Mode;
	if (ImGui::Combo("Mode", &mode, modes, int(std::size(modes)))) {
		settings.Mode = mode;
		selectedMode.store(mode, std::memory_order_relaxed);
	}
	ImGui::TextDisabled(settings.Mode == 0 ?
							"Native shadows replace matched raster static shadows." :
							"Raster shadows remain unchanged; native shadow dispatch is paused for comparison.");

	if (!IsActive()) {
		ImGui::Checkbox("Enable native DX12 features (restart required)", &settings.Enabled);
		if (settings.Enabled)
			ImGui::TextDisabled("Restart the game to initialize DX12 Resources.");
		return;
	}
	if (!Util::BeginPillTabBar("##DX12Tabs"))
		return;
	const bool devMode = globals::state && globals::state->IsDeveloperMode();
	const char* labels[] = { "Resources", "Native Scene", "Native Composite", "Master Switch", "Dev" };
	for (int index = 0; index < (devMode ? 5 : 4); ++index) {
		if (!Util::BeginPillTabItem(labels[index]))
			continue;
		selectedTab = index;
		switch (index) {
		case 0:
			globals::features::dx12Resources.DrawSettings();
			break;
		case 1:
			globals::features::dx12NativeScene.DrawSettings();
			break;
		case 2:
			{
				ImGui::Checkbox("Spatial shadow reconstruction", &settings.SpatialReconstruction);
				spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
				int gridIndex = int((settings.ReconstructionGridSize - 3u) / 2u);
				ImGui::BeginDisabled(!settings.SpatialReconstruction);
				if (ImGui::Combo("Reconstruction grid", &gridIndex,
						"3x3\0"
						"5x5\0"
						"7x7\0"
						"9x9\0")) {
					settings.ReconstructionGridSize = uint32_t(3 + gridIndex * 2);
					reconstructionGrid.store(settings.ReconstructionGridSize, std::memory_order_relaxed);
				}
				ImGui::EndDisabled();
				if (ImGui::SliderFloat("Shadow strength", &settings.ShadowStrength, 0.0f, 4.0f, "%.2f"))
					shadowStrength.store(settings.ShadowStrength, std::memory_order_relaxed);
				if (ImGui::SliderFloat("Shadow blend", &settings.ShadowBlend, 0.0f, 1.0f, "%.2f"))
					shadowBlend.store(settings.ShadowBlend, std::memory_order_relaxed);
				ImGui::TextDisabled("Reconstruction and shadow application run before native upscaling.");
				break;
			}
		case 3:
			ImGui::Checkbox("Enable native DX12 features (restart required)", &settings.Enabled);
			if (settings.Enabled != bootActive.load(std::memory_order_acquire))
				ImGui::TextDisabled("A game restart is required; DX12 Resources retains its boot state until then.");
			break;
		case 4:
			{
				static constexpr const char* previewNames[] = { "Off", "Raw shadowmask", "Reconstructed shadowmask",
					"DX12 geometry (all)", "DX12 geometry (companion)", "DX12 geometry (runtime)" };
				int preview = int(shadowPreview.load(std::memory_order_relaxed));
				if (ImGui::Combo("Shadowmask preview", &preview, previewNames, int(std::size(previewNames))))
					shadowPreview.store(uint32_t(preview), std::memory_order_relaxed);
				ImGui::TextDisabled("Raw: magenta = density omitted; red = invalid receiver; blue = no applicable light.");
				ImGui::TextDisabled("Reconstructed: orange = no valid neighborhood. Full raw density cannot omit pixels.");
				ImGui::TextDisabled("DX12 geometry: cyan = companion, amber = runtime; dim = no TLAS hit.");
				ImGui::TextDisabled("Preview traces published TLAS triangles, not missing-NIF placeholders.");
				ImGui::TextDisabled("Visualizations use Native Composite -> FSR; non-FSR paths are unchanged.");
				globals::features::dx12NativeScene.DrawDiagnostics();
				if (ImGui::CollapsingHeader("Companion / Geometry (details)"))
					globals::features::dx12Resources.DrawDiagnostics();
				ImGui::SeparatorText("Native Composite / DXGI budget");
				ImGui::Text("Spatial reconstruction: %s (%ux%u)", UseSpatialReconstruction() ? "enabled" : "disabled", GetReconstructionGridSize(), GetReconstructionGridSize());
				ImGui::Text("Shadow strength %.2f, blend %.2f", GetShadowStrength(), GetShadowBlend());
				const auto memory = globals::features::upscaling.nativeComposite.GetMemoryBudgetSnapshot();
				ImGui::Text("DXGI local budget: %s", memory.dxgiAvailable ? "available (last allocation query)" : "not queried / unavailable");
				if (memory.dxgiAvailable) {
					constexpr double mib = 1024.0 * 1024.0;
					ImGui::Text("Budget %.1f MiB, usage %.1f MiB, headroom %.1f MiB",
						double(memory.localBudget) / mib, double(memory.localUsage) / mib, double(memory.headroom) / mib);
					ImGui::Text("Reserve %.1f MiB, effective allocation allowance %.1f MiB",
						double(memory.reserve) / mib, double(memory.effectiveBytes) / mib);
				}
				break;
			}
		}
		Util::EndPillTabItem();
	}
	Util::EndPillTabBar();
}

uint32_t DX12Control::GetShadowPreview() const
{
	// Leaving the main-menu Developer Mode immediately disables all debug
	// visualization, even if a preview was previously selected in this tab.
	return globals::state && globals::state->IsDeveloperMode() ?
	           shadowPreview.load(std::memory_order_relaxed) :
	           0u;
}

void DX12Control::LoadSettings(json& input)
{
	settings.Enabled = input.value("Enabled", false);
	settings.Mode = std::clamp(input.value("Mode", 0), 0, 1);
	settings.SpatialReconstruction = input.value("SpatialReconstruction", true);
	const uint32_t requestedGrid = std::clamp(input.value("ReconstructionGridSize", 7u), 3u, 9u);
	settings.ReconstructionGridSize = requestedGrid - ((requestedGrid & 1u) == 0u ? 1u : 0u);
	settings.ShadowStrength = std::clamp(input.value("ShadowStrength", 2.0f), 0.0f, 4.0f);
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
	bootActive.store(false, std::memory_order_release);
}
