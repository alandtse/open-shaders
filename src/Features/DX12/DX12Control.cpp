#include "DX12Control.h"

void DX12Control::DrawSettings()
{
	if (ImGui::Checkbox("Enable native DX12 features", &settings.Enabled))
		active.store(settings.Enabled, std::memory_order_release);
	ImGui::TextWrapped("Native geometry, shadow offloading, and final composite are governed by this switch. Legacy rendering remains active when disabled.");
}

void DX12Control::LoadSettings(json& input)
{
	settings.Enabled = input.value("Enabled", false);
	active.store(settings.Enabled, std::memory_order_release);
}

void DX12Control::SaveSettings(json& output)
{
	output = { { "Enabled", settings.Enabled } };
}

void DX12Control::RestoreDefaultSettings()
{
	settings = {};
	active.store(false, std::memory_order_release);
}

void DX12Control::OnRuntimeDisabled()
{
	active.store(false, std::memory_order_release);
}
