#include "DX12Resources.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DX12Resources::Settings, Enabled)

void DX12Resources::DrawSettings()
{
    ImGui::Checkbox("Enable manual DX12 Resources preparation", &settings.Enabled);
    ImGui::TextDisabled("Independent from DX12 Native Scene and legacy True PBR.");
    ImGui::BeginDisabled(!settings.Enabled);
    ImGui::InputTextWithHint("Source plugin", "Skyrim.esm", pluginText.data(), pluginText.size());
    ImGui::InputTextWithHint("Local CELL FormID (hex)", "000001", localIdText.data(),
        localIdText.size(), ImGuiInputTextFlags_CharsHexadecimal);
    const auto request = DX12ResourceService::ParseCellRequest(
        pluginText.data(), localIdText.data());
    if (request)
        ImGui::Text("Cell identity: %s", request->Key().c_str());
    else
        ImGui::TextDisabled("Enter an exact source plugin and local CELL FormID.");
    ImGui::TextDisabled("Only closed interior cells. No automatic save or cell prediction.");
    ImGui::BeginDisabled();
    ImGui::Button("Prepare Selected Cell");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Preparation backend will be connected in the next stage.");
    ImGui::EndDisabled();
}

void DX12Resources::LoadSettings(json& input)
{
    settings = input.get<Settings>();
}
void DX12Resources::SaveSettings(json& output) { output = settings; }
void DX12Resources::RestoreDefaultSettings() { settings = {}; }
json DX12Resources::GetDiagnostics()
{
    return { { "enabled", settings.Enabled }, { "dataLoaded", dataLoaded },
             { "stage", "manual-cell-selection" }, { "prepared", false } };
}
