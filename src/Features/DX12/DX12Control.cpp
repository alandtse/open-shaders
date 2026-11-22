#include "DX12Control.h"
#include "DX12NativeScene.h"
#include "DX12Resources.h"
#include "Globals.h"
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
	const char* labels[] = { "Resources", "Native Scene", "Native Composite", "Master Switch" };
	for (int index = 0; index < 4; ++index) {
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
			ImGui::Checkbox("Spatial shadow reconstruction", &settings.SpatialReconstruction);
			spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
			if (ImGui::SliderFloat("Shadow strength", &settings.ShadowStrength, 0.0f, 4.0f, "%.2f"))
				shadowStrength.store(settings.ShadowStrength, std::memory_order_relaxed);
			if (ImGui::SliderFloat("Shadow blend", &settings.ShadowBlend, 0.0f, 1.0f, "%.2f"))
				shadowBlend.store(settings.ShadowBlend, std::memory_order_relaxed);
			ImGui::TextDisabled("Reconstruction and shadow application run before native upscaling.");
			break;
		case 3:
			ImGui::Checkbox("Enable native DX12 features (restart required)", &settings.Enabled);
			if (settings.Enabled != bootActive.load(std::memory_order_acquire))
				ImGui::TextDisabled("A game restart is required; DX12 Resources retains its boot state until then.");
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
	settings.ShadowStrength = std::clamp(input.value("ShadowStrength", 2.0f), 0.0f, 4.0f);
	settings.ShadowBlend = std::clamp(input.value("ShadowBlend", 0.70f), 0.0f, 1.0f);
	selectedMode.store(settings.Mode, std::memory_order_relaxed);
	spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
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
		{ "ShadowStrength", settings.ShadowStrength }, { "ShadowBlend", settings.ShadowBlend } };
}

void DX12Control::RestoreDefaultSettings()
{
	settings = {};
	selectedMode.store(settings.Mode, std::memory_order_relaxed);
	spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
	shadowStrength.store(settings.ShadowStrength, std::memory_order_relaxed);
	shadowBlend.store(settings.ShadowBlend, std::memory_order_relaxed);
}

void DX12Control::OnRuntimeDisabled()
{
	bootActive.store(false, std::memory_order_release);
}
