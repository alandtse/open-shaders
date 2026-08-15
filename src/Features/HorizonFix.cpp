#include "HorizonFix.h"

#include <imgui.h>

bool HorizonFix::IsInMenu() const
{
	return loaded || companionPluginDetected;
}

void HorizonFix::DrawSettings()
{
	ImGui::TextWrapped(
		"This feature provides compatibility with the Horizon Fix SKSE plugin, which extends the water far clip plane to allow water to be rendered beyond the vanilla far clip distance. This feature is only active when the Horizon Fix plugin is installed.");
}

void HorizonFix::PostPostLoad()
{
	// The shader-side far-water support is only wanted while the HorizonFix plugin is
	// actually installed; without it water keeps the vanilla far-clip look. Checked here
	// because every SKSE plugin has loaded by now and the shader disk cache has not been
	// validated yet, so installing or removing the plugin invalidates the cache through
	// regular feature validation.
	companionPluginDetected = GetModuleHandleW(L"HorizonFix.dll") != nullptr;
	if (loaded && !companionPluginDetected) {
		// No companion plugin is an expected, benign disable, not a load failure --
		// leave failedLoadedMessage unset so the cache classifier treats it as such.
		loaded = false;
		logger::info("[Horizon Fix] HorizonFix plugin not detected, compatibility disabled");
	} else {
		logger::info("[Horizon Fix] HorizonFix plugin detected, compatibility enabled");
	}
}
