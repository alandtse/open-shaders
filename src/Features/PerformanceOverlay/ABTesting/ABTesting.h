#pragma once
#include "ABTestAggregator.h"
#include "Utils/FileSystem.h"
#include <nlohmann/json.hpp>
#include <vector>

// A/B Testing Manager - handles the overall A/B testing system
class ABTestingManager
{
public:
	static ABTestingManager* GetSingleton();

	// Configuration
	void SetTestInterval(uint32_t interval);
	uint32_t GetTestInterval() const { return testInterval; }
	bool IsEnabled() const { return abTestingEnabled; }
	bool IsUsingTestConfig() const { return usingTestConfig; }

	// State management
	void Enable();
	void Disable();
	void Update();  // Called each frame to handle timing and switching

	// Manual switching: in manual mode Update() does not auto-swap on the timer; the caller
	// drives swaps via SwitchVariant(). Lets an external driver (devbench) align variant
	// switches to whole benchmark passes instead of a fixed interval. Must run on the main
	// thread (SwitchVariant loads config the render thread reads).
	void SetManualMode(bool a_manual) { manualMode = a_manual; }
	bool IsManualMode() const { return manualMode; }
	void SwitchVariant();

	// UI
	void DrawSettingsUI();  // The A/B test interval slider
	void DrawOverlayUI();   // The "Variant X: Y seconds left" overlay

	// Access to aggregator
	ABTestAggregator& GetAggregator() { return aggregator; }

	// Access to cached settings for performance overlay (in-memory, no disk I/O)
	bool HasCachedSnapshots() const { return hasTestSnapshot && hasUserSnapshot; }
	const nlohmann::json& GetUserSnapshot() const { return userConfigSnapshot; }
	const nlohmann::json& GetTestSnapshot() const { return testConfigSnapshot; }
	std::vector<std::string> GetConfigDifferencesForDisplay() const;
	std::vector<SettingsDiffEntry> GetConfigDiffEntries(float epsilon = 0.0001f) const;

	// Clear cached snapshots explicitly (used when overlay results are cleared)
	void ClearCachedSnapshots();

private:
	uint32_t testInterval = 0;
	bool abTestingEnabled = false;
	bool usingTestConfig = false;
	bool manualMode = false;  // when true, Update() skips the timer swap (caller drives it)
	LARGE_INTEGER timingFrequency = { 0 };
	LARGE_INTEGER lastTestSwitch = { 0 };
	ABTestAggregator aggregator;

	// In-memory storage for both variants to avoid disk I/O during swapping
	nlohmann::json testConfigSnapshot;
	nlohmann::json userConfigSnapshot;
	bool hasTestSnapshot = false;
	bool hasUserSnapshot = false;

	// Track what changed between USER and TEST configs
	std::vector<std::string> GetConfigDifferences() const;

	// The variant swap (load the other snapshot + notify the aggregator), shared by the
	// timer path in Update() and the manual SwitchVariant(). Must run on the main thread.
	void SwapVariant();
};