#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct Feature;

namespace MenuPageIds
{
	inline constexpr std::string_view kHome = "Home";
	inline constexpr std::string_view kPresets = "Presets";
	inline constexpr std::string_view kSettings = "Settings";
	/// Alias: opens Settings at its first General section (General and Advanced are part of Settings)
	inline constexpr std::string_view kGeneral = "General";
	/// Alias: opens Settings at its first Advanced section
	inline constexpr std::string_view kAdvanced = "Advanced";
	inline constexpr std::string_view kProfiling = "Performance";
	inline constexpr std::string_view kCSEditor = "CSEditor";
	/// Feature issues and active setting constraints; listed only while either has content
	inline constexpr std::string_view kIssues = "Issues";
	/// Alias of kIssues: Feature Issues is a section of the Issues page
	inline constexpr std::string_view kFeatureIssues = kIssues;
}

/**
 * @brief Filters of the main menu's feature list, stored as a bitmask in Menu::Settings::FeatureListFilter.
 *
 * Filters combine with AND; kUnloaded limits results to unloaded features.
 * The sort and filter popup is shared by the feature pane and Go to palette.
 */
namespace FeatureListFilter
{
	inline constexpr std::uint32_t kFavorites = 1u << 0;   ///< Pinned features
	inline constexpr std::uint32_t kModified = 1u << 1;    ///< Settings differ from defaults and Baseline presets
	inline constexpr std::uint32_t kScene = 1u << 2;       ///< Has Scene Manager settings
	inline constexpr std::uint32_t kPreRelease = 1u << 3;  ///< Alpha or beta
	inline constexpr std::uint32_t kUnloaded = 1u << 4;    ///< Not loaded: missing, failed, or disabled at boot
	/// Every filter; a saved mask is cut to these bits.
	inline constexpr std::uint32_t kKnown = kFavorites | kModified | kScene | kPreRelease | kUnloaded;
}

/**
 * @brief Renders the two-column feature list and settings panel in the main menu.
 *
 * The left column shows a searchable, categorized list of built-in pages and
 * installed features. The right column displays the settings UI for whichever
 * item is currently selected.
 */
class FeatureListRenderer
{
public:
	struct Hooks
	{
		int featureStep = 0;
		std::function<void(Feature*)> onFeatureShown;
	};

	/** @brief Attributes used by feature search filters. */
	static std::uint32_t GetFilterBits(Feature* feat);
	/** @brief Whether feature attributes satisfy the active filters. */
	static bool PassesFilter(std::uint32_t bits, std::uint32_t filter);
	/** @brief Shared sorting and filtering controls. */
	static void RenderSortPopup(const char* popupId);
	/** @brief Feature filters shared with the Go to palette. */
	static void RenderFilterPopup(const char* popupId);
	/** @brief Pins or unpins a loaded feature. */
	static void ToggleFavorite(Feature* feat);
	/** @brief Describes a built-in (non-feature) menu page with a name and draw callback. */
	struct BuiltInMenu
	{
		std::string name;
		std::function<void()> func;
		std::string canonicalId;
	};

	/** @brief Represents a collapsible category header in the feature list. */
	struct CategoryHeader
	{
		std::string name;
	};

	/** @brief Variant type representing any entry in the menu list. */
	using MenuFuncInfo = std::variant<BuiltInMenu, std::string, CategoryHeader, Feature*>;

	/**
	 * @brief Renders the full two-column feature list and settings panel.
	 *
	 * Builds the menu list from built-in pages and loaded features, handles
	 * pending feature selection requests, then draws the left-column navigation
	 * and right-column settings content.
	 *
	 * @param footerHeight Height reserved for the footer area below the list.
	 * @param selectedMenuId Stable identity of the selected page (updated on selection change).
	 * @param featureSearch Current search filter string (updated by the search input).
	 * @param pendingFeatureSelection Name of a feature to auto-select (cleared after processing).
	 * @param categoryExpansionStates Map of category name to expanded/collapsed state.
	 * @param drawSettings Callback that renders the General settings page content.
	 * @param drawSettingsNav Callback that renders the Advanced settings page content.
	 */
	static void RenderFeatureList(
		float footerHeight,
		std::string& selectedMenuId,
		std::string& featureSearch,
		std::string& pendingFeatureSelection,
		std::map<std::string, bool>& categoryExpansionStates,
		const std::function<void()>& drawSettings,
		const std::function<void()>& drawSettingsNav, bool editorLayout = false, bool sidebarVisible = true, bool resetLayout = false, Hooks* hooks = nullptr);

	/**
	 * @brief Draws a loaded feature's settings under the baseline interceptor scope and queues new constraint warnings.
	 * @param feat A loaded feature.
	 */
	static void DrawFeatureBody(Feature* feat);
	/** @brief Routes delayed setting information to the current feature page's action rail. */
	static void SetSettingHint(std::string message);

	/**
	 * @brief Draws the reactive constraint warning modal while one is pending; call once per frame, outside child windows.
	 * @param pendingFeatureSelection Receives the short name of a feature the user navigates to from the dialog.
	 */
	static void DrawConstraintWarningDialog(std::string& pendingFeatureSelection);

	/** @brief Translated display label for a FeatureCategories value. */
	static std::string TranslateCategory(std::string_view category);

private:
	static void RenderBuiltInRow(const BuiltInMenu& menu, size_t listId, size_t& selectedMenu, float width = 0.0f);
	struct ListMenuVisitor
	{
		size_t listId;
		size_t& selectedMenuRef;
		std::map<std::string, bool>& categoryExpansionStates;
		bool showVersions;

		void operator()(const BuiltInMenu& menu);
		void operator()(const std::string& label);
		void operator()(const CategoryHeader& header);
		void operator()(Feature* feat);
		void RenderRowContextMenu(Feature* feat) const;
	};

	struct DrawMenuVisitor
	{
		explicit DrawMenuVisitor(std::string& pendingFeatureSelectionRef, const Hooks* callbacks = nullptr) :
			pendingFeatureSelection(pendingFeatureSelectionRef), hooks(callbacks) {}

		void operator()(const BuiltInMenu& menu);
		void operator()(const std::string&);
		void operator()(const CategoryHeader&);
		void operator()(Feature* feat);

	private:
		std::string& pendingFeatureSelection;
		const Hooks* hooks;

		// Helper methods for Feature rendering
		void RenderFeatureActions(Feature* feat, bool isDisabled, bool isLoaded);
		void RenderFeatureHeader(Feature* feat, bool isLoaded);
		void RenderFeatureSettings(Feature* feat, bool isDisabled, bool isLoaded, bool hasFailedMessage);
		/** @brief Drops the feature's edits to settings its Baseline packs set. */
		static void RenderUseBaselinesButton(Feature* feat);
		static void RenderRestoreDefaultsButton(Feature* feat);
		static void RenderFavoriteButton(Feature* feat);
	};

	static std::vector<MenuFuncInfo> BuildMenuList(
		std::map<std::string, bool>& categoryExpansionStates,
		const std::function<void()>& drawSettings,
		const std::function<void()>& drawSettingsNav);

	static void HandlePendingFeatureSelection(
		std::string& pendingFeatureSelection,
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu);

	static void RenderLeftColumn(
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu,
		std::string& featureSearch,
		std::map<std::string, bool>& categoryExpansionStates, bool showVersions, const std::function<void()>& drawSettingsNav);

	static void RenderRightColumn(
		const std::vector<MenuFuncInfo>& menuList,
		size_t selectedMenu,
		std::string& pendingFeatureSelection, const Hooks* hooks = nullptr);
};
