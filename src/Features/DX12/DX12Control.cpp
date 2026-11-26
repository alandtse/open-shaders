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
	const char* modes[] = { T("menu.dx12.mode.shadow_offload", "Shadow offload"), T("menu.dx12.mode.raster_comparison", "Raster comparison"), T("menu.dx12.mode.ray_traced_static_shadows", "Ray-Traced Static Shadows") };
	int mode = settings.Mode;
	if (ImGui::Combo(T("menu.dx12.mode.label", "Mode"), &mode, modes, int(std::size(modes)))) {
		settings.Mode = mode;
		selectedMode.store(mode, std::memory_order_relaxed);
	}


	if (!IsActive()) {
		ImGui::Checkbox(T("menu.dx12.master.enable", "Enable native DX12 features (restart required)"), &settings.Enabled);
		if (settings.Enabled)
			ImGui::TextDisabled(T("menu.dx12.master.restart_resources", "Restart the game to initialize DX12 Resources."));
		return;
	}
	if (!Util::BeginPillTabBar("##DX12Tabs"))
		return;
	const char* labels[] = { T("menu.dx12.tab.resources", "Resources"), T("menu.dx12.tab.native_scene", "Native Scene"), T("menu.dx12.tab.master_switch", "Master Switch") };
	for (int index = 0; index < int(std::size(labels)); ++index) {
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
	settings.Mode = std::clamp(input.value("Mode", 0), 0, 2);
	selectedMode.store(settings.Mode, std::memory_order_relaxed);
	if (!bootCaptured) {
		bootActive.store(settings.Enabled, std::memory_order_release);
		bootCaptured = true;
	}
}

void DX12Control::SaveSettings(json& output)
{
	output = { { "Enabled", settings.Enabled }, { "Mode", settings.Mode } };
}

void DX12Control::RestoreDefaultSettings()
{
	settings = {};
	selectedMode.store(settings.Mode, std::memory_order_relaxed);
}

void DX12Control::OnRuntimeDisabled()
{
	if (!bootActive.exchange(false, std::memory_order_acq_rel))
		return;
	globals::features::dx12NativeScene.OnRuntimeDisabled();
	globals::features::dx12Resources.OnRuntimeDisabled();
}
