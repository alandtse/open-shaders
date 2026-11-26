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
	Mode)

void DX12Control::SyncRuntimeSettings() noexcept
{
	settings.Mode = static_cast<int>(DX12::SanitizeControlMode(settings.Mode, globals::state && globals::state->IsDeveloperMode()));
	selectedMode.store(settings.Mode, std::memory_order_relaxed);
}

DX12::ControlMode DX12Control::GetMode() const
{
	using DX12::ControlMode;
	int mode = selectedMode.load(std::memory_order_relaxed);
	// Check the existing developer gate only for the diagnostic mode.
	if (mode == static_cast<int>(ControlMode::RasterComparison) &&
		(!globals::state || !globals::state->IsDeveloperMode())) {
		const int fallback = static_cast<int>(ControlMode::ShadowOffload);
		if (selectedMode.compare_exchange_strong(mode, fallback, std::memory_order_relaxed))
			mode = fallback;
	}
	return static_cast<ControlMode>(mode);
}

void DX12Control::DrawSimpleSettings(bool developerMode)
{
	using DX12::ControlMode;
	// Items follow the persisted ControlMode values, so item i is mode i.
	const char* modes[] = {
		T("menu.dx12.mode.shadow_offload", "Shadow offload"),
		T("menu.dx12.mode.raster_comparison", "Raster comparison"),
		T("menu.dx12.mode.ray_traced_static_shadows", "Ray-Traced Static Shadows")
	};
	const char* playerModes[] = { modes[0], modes[2] };
	constexpr std::array kPlayerModeValues{ 0, 2 };
	static_assert(kPlayerModeValues[1] == static_cast<int>(ControlMode::FullRT));
	settings.Mode = static_cast<int>(DX12::SanitizeControlMode(static_cast<int>(GetMode()), developerMode));
	const char* label = T("menu.dx12.mode.label", "Mode");
	const bool picked = developerMode ? Util::Combo(label, &settings.Mode, modes) :
	                                    Util::Combo(label, &settings.Mode, playerModes, kPlayerModeValues);
	if (picked && !SettingsControl::IsReplicating())
		SyncRuntimeSettings();
	ImGui::Checkbox(T("menu.dx12.master.enable", "Enable native DX12 features (restart required)"), &settings.Enabled);
	if (settings.Enabled != bootActive.load(std::memory_order_acquire))
		ImGui::TextDisabled(T("menu.dx12.master.restart_boot_state", "A game restart is required; DX12 Resources retains its boot state until then."));
	if (!IsActive())
		return;

	ImGui::SeparatorText(T("menu.dx12.tab.native_scene", "Native Scene"));
	globals::features::dx12NativeScene.DrawSettings();
	ImGui::SeparatorText(T("menu.dx12.tab.resources", "Resources"));
	globals::features::dx12Resources.DrawSettings();
}

void DX12Control::DrawSettings()
{
	const bool developerMode = globals::state && globals::state->IsDeveloperMode();
	if (!Util::BeginPillTabBar("##DX12Tabs"))
		return;
	if (Util::BeginPillTabItem(T("menu.dx12.tab.resources", "Resources"))) {
		if (IsActive())
			globals::features::dx12Resources.DrawResourceStatus();
		else
			ImGui::TextDisabled("%s", T("menu.dx12.resources.inactive", "DX12 Resources is inactive."));
		Util::EndPillTabItem();
	}
	if (Util::BeginPillTabItem(T("menu.dx12.tab.settings", "Settings"))) {
		DrawSimpleSettings(developerMode);
		Util::EndPillTabItem();
	}
	if (IsActive() && developerMode && Util::BeginPillTabItem(T("menu.dx12.tab.dev", "Dev"))) {
		globals::features::dx12NativeScene.DrawDevSettings();
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
	settings.Mode = static_cast<int>(GetMode());
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
