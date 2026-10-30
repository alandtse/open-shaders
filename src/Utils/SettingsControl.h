#pragma once

#include <compare>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class Feature;
namespace SceneSettingsCatalog
{
	struct SettingMetadata;
}

namespace SettingsControl
{
	/** @brief Identifies a persisted setting independently of its UI host. */
	struct SettingIdentity
	{
		std::string featureShortName;
		std::vector<std::string> settingPath;
		std::string settingKey;
		auto operator<=>(const SettingIdentity&) const = default;
	};

	/** @brief Associates a displayed temporary with its persisted member. */
	struct Proxy
	{
		/// The settings member the control stands for, which is what the catalog resolves.
		const void* member = nullptr;
		/// Temporary value = member value * displayScale, matching the catalog's displayScale.
		float displayScale = 1.0f;
	};

	/** @brief Scopes a temporary-to-settings binding for a custom control. */
	class ProxyScope
	{
	public:
		ProxyScope(const void* member, float displayScale);
		~ProxyScope();
		ProxyScope(const ProxyScope&) = delete;
		ProxyScope& operator=(const ProxyScope&) = delete;

	private:
		Proxy previous;
	};

	using PathValidator = std::function<std::string(const std::filesystem::path&)>;
	using IntDrawer = std::function<bool(int&)>;
	/** @brief Optional control adapters supplied by the active settings UI integration. */
	struct Callbacks
	{
		bool (*isReplicating)() = nullptr;
		bool (*interceptInt)(const char*, int&, const IntDrawer&) = nullptr;
		bool (*drawPath)(const SettingIdentity&, std::string&, std::string&, const PathValidator&) = nullptr;
		void (*recordBaselineEdit)(const SettingIdentity&, const nlohmann::json&) = nullptr;
	};

	/** @brief Installs the settings UI adapters on the UI thread. */
	void SetCallbacks(const Callbacks& callbacks);
	/** @brief Optional actions for catalogued controls, independent of their UI host. */
	struct Actions
	{
		void (*drawContextMenu)(Feature&, const SceneSettingsCatalog::SettingMetadata&, bool disabled) = nullptr;
		bool (*isHighlighted)(const SceneSettingsCatalog::SettingMetadata&) = nullptr;
	};
	/** @brief Installs optional control actions during feature initialization. */
	void SetActions(const Actions& actions);
	/** @brief Draws registered actions inside the control's context menu. */
	void DrawContextActions(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, bool disabled);
	/** @brief Whether the control has a pending registered action. */
	bool IsHighlighted(const SceneSettingsCatalog::SettingMetadata& setting);
	/** @brief Returns the current temporary binding, or null when none is scoped. */
	const Proxy* GetProxy();
	/** @brief Returns whether controls are being drawn for a settings replica. */
	bool IsReplicating();
	/** @brief Draws a custom integer control through the optional settings adapter. */
	bool InterceptInt(const char* label, int& value, const IntDrawer& draw);
	/** @brief Returns true when the settings adapter supplies a path control. */
	bool DrawPathControls(const SettingIdentity& setting, std::string& path, std::string& error, const PathValidator& validate);
	/** @brief Records a custom control's edited persisted value. */
	void RecordBaselineEdit(const SettingIdentity& setting, const nlohmann::json& value);
}
