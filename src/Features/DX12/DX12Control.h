#pragma once

#include "DX12ControlMode.h"
#include "Feature.h"
#include "Utils/BootSnapshot.h"

#include <atomic>

/** @brief Session-wide opt-in for DX12 Resources and future native features. */
struct DX12Control final : Feature
{
	struct Settings
	{
		bool Enabled = false;
		int Mode = static_cast<int>(DX12::ControlMode::ShadowOffload);
	};

	Settings settings;

	inline static constexpr Util::Settings::RestartTable<Settings, 1> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, Enabled, "Enable native DX12 features"),
	} };
	Util::Settings::BootSnapshot<Settings> bootSnapshot{ kRestartFields };

	std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const override
	{
		return { kRestartFields.data(), kRestartFields.size() };
	}
	const void* GetBootValue(std::string_view jsonKey) const override { return bootSnapshot.RawBoot(jsonKey); }
	const void* GetSettingsBlob() const override { return &settings; }
	size_t GetSettingsBlobSize() const override { return sizeof(settings); }

	std::string GetName() override { return "DX12"; }
	std::string GetShortName() override { return "DX12"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	// The master checkbox is the only DX12 activation control; do not offer a
	// separate Disable-at-Boot switch that can silently override it.
	bool IsAlwaysEnabled() const override { return true; }
	bool SupportsVR() override { return false; }
	/** @brief Whether native work may be submitted this frame. */
	bool IsActive() const { return loaded && bootActive.load(std::memory_order_acquire); }
	bool IsShadowOffloadMode() const { return GetMode() == DX12::ControlMode::ShadowOffload; }
	bool IsFullRTMode() const { return GetMode() == DX12::ControlMode::FullRT; }
	void DrawSettings() override;
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	void OnRuntimeDisabled() override;

private:
	void SyncRuntimeSettings() noexcept;
	std::atomic<bool> bootActive{ false };
	bool bootCaptured = false;
	DX12::ControlMode GetMode() const;
	void DrawSimpleSettings(bool developerMode);
	mutable std::atomic<int> selectedMode{ static_cast<int>(DX12::ControlMode::ShadowOffload) };
};
