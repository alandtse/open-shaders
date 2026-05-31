#include "ABTesting.h"
#include "Features/PerformanceOverlay.h"
#include "Menu.h"
#include "Menu/ThemeManager.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/UI.h"
#include <cmath>
#include <fmt/format.h>
#include <fstream>
#include <imgui.h>

ABTestingManager* ABTestingManager::GetSingleton()
{
	static ABTestingManager singleton;
	return &singleton;
}

void ABTestingManager::SetTestInterval(uint32_t interval)
{
	testInterval = interval;
}

void ABTestingManager::Enable()
{
	if (!abTestingEnabled) {
		auto* state = globals::state;
		auto& performanceOverlay = globals::features::performanceOverlay;

		logger::info("Starting A/B testing with current settings as Variant B (TEST), interval {} seconds.", testInterval);

		// Preserve overlay enabled state before config operations
		bool overlayWasEnabled = performanceOverlay.settings.ShowInOverlay;

		// Save current settings as TEST variant (Variant B) in memory
		testConfigSnapshot = nlohmann::json::object();
		state->SaveToJson(testConfigSnapshot);
		hasTestSnapshot = true;

		// Load and cache USER settings in memory (to avoid disk I/O during swaps)
		userConfigSnapshot = nlohmann::json::object();
		state->Load(State::ConfigMode::USER);
		state->SaveToJson(userConfigSnapshot);
		hasUserSnapshot = true;

		// Load TEST variant to start with (user's configured test settings)
		state->LoadFromJson(testConfigSnapshot);
		usingTestConfig = true;

		abTestingEnabled = true;

		// Initialize QueryPerformanceCounter timing
		if (timingFrequency.QuadPart == 0) {
			QueryPerformanceFrequency(&timingFrequency);
		}
		QueryPerformanceCounter(&lastTestSwitch);

		// Restore overlay enabled state after config operations
		performanceOverlay.settings.ShowInOverlay = overlayWasEnabled;

		// Manual/benchmark mode needs the draw-call profiling running so the aggregator
		// collects per-variant timing — that only happens while the overlay is shown. Force it
		// on (remembering we did) and restore on Disable.
		if (manualMode && !performanceOverlay.settings.ShowInOverlay) {
			overlayForcedByManual = true;
			performanceOverlay.settings.ShowInOverlay = true;
		}

		logger::info("A/B Testing enabled - starting with Variant B (TEST). Both variants cached in memory for unbiased swapping.");
	}
}

void ABTestingManager::Disable()
{
	if (abTestingEnabled) {
		auto* state = globals::state;
		auto& performanceOverlay = globals::features::performanceOverlay;

		// Preserve overlay enabled state
		bool overlayWasEnabled = performanceOverlay.settings.ShowInOverlay;

		logger::info("Disabling A/B testing. Restoring to Variant B (TEST) config from memory.");

		// Restore TEST config from memory snapshot (no disk read)
		if (hasTestSnapshot) {
			state->LoadFromJson(testConfigSnapshot);
		} else {
			logger::warn("No TEST snapshot available, staying with current config.");
		}

		abTestingEnabled = false;
		manualMode = false;  // reset so a later UI-driven test rotates on the timer again

		performanceOverlay.settings.ShowInOverlay = overlayWasEnabled;

		// Undo a manual-mode overlay force so we don't leave it on for the user.
		if (overlayForcedByManual) {
			performanceOverlay.settings.ShowInOverlay = false;
			overlayForcedByManual = false;
		}
	}
}

void ABTestingManager::SwapVariant()
{
	auto* state = globals::state;
	auto& performanceOverlay = globals::features::performanceOverlay;
	bool overlayWasEnabled = performanceOverlay.settings.ShowInOverlay;

	// Swap between variants (in-memory snapshots, no disk I/O)
	usingTestConfig = !usingTestConfig;
	logger::info("A/B Test swap to {} (from memory snapshot)",
		usingTestConfig ? "Variant B (TEST)" : "Variant A (USER)");

	if (usingTestConfig) {
		if (hasTestSnapshot) {
			state->LoadFromJson(testConfigSnapshot);
		} else {
			logger::error("TEST snapshot missing! Cannot swap to Variant B.");
			usingTestConfig = false;  // Stay on USER
		}
	} else {
		if (hasUserSnapshot) {
			state->LoadFromJson(userConfigSnapshot);
		} else {
			logger::error("USER snapshot missing! Cannot swap to Variant A.");
			usingTestConfig = true;  // Stay on TEST
		}
	}

	performanceOverlay.settings.ShowInOverlay = overlayWasEnabled;
	QueryPerformanceCounter(&lastTestSwitch);

	// Notify the A/B test aggregator of the variant switch
	aggregator.OnABSwitch(usingTestConfig ? ABVariant::B : ABVariant::A);
}

void ABTestingManager::Update()
{
	// Manual mode: the caller (e.g. devbench) drives swaps via SwitchVariant() so they align
	// to whole benchmark passes — skip the timer swap.
	if (!abTestingEnabled || manualMode)
		return;

	LARGE_INTEGER currentTime;
	QueryPerformanceCounter(&currentTime);
	float seconds = (currentTime.QuadPart - lastTestSwitch.QuadPart) / static_cast<float>(timingFrequency.QuadPart);
	if (static_cast<float>(testInterval) - seconds < 0.0f)
		SwapVariant();
}

void ABTestingManager::SwitchVariant()
{
	if (!abTestingEnabled) {
		logger::warn("A/B SwitchVariant ignored — A/B testing is not enabled.");
		return;
	}
	SwapVariant();
}

void ABTestingManager::DrawSettingsUI()
{
	auto& performanceOverlay = globals::features::performanceOverlay;

	if (ImGui::SliderInt("A/B Test Interval", reinterpret_cast<int*>(&testInterval), 0, 10)) {
		bool overlayWasEnabled = performanceOverlay.settings.ShowInOverlay;
		if (testInterval == 0) {
			Disable();
		} else if (!abTestingEnabled) {
			Enable();
		} else {
			logger::info("Setting new A/B test interval {}.", testInterval);
		}
		performanceOverlay.settings.ShowInOverlay = overlayWasEnabled;
	}

	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"A/B Testing compares two configurations by automatically swapping between them.\n"
			"Workflow: Configure your test settings, then enable A/B testing.\n"
			"- Variant B (TEST) = Your current settings when you enable testing\n"
			"- Variant A (USER) = Your previously saved user configuration\n"
			"Testing starts with Variant B, then swaps every N seconds.\n"
			"Set to 0 to disable and restore TEST settings.");
	}
}

std::vector<std::string> ABTestingManager::GetConfigDifferencesForDisplay() const
{
	std::vector<std::string> differences;

	if (!hasTestSnapshot || !hasUserSnapshot)
		return differences;

	auto diffEntries = Util::FileSystem::DiffJson(userConfigSnapshot, testConfigSnapshot, 0.0001f);

	// Format diff entries for display
	for (const auto& entry : diffEntries) {
		std::string path = entry.path;
		std::string aVal = entry.aValue;
		std::string bVal = entry.bValue;

		// Clean up JSON path (remove leading slash and simplify)
		if (!path.empty() && path[0] == '/') {
			path = path.substr(1);
			// Replace slashes with dots for readability
			std::replace(path.begin(), path.end(), '/', '.');
		}

		// Skip version changes (not relevant to user)
		if (path == "Version")
			continue;

		// Truncate long values
		if (aVal.length() > 30)
			aVal = aVal.substr(0, 27) + "...";
		if (bVal.length() > 30)
			bVal = bVal.substr(0, 27) + "...";

		// Format: "path: oldValue -> newValue"
		differences.push_back(fmt::format("{}: {} -> {}", path, aVal, bVal));
	}

	return differences;
}

std::vector<SettingsDiffEntry> ABTestingManager::GetConfigDiffEntries(float epsilon) const
{
	if (!hasTestSnapshot || !hasUserSnapshot)
		return {};

	return Util::FileSystem::DiffJson(userConfigSnapshot, testConfigSnapshot, epsilon);
}

void ABTestingManager::ClearCachedSnapshots()
{
	try {
		testConfigSnapshot.clear();
		userConfigSnapshot.clear();
		hasTestSnapshot = false;
		hasUserSnapshot = false;
	} catch (...) {
		// No-op if clear fails
	}
}

std::vector<std::string> ABTestingManager::GetConfigDifferences() const
{
	// Keep this for DrawOverlayUI to avoid circular dependencies
	return GetConfigDifferencesForDisplay();
}

void ABTestingManager::DrawOverlayUI()
{
	if (!abTestingEnabled)
		return;

	LARGE_INTEGER currentTime;
	QueryPerformanceCounter(&currentTime);
	float seconds = (currentTime.QuadPart - lastTestSwitch.QuadPart) / static_cast<float>(timingFrequency.QuadPart);
	auto remaining = static_cast<float>(testInterval) - seconds;

	// Scale position for resolution
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * Util::GetUIScale();
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGui::SetNextWindowPos(ImVec2(pos, pos));
	if (!ImGui::Begin("A/B Testing", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::End();
		return;
	}

	remaining = std::max(0.0f, remaining);

	// Show current variant and time
	ImGui::Text(fmt::format("{} : {:.1f}s left",
		usingTestConfig ? "Variant B (TEST)" : "Variant A (USER)", remaining)
			.c_str());

	// Show what changed (for both variants)
	if (hasTestSnapshot) {
		auto differences = GetConfigDifferences();

		if (!differences.empty()) {
			ImGui::Separator();

			constexpr size_t MAX_CHANGES_DISPLAYED = 10;  // Show max 10 individual changes, otherwise show count
			if (differences.size() <= MAX_CHANGES_DISPLAYED) {
				ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Changes from USER:");
				for (const auto& diff : differences) {
					ImGui::BulletText("%s", diff.c_str());
				}
			} else {
				ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
					"%zu settings changed", differences.size());
			}
		}
	}

	ImGui::End();
}