#include "DX12Control.h"
#include "DX12Resources.h"
#include "DX12NativeScene.h"
#include "Globals.h"
#include "Utils/UI.h"

void DX12Control::DrawSettings()
{
	// The boot-latched master is the only visible control when DX12 was off at startup.
	if (!IsActive()) {
		ImGui::Checkbox("Enable native DX12 features (restart required)", &settings.Enabled);
		if (settings.Enabled)
			ImGui::TextDisabled("Restart the game to initialize DX12 Resources.");
		return;
	}
	if (!Util::BeginPillTabBar("##DX12Tabs"))
		return;
	// Persistent choice between openings of the settings page; the active selection is
	// tracked by the pill-tab widget itself while this control stays loaded.
	const char* labels[] = { "Resources", "Native Scene", "Master Switch" };
	for (int index = 0; index < 3; ++index) {
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
	if (!bootCaptured) {
		bootActive.store(settings.Enabled, std::memory_order_release);
		bootCaptured = true;
	}
}

void DX12Control::SaveSettings(json& output)
{
	output = { { "Enabled", settings.Enabled } };
}

void DX12Control::RestoreDefaultSettings()
{
	settings = {};
}

void DX12Control::OnRuntimeDisabled()
{
	bootActive.store(false, std::memory_order_release);
}
