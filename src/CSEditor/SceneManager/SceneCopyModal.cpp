#include "SceneCopyModal.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "../../I18n/I18n.h"
#include "../Browser/BrowserWidgets.h"
#include "../EditorWindow.h"
#include "../WeatherUtils.h"
#include "IconsTabler.h"
#include "Menu/Icons/helpers/LocationTargetIcons.h"
#include "Menu/Icons/helpers/WeatherTypeIcons.h"
#include "Menu/MenuIcons.h"
#include "SceneSettingsContextRules.h"
#include "SceneSettingsUI.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using SceneContextId = SceneSettingsManager::SceneContextId;
	using SceneContextType = SceneSettingsManager::SceneContextType;
	using CopyCandidate = SceneSettingsManager::CopyCandidate;
	using CopyConflictPolicy = SceneSettingsManager::CopyConflictPolicy;
	using CopyRejection = SceneSettingsManager::CopyRejection;
	using CopySource = SceneSettingsManager::CopySource;
	using EntrySource = SceneSettingsManager::EntrySource;
	using LocationTargetType = SceneSettingsManager::LocationTargetType;
	using TimeOfDayPeriod = SceneSettingsManager::TimeOfDayPeriod;
	using WeatherFlag = RE::TESWeather::WeatherDataFlag;
	using PeriodSet = std::bitset<SceneSettingsManager::kPeriodCount>;

	/// Fixed ID behind the translated title, so the popup survives a language switch.
	constexpr const char* kPopupId = "###SceneCopyModal";

	/// Sizes in font heights, so the modal scales with the UI font.
	constexpr float kModalWidthEm = 36.0f;
	constexpr float kPaneHeightEm = 17.0f;
	constexpr float kSearchWidthEm = 10.0f;
	constexpr float kPeriodDotPitchEm = 0.5f;
	constexpr float kPeriodDotRadiusEm = 0.16f;
	/// Large fonts would otherwise push the modal past the screen edge.
	constexpr float kMaxViewportWidthShare = 0.9f;
	/// The pick pane's share of the width; the review pane takes the rest.
	constexpr float kPickPaneShare = 0.56f;
	constexpr float kReviewSettingColumnWeight = 2.0f;

	constexpr size_t kSearchBufferSize = 256;

	constexpr ImGuiTableFlags kDetailsTableFlags =
		ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;

	constexpr std::array kContextTypes{ SceneContextType::TimeOfDay, SceneContextType::Weather,
		SceneContextType::Location, SceneContextType::Interior };
	constexpr std::array kWeatherClasses{ WeatherFlag::kPleasant, WeatherFlag::kCloudy, WeatherFlag::kRainy,
		WeatherFlag::kSnow };

	/// A page that has not asked for this long has been closed; evicting on size would drop pages still in use.
	constexpr int kSourceCheckRetentionFrames = 120;

	/** @brief Which side of the copy the page is on: From copies into it, To copies out of it. */
	enum class CopyDirection
	{
		From,
		To
	};

	/** @brief How one setting fares in a copy; also the order the summary chips list them in. */
	enum class CopyOutcome
	{
		Copy,
		AlreadySet,
		Dropped
	};
	constexpr std::array kOutcomes{ CopyOutcome::Copy, CopyOutcome::AlreadySet, CopyOutcome::Dropped };

	/** @brief A glyph and its colour, resolved when the rows are built rather than every frame. */
	struct TintedGlyph
	{
		Icons::GlyphRef glyph;
		ImVec4 color;
	};

	/** @brief One scene (a weather, a location, the interior or the time of day) with the sets it resolves. */
	struct SceneRow
	{
		SceneContextId scene;
		std::string name;
		TintedGlyph icon;
		std::vector<CopySource> sets;
		size_t settingCount = 0;
		std::uint32_t weatherFlags = 0;
		bool flat = false;
		bool authored = false;
		bool current = false;

		/** @brief The set a period addresses: the flat set for any period, or null when the scene has none. */
		const CopySource* FindSet(TimeOfDayPeriod period) const
		{
			if (flat)
				return &sets.front();
			const auto it = std::ranges::find(sets, period, [](const auto& set) { return set.context.period; });
			return it == sets.end() ? nullptr : &*it;
		}
	};

	/** @brief Filters outlive the modal, so reopening it lands where the user left off. */
	struct Filters
	{
		CopyDirection direction = CopyDirection::To;
		std::optional<SceneContextType> type;
		std::optional<WeatherFlag> weatherClass;
		std::optional<LocationTargetType> locationType;
	};
	Filters filters;

	/** @brief One set a copy writes into, with the name the summary shows for it. */
	struct Destination
	{
		SceneContextId context;
		std::string name;

		bool operator==(const Destination&) const = default;
	};

	/** @brief What Copy would run right now, including how source and destination periods are paired. */
	struct CopyPlan
	{
		SceneContextId source;
		std::string sourceName;
		std::vector<Destination> destinations;
		bool allSourcePeriods = false;
		std::optional<bool> emptyDestinationMode;

		bool operator==(const CopyPlan&) const = default;
	};

	/** @brief One details-table row: a setting that fares the same way in every destination it names. */
	struct DetailRow
	{
		std::string feature;
		std::string name;
		std::string value;
		/// The destination's current value, which Overwrite would replace.
		std::optional<std::string> previous;
		CopyRejection rejection = CopyRejection::None;
		CopyOutcome outcome = CopyOutcome::Copy;
		size_t destinationCount = 0;
		/// Newline-joined, ready for the tooltip.
		std::string destinationNames;
	};

	/** @brief Setting counts across every destination, grouped the way the copy lands them. */
	struct CopySummary
	{
		std::array<size_t, kOutcomes.size()> counts{};
		std::map<CopyRejection, size_t> dropped;
		/// Sorted by feature, so the table can head each run of rows with its feature.
		std::vector<DetailRow> details;

		size_t Count(CopyOutcome outcome) const { return counts[std::to_underlying(outcome)]; }
	};

	/** @brief One opening of the modal. Only the page that opened it draws it: a popup opened under one
	 *  page's ID stack cannot be drawn under another's. */
	struct Session
	{
		SceneContextId page;
		std::string featureShortName;
		std::string featureName;
		SceneSettingsManager::SettingSelection pageScope;
		SceneSettingsManager::SettingSelection selection;
		EntrySource sourceLayer = EntrySource::User;
		SceneSettingsManager::RevisionCache<std::vector<CopyCandidate>> sourceSettings;
		bool selectedSourceSettings = false;
		bool allSourcePeriods = false;
		bool timeOfDayForEmptyDestinations = false;
		std::string pageName;
		TintedGlyph pageIcon;
		char search[kSearchBufferSize]{};
		/// Keyed by scene, so a ticked row stays ticked while a filter hides it.
		std::set<SceneContextId> ticked;
		/// From's source, and To's target while nothing is ticked.
		std::optional<SceneContextId> cursor;
		PeriodSet periods;
		int cursorStep = 0;
		bool scrollToCursor = false;
		bool showUnauthored = false;
		bool focusSearch = false;
		Util::DeferredModal modal;
		/// The swap button sits above the list, so it applies next frame rather than under rows already fetched.
		bool swapPending = false;
		/// Enter commits only from the search box or a row, never from a chip or button it would also press.
		bool enterCommits = false;
		/// The outcome chip narrowing the details table, if any.
		std::optional<CopyOutcome> detailsFilter;
		SceneSettingsManager::RevisionCache<std::vector<SceneRow>> rows;
		SceneSettingsManager::RevisionCache<CopySummary> summary;
		CopyPlan summaryPlan;
	};
	Session session;
	struct StandaloneCopy
	{
		EntrySource layer = EntrySource::User;
		std::optional<SceneContextId> source;
		std::optional<SceneContextType> sourceType;
		bool allSourcePeriods = false;
		SceneSettingsManager::RevisionCache<std::vector<CopySource>> sources;
		SceneSettingsManager::RevisionCache<std::vector<CopyCandidate>> settings;
		std::set<SceneSettingsManager::SettingIdentity> selected;
		Util::ConfirmationPopup confirmUserCopy;
		Util::DeferredModal destinations;
		char search[kSearchBufferSize]{};
		char sceneSearch[kSearchBufferSize]{};
	};
	StandaloneCopy standalone;
	bool embedded = false;

	std::optional<EntrySource> GetSourceLayer()
	{
		if (embedded)
			return standalone.layer;
		if (!session.featureShortName.empty())
			return session.sourceLayer;
		return std::nullopt;
	}

	std::vector<CopyCandidate> GetFeatureCopySettings()
	{
		auto settings = SceneSettingsManager::GetSingleton()->GetCopySourceSettings(session.page, session.sourceLayer, session.allSourcePeriods);
		std::erase_if(settings, [](const auto& candidate) {
			return candidate.setting.featureShortName != session.featureShortName ||
			       (session.pageScope && !session.pageScope->contains(candidate.setting));
		});
		return settings;
	}

	bool GetEmptyDestinationMode()
	{
		return session.allSourcePeriods || session.timeOfDayForEmptyDestinations;
	}

	bool DrawAllSourcePeriods(bool& selected)
	{
		const bool changed = ImGui::Checkbox(T(TKEY("scene_copy_all_source_periods"), "All Source Periods"), &selected);
		Util::AddTooltip(T(TKEY("scene_copy_all_source_periods_tooltip"), "Copies each saved value to the matching time of day."));
		return changed;
	}

	/** @brief One line of the list: a section heading, a scene, or the link that reveals hidden scenes. */
	struct ListLine
	{
		enum class Kind
		{
			Header,
			Scene,
			ShowUnauthored
		};
		Kind kind = Kind::Scene;
		const char* text = nullptr;
		const SceneRow* row = nullptr;
	};

	/** @brief The lines the list draws this frame, and how many unauthored scenes it left out. */
	struct VisibleList
	{
		std::vector<ListLine> lines;
		size_t hiddenUnauthored = 0;
	};

	/** @brief One page's cached answer to HasSources. */
	struct SourceCheck
	{
		SceneSettingsManager::RevisionCache<bool> hasSources;
		int lastUsedFrame = 0;
	};
	using SourceCheckKey = std::pair<SceneContextId, std::string>;
	std::map<SourceCheckKey, SourceCheck> sourceChecks;

	using BrowserUI::ChipFace;
	using BrowserUI::FilterChip;
	using Util::Settings::FormatValue;

	/** @brief What one side of the preview names, and the glyph leading it. */
	struct PreviewSide
	{
		std::string label;
		TintedGlyph icon;
	};

	/** @brief Heading a scene's type is listed under. */
	const char* GetContextTypeLabel(SceneContextType type)
	{
		switch (type) {
		case SceneContextType::Interior:
			return T(TKEY("scene_page_group_interior"), "Interior");
		case SceneContextType::Weather:
			return T(TKEY("scene_page_group_weather"), "Weather");
		case SceneContextType::Location:
			return T(TKEY("scene_page_group_location"), "Location");
		default:
			return T(TKEY("scene_page_group_time_of_day"), "Time of Day");
		}
	}

	/** @brief Glyph standing for a whole scene type. */
	Icons::GlyphRef GetContextTypeIcon(SceneContextType type)
	{
		switch (type) {
		case SceneContextType::Interior:
			return Icons::TI(ICON_TI_HOME);
		case SceneContextType::Weather:
			return WeatherTypeIcons::kCloudy;
		case SceneContextType::Location:
			return Icons::TI(MenuIcons::kLocations);
		default:
			return Icons::TI(ICON_TI_CLOCK);
		}
	}

	/** @brief A scene's own glyph: its weather class or location type, else its type's glyph. */
	TintedGlyph GetSceneIcon(const SceneContextId& scene)
	{
		const ImVec4 secondary = Util::Colors::GetSecondary();
		switch (scene.type) {
		case SceneContextType::Weather:
			{
				auto* weather = RE::TESForm::LookupByID<RE::TESWeather>(scene.weatherId);
				return { WeatherTypeIcons::ResolveGlyph(weather), WeatherUtils::GetWeatherTypeColor(weather) };
			}
		case SceneContextType::Location:
			return { LocationTargetIcons::Resolve({ .category = static_cast<LocationTargetIcons::Category>(scene.locationType),
						 .formKey = scene.locationFormKey }),
				secondary };
		default:
			return { GetContextTypeIcon(scene.type), secondary };
		}
	}

	/** @brief Why one candidate is not going to be copied. Empty when it is. */
	const char* GetRejectionText(CopyRejection rejection)
	{
		switch (rejection) {
		case CopyRejection::NotInCatalog:
			return T(TKEY("scene_page_copy_reject_catalog"), "Not a setting a scene can override.");
		case CopyRejection::NotBlendable:
			return T(TKEY("scene_page_copy_reject_blendable"), "Only blendable values can vary by time of day or weather.");
		case CopyRejection::NotAllowedInLayer:
			return T(TKEY("scene_page_copy_reject_layer"), "This page cannot hold this setting.");
		case CopyRejection::ValueRejected:
			return T(TKEY("scene_page_copy_reject_value"), "The value is not valid here.");
		case CopyRejection::BlockedByOverwrite:
			return T(TKEY("scene_page_copy_reject_overwrite"), "A mod override already holds this setting.");
		case CopyRejection::GroupCompanionRejected:
			return T(TKEY("scene_page_copy_reject_companion"), "Another part of the same control cannot be copied.");
		default:
			return "";
		}
	}

	/** @brief Chip for a scene type. */
	ChipFace DescribeContextType(SceneContextType type)
	{
		return { GetContextTypeLabel(type), GetContextTypeIcon(type) };
	}

	/** @brief Chip for a weather classification flag, in the flag's colour. */
	ChipFace DescribeWeatherClass(WeatherFlag flag)
	{
		const auto face = [flag](const char* label, Icons::GlyphRef icon) {
			return ChipFace{ label, icon, WeatherUtils::GetWeatherFlagColor(flag) };
		};
		switch (flag) {
		case WeatherFlag::kPleasant:
			return face(T("feature.cs_editor.pleasant", "Pleasant"), WeatherTypeIcons::kPleasant);
		case WeatherFlag::kCloudy:
			return face(T("feature.cs_editor.cloudy", "Cloudy"), WeatherTypeIcons::kCloudy);
		case WeatherFlag::kRainy:
			return face(T("feature.cs_editor.rainy", "Rainy"), WeatherTypeIcons::kRain);
		default:
			return face(T("feature.cs_editor.snow", "Snow"), WeatherTypeIcons::kSnow);
		}
	}

	/** @brief Chip for a location target type. */
	ChipFace DescribeLocationType(LocationTargetType type)
	{
		return { SceneSettingsContextRules::GetCopyLocationTypeName(type),
			LocationTargetIcons::ResolveCategory(static_cast<LocationTargetIcons::Category>(type)) };
	}

	/** @brief A translated "{}" pattern filled with one count. */
	std::string FormatCount(const char* format, size_t count)
	{
		return std::vformat(format, std::make_format_args(count));
	}

	/** @brief The glyph an outcome's chip and rows carry, in the outcome's colour. */
	TintedGlyph GetOutcomeIcon(CopyOutcome outcome)
	{
		switch (outcome) {
		case CopyOutcome::Copy:
			return { Icons::TI(ICON_TI_CHECK), Util::Colors::GetSuccess() };
		case CopyOutcome::AlreadySet:
			return { Icons::TI(ICON_TI_REFRESH), Util::Colors::GetInfo() };
		default:
			return { Icons::TI(ICON_TI_X), Util::Colors::GetWarning() };
		}
	}

	/** @brief The translated "{}" pattern an outcome's chip counts with. */
	const char* GetOutcomeCountFormat(CopyOutcome outcome)
	{
		switch (outcome) {
		case CopyOutcome::Copy:
			return T(TKEY("scene_copy_will_copy"), "{} will copy");
		case CopyOutcome::AlreadySet:
			return T(TKEY("scene_copy_already_set"), "{} already set");
		default:
			return T(TKEY("scene_copy_dropped"), "{} dropped");
		}
	}

	/** @brief What happens to one details row, for its glyph's tooltip. */
	const char* GetDetailTooltip(const DetailRow& row)
	{
		switch (row.outcome) {
		case CopyOutcome::Copy:
			return T(TKEY("scene_copy_row_copy_tooltip"), "Copies into the destination.");
		case CopyOutcome::AlreadySet:
			return T(TKEY("scene_copy_row_already_set_tooltip"),
				"The destination already holds this. Only Overwrite replaces it.");
		default:
			return GetRejectionText(row.rejection);
		}
	}

	/** @brief A scene's identity, shared by all of its period sets. */
	SceneContextId GetSceneOf(SceneContextId context)
	{
		context.period = TimeOfDayPeriod::Count;
		return context;
	}

	/** @brief A set's name without its period. The time of day has no scene name, so its type stands in. */
	std::string GetSceneName(const CopySource& set)
	{
		if (set.context.type == SceneContextType::TimeOfDay)
			return GetContextTypeLabel(SceneContextType::TimeOfDay);
		if (set.context.period == TimeOfDayPeriod::Count)
			return set.displayName;
		const auto suffix = std::format(" / {}", SceneSettingsContextRules::GetCopyPeriodName(set.context.period));
		assert(set.displayName.ends_with(suffix));
		return set.displayName.substr(0, set.displayName.size() - suffix.size());
	}

	/** @brief The weather's classification flags for the class chips; zero for any other scene. */
	std::uint32_t GetWeatherFlags(const SceneContextId& scene)
	{
		if (scene.type != SceneContextType::Weather)
			return 0;
		const auto* weather = RE::TESForm::LookupByID<RE::TESWeather>(scene.weatherId);
		return weather ? weather->data.flags.underlying() : 0;
	}

	/** @brief Groups the manager's per-period sets into one row per scene, keeping the manager's order. */
	std::vector<SceneRow> BuildRows(const std::vector<CopySource>& sets)
	{
		std::vector<SceneRow> rows;
		std::map<SceneContextId, size_t> rowIndices;
		for (const auto& set : sets) {
			const auto scene = GetSceneOf(set.context);
			const auto [indexIt, inserted] = rowIndices.try_emplace(scene, rows.size());
			if (inserted)
				rows.push_back({ .scene = scene,
					.name = GetSceneName(set),
					.icon = GetSceneIcon(scene),
					.weatherFlags = GetWeatherFlags(scene),
					.flat = set.context.period == TimeOfDayPeriod::Count });
			auto& row = rows[indexIt->second];
			row.sets.push_back(set);
			row.settingCount = std::max(row.settingCount, set.settingCount);
			row.authored |= set.authored;
			row.current |= set.current;
		}
		for (auto& row : rows)
			std::ranges::sort(row.sets, {}, [](const auto& set) { return set.context.period; });
		return rows;
	}

	/** @brief The row for a scene, or null when the list no longer holds it. */
	const SceneRow* FindRow(const std::vector<SceneRow>& rows, const SceneContextId& scene)
	{
		const auto it = std::ranges::find(rows, scene, &SceneRow::scene);
		return it == rows.end() ? nullptr : &*it;
	}

	/** @brief Whether a row passes the chips and the search. The authored filter is applied by the caller. */
	bool MatchesFilters(const SceneRow& row, const std::string& query)
	{
		if (filters.type && row.scene.type != *filters.type)
			return false;
		if (filters.type == SceneContextType::Weather && filters.weatherClass &&
			!(row.weatherFlags & std::to_underlying(*filters.weatherClass)))
			return false;
		if (filters.type == SceneContextType::Location && filters.locationType &&
			row.scene.locationType != *filters.locationType)
			return false;
		return Util::StringMatchesSearch(row.name, query);
	}

	/** @brief The lines the list draws: current scenes pinned first, then one section per type in chip order, then
	 *  the way to reveal hidden ones. The headings stand in for a type column. */
	VisibleList BuildVisibleList(const std::vector<SceneRow>& rows, const std::string& query)
	{
		VisibleList list;
		const bool authoredOnly = filters.direction == CopyDirection::To && !session.showUnauthored && query.empty();
		std::array<std::vector<const SceneRow*>, kContextTypes.size() + 1> sections;
		for (const auto& row : rows) {
			if (!MatchesFilters(row, query))
				continue;
			// A current scene stays listed with nothing authored: it is the likeliest target.
			if (row.current)
				sections.front().push_back(&row);
			else if (authoredOnly && !row.authored)
				++list.hiddenUnauthored;
			else
				sections[1 + static_cast<size_t>(std::ranges::find(kContextTypes, row.scene.type) - kContextTypes.begin())]
					.push_back(&row);
		}

		// Headings only earn their space once there are sections to tell apart.
		const bool headed = std::ranges::count_if(sections, [](const auto& section) { return !section.empty(); }) > 1;
		for (size_t index = 0; index < sections.size(); ++index) {
			if (headed && !sections[index].empty())
				list.lines.push_back({ .kind = ListLine::Kind::Header,
					.text = index == 0 ? T(TKEY("scene_copy_current"), "Current") : GetContextTypeLabel(kContextTypes[index - 1]) });
			for (const auto* row : sections[index])
				list.lines.push_back({ .kind = ListLine::Kind::Scene, .row = row });
		}
		if (list.hiddenUnauthored != 0)
			list.lines.push_back({ .kind = ListLine::Kind::ShowUnauthored });
		return list;
	}

	/** @brief Applies this frame's arrow steps and keeps the cursor on a visible row.
	 *  @return The cursor's line index, or -1 when no row is visible. */
	int UpdateCursor(const VisibleList& list)
	{
		std::vector<int> sceneLines;
		for (int index = 0; index < static_cast<int>(list.lines.size()); ++index)
			if (list.lines[index].kind == ListLine::Kind::Scene)
				sceneLines.push_back(index);
		const int step = std::exchange(session.cursorStep, 0);
		if (sceneLines.empty()) {
			session.cursor.reset();
			return -1;
		}

		const auto cursorIt = std::ranges::find_if(sceneLines,
			[&](int index) { return list.lines[index].row->scene == session.cursor; });
		// A cursor the filters hid snaps to the top match, so typing then Enter picks it.
		const int position = cursorIt == sceneLines.end() ?
		                         0 :
		                         std::clamp(static_cast<int>(cursorIt - sceneLines.begin()) + step, 0,
									 static_cast<int>(sceneLines.size()) - 1);
		const int line = sceneLines[position];
		session.cursor = list.lines[line].row->scene;
		session.scrollToCursor = step != 0;
		return line;
	}

	/** @brief Turns Up/Down in the search box into cursor steps, so the list is driven without leaving it. */
	int OnSearchHistory(ImGuiInputTextCallbackData* data)
	{
		*static_cast<int*>(data->UserData) += data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
		return 0;
	}

	/** @brief An "All" chip, then one chip per option, wrapping as needed; picking one narrows the filter to it. */
	template <typename Option, size_t Count, typename Describe>
	void DrawFilterChips(const char* id, std::optional<Option>& filter, const std::array<Option, Count>& options,
		Describe describe)
	{
		ImGui::PushID(id);
		if (FilterChip("##all", { T(TKEY("filter_all"), "All") }, !filter))
			filter.reset();
		for (const auto option : options) {
			const ChipFace face = describe(option);
			BrowserUI::SameLineIfFits(BrowserUI::MeasureChip(face.label, face.icon.IsValid()));
			ImGui::PushID(static_cast<int>(option));
			if (FilterChip("##chip", face, filter == option))
				filter = option;
			ImGui::PopID();
		}
		ImGui::PopID();
	}

	/** @brief Clears the picks and seeds the periods: the page's own, else every period for a flat page copying out. */
	void ResetSelection()
	{
		session.ticked.clear();
		session.cursor.reset();
		session.periods.reset();
		if (session.page.period != TimeOfDayPeriod::Count)
			session.periods.set(static_cast<size_t>(session.page.period));
		else if (filters.direction == CopyDirection::To)
			session.periods.set();
	}

	/** @brief The period From copies out of: the ticked one if the row holds it, else the row's first set. */
	TimeOfDayPeriod GetSourcePeriod(const SceneRow& row)
	{
		for (const auto period : SceneSettingsManager::kPeriods)
			if (session.periods.test(static_cast<size_t>(period)) && row.FindSet(period))
				return period;
		return row.sets.front().context.period;
	}

	/** @brief The copy the current picks describe; no destinations when nothing is picked. */
	CopyPlan BuildPlan(const std::vector<SceneRow>& rows)
	{
		if (filters.direction == CopyDirection::From) {
			const auto* row = session.cursor ? FindRow(rows, *session.cursor) : nullptr;
			if (!row)
				return {};
			const auto* set = row->FindSet(GetSourcePeriod(*row));
			assert(set);
			return { set->context, set->displayName, { { session.page, session.pageName } } };
		}

		CopyPlan plan{ .source = session.page, .sourceName = session.pageName, .allSourcePeriods = session.allSourcePeriods, .emptyDestinationMode = GetEmptyDestinationMode() };
		if (plan.allSourcePeriods)
			plan.sourceName = GetSceneName({ .context = session.page, .displayName = session.pageName });
		// With nothing ticked the highlighted row is the target, so a single copy needs no tick.
		const auto targets = session.ticked.empty() && session.cursor ? std::set{ *session.cursor } : session.ticked;
		for (const auto& scene : targets) {
			const auto* row = FindRow(rows, scene);
			if (!row)
				continue;
			for (const auto& set : row->sets)
				if (row->flat || plan.allSourcePeriods || session.periods.test(static_cast<size_t>(set.context.period)))
					plan.destinations.push_back({ set.context, set.displayName });
		}
		return plan;
	}

	/** @brief Dry-runs the plan. Mirrors CopySettingsToContext: a control lands, skips or drops as a whole. */
	CopySummary BuildSummary(const CopyPlan& plan)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		CopySummary summary;
		// A setting that fares alike in several destinations is one row, so its key holds everything the row shows.
		using RowKey = std::tuple<SceneSettingsManager::SettingIdentity, CopyOutcome, nlohmann::json, std::optional<std::string>, CopyRejection>;
		std::map<RowKey, size_t> rowIndices;
		for (const auto& destination : plan.destinations) {
			const auto candidates = manager->GetCopyCandidates(plan.source, destination.context, session.featureShortName, session.selection,
				GetSourceLayer(), plan.allSourcePeriods);
			std::map<SceneSettingsContextRules::CopyGroupKey, std::vector<const CopyCandidate*>> groups;
			for (const auto& candidate : candidates)
				groups[SceneSettingsContextRules::GetCopyGroupKey(candidate.setting)].push_back(&candidate);
			for (const auto& [groupKey, group] : groups) {
				const auto outcome = std::ranges::any_of(group, std::logical_not{}, &CopyCandidate::compatible) ?
				                         CopyOutcome::Dropped :
				                     std::ranges::any_of(group, std::identity{}, &CopyCandidate::conflicts) ?
				                         CopyOutcome::AlreadySet :
				                         CopyOutcome::Copy;
				summary.counts[std::to_underlying(outcome)] += group.size();
				for (const auto* candidate : group) {
					if (outcome == CopyOutcome::Dropped)
						++summary.dropped[candidate->rejection];
					auto previous = outcome == CopyOutcome::AlreadySet && candidate->destinationValue ?
					                    std::optional{ FormatValue(*candidate->destinationValue) } :
					                    std::nullopt;
					const auto [indexIt, inserted] = rowIndices.try_emplace(
						RowKey{ candidate->setting, outcome, candidate->value, previous, candidate->rejection }, summary.details.size());
					if (inserted)
						summary.details.push_back({ .feature = SceneSettingsManager::GetFeatureDisplayName(
														candidate->setting.featureShortName),
							.name = candidate->displayName,
							.value = FormatValue(candidate->value),
							.previous = std::move(previous),
							.rejection = candidate->rejection,
							.outcome = outcome });
					auto& row = summary.details[indexIt->second];
					++row.destinationCount;
					if (!row.destinationNames.empty())
						row.destinationNames += '\n';
					row.destinationNames += destination.name;
				}
			}
		}
		std::ranges::stable_sort(summary.details, {}, &DetailRow::feature);
		return summary;
	}

	/** @brief Commits the plan as one batch and reports what landed in a toast. */
	bool RunCopy(const CopyPlan& plan, CopyConflictPolicy policy, std::string_view featureShortName,
		const SceneSettingsManager::SettingSelection& selection)
	{
		const auto destinations =
			plan.destinations | std::views::transform(&Destination::context) | std::ranges::to<std::vector>();
		const auto result = SceneSettingsManager::GetSingleton()->CopySettingsBatch(plan.source, destinations, policy, featureShortName, selection,
			GetSourceLayer(), plan.allSourcePeriods, plan.emptyDestinationMode);
		if (result.blockedByPendingEdit) {
			SceneSettingsUI::WarnPendingContextEdit();
			return false;
		}
		auto copied = result.copied;
		auto overwritten = result.overwritten;
		// Dropped rows count as skipped; the summary already said why they were dropped.
		auto skipped = result.skipped + result.incompatible;
		EditorWindow::GetSingleton()->ShowNotification(
			std::vformat(T(TKEY("scene_page_copy_result"), "{} copied, {} overwritten, {} skipped"),
				std::make_format_args(copied, overwritten, skipped)),
			result.Changed() ? Util::Colors::GetSuccess() : Util::Colors::GetWarning());
		return true;
	}

	/** @brief Flips which side the page is on. The list is refetched and the picks start over. */
	void SwapDirection()
	{
		filters.direction = filters.direction == CopyDirection::To ? CopyDirection::From : CopyDirection::To;
		session.selection = session.pageScope;
		session.selectedSourceSettings = false;
		session.allSourcePeriods = false;
		session.rows = {};
		ResetSelection();
	}

	/** @brief Search and the type chips. */
	void DrawFilterRow()
	{
		if (std::exchange(session.focusSearch, false))
			ImGui::SetKeyboardFocusHere();
		BrowserUI::SearchField("##search", session.search, sizeof(session.search), T("ui.search", "Search..."),
			ImGui::GetFontSize() * kSearchWidthEm, true, ImGuiInputTextFlags_CallbackHistory, OnSearchHistory,
			&session.cursorStep);
		session.enterCommits = ImGui::IsItemFocused();
		ImGui::SameLine();
		DrawFilterChips("type", filters.type, kContextTypes, DescribeContextType);
	}

	/** @brief The narrower chips the Weather and Location types offer and, in To, the authored-only toggle. */
	void DrawContextRow(const std::vector<SceneRow>& rows, const std::string& query)
	{
		if (filters.type == SceneContextType::Weather)
			DrawFilterChips("weatherClass", filters.weatherClass, kWeatherClasses, DescribeWeatherClass);
		else if (filters.type == SceneContextType::Location)
			DrawFilterChips("locationType", filters.locationType, SceneSettingsManager::kLocationTargetTypes,
				DescribeLocationType);
		else  // Holds the row's height, so the list does not jump as the type changes.
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));

		// From lists only scenes that hold settings, so there is nothing for the toggle to hide.
		if (filters.direction != CopyDirection::To)
			return;
		const auto authoredCount = static_cast<int>(
			std::ranges::count_if(rows, [&](const SceneRow& row) { return row.authored && MatchesFilters(row, query); }));
		const float width = BrowserUI::MeasureToggleChip(authoredCount);
		BrowserUI::SameLineIfFits(width);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
		const auto tooltip = std::format("{}\n{}", T(TKEY("scene_copy_authored_only"), "Authored only"),
			T(TKEY("scene_copy_authored_only_tooltip"), "Hides weathers and locations that hold no settings yet."));
		if (BrowserUI::ToggleChip("##authoredOnly", BrowserUI::GlyphIcon(Icons::TI(ICON_TI_PENCIL)), authoredCount,
				!session.showUnauthored, Util::Colors::GetInfo(), tooltip.c_str()))
			session.showUnauthored = !session.showUnauthored;
	}

	/** @brief Leaves a row-wide selectable for the name column: the next cell in To, beside it in From. */
	void EnterNameColumn(bool to)
	{
		if (to)
			ImGui::TableNextColumn();
		else
			ImGui::SameLine(0.0f, 0.0f);
		ImGui::AlignTextToFramePadding();
	}

	/** @brief A glyph in a font-sized box `height` tall, so the text after it lines up whatever the glyph's width. */
	void DrawTintedGlyph(const TintedGlyph& icon, float height)
	{
		const float box = ImGui::GetFontSize();
		const ImVec2 min = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(box, height));
		Icons::DrawCenteredGlyph(ImGui::GetWindowDrawList(), ImVec2(min.x, min.y + (height - box) * 0.5f),
			ImVec2(box, box), icon.glyph, ImGui::GetColorU32(icon.color));
		ImGui::SameLine();
	}

	/** @brief One dot per period, filled where that period's set holds settings; rings when one set covers them all. */
	void DrawPeriodDots(const SceneRow& row)
	{
		const float pitch = ImGui::GetFontSize() * kPeriodDotPitchEm;
		const float radius = ImGui::GetFontSize() * kPeriodDotRadiusEm;
		const float height = ImGui::GetFrameHeight();
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(pitch * SceneSettingsManager::kPeriodCount, height));

		const ImU32 flatColor = ImGui::GetColorU32(Util::Colors::GetInfo());
		const ImU32 setColor = ImGui::GetColorU32(Util::Colors::GetAccent());
		const ImU32 emptyColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		for (const auto period : SceneSettingsManager::kPeriods) {
			const auto index = static_cast<float>(static_cast<size_t>(period));
			const ImVec2 center(origin.x + pitch * (index + 0.5f), origin.y + height * 0.5f);
			if (row.flat) {
				Util::DrawIconCircle(center, radius, flatColor, false);
				continue;
			}
			const auto* set = row.FindSet(period);
			Util::DrawIconCircle(center, radius, set && set->settingCount != 0 ? setColor : emptyColor, true);
		}
		Util::AddTooltip(row.flat ? T(TKEY("scene_copy_dots_flat_tooltip"), "One set covers every period.") :
									T(TKEY("scene_copy_dots_tooltip"), "Filled periods hold settings."));
	}

	/** @brief One scene: click highlights it, the tick box adds it to To's targets, double-click commits.
	 *  @return Whether a double-click asked to commit. */
	bool DrawSceneRow(const SceneRow& row, bool isCursor, bool to)
	{
		if (row.current)
			BrowserUI::RowStripe(ImGui::GetColorU32(Util::Colors::GetSuccess()), BrowserUI::RowHeight());
		const bool pressed = Util::TableRowSelectable("##row", isCursor, BrowserUI::kTickRowFlags | ImGuiSelectableFlags_AllowDoubleClick);
		session.enterCommits |= ImGui::IsItemFocused();
		const bool doubleClicked = pressed && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
		if (pressed)
			session.cursor = row.scene;
		if (isCursor && session.scrollToCursor)
			ImGui::SetScrollHereY();

		if (to) {
			ImGui::SameLine(0.0f, 0.0f);
			bool ticked = session.ticked.contains(row.scene);
			if (ImGui::Checkbox("##tick", &ticked)) {
				if (ticked)
					session.ticked.insert(row.scene);
				else
					session.ticked.erase(row.scene);
			}
		}
		EnterNameColumn(to);
		DrawTintedGlyph(row.icon, ImGui::GetFrameHeight());
		const char* live = T(TKEY("badge_live"), "LIVE");
		const float badgeWidth = row.current ? Util::MeasureBadgeWidth(live) + ImGui::GetStyle().ItemSpacing.x : 0.0f;
		BrowserUI::EllipsizedText(row.name.c_str(), std::max(1.0f, ImGui::GetContentRegionAvail().x - badgeWidth),
			ImGui::GetStyleColorVec4(ImGuiCol_Text));
		if (row.current)
			BrowserUI::InlineBadge(live, Util::Colors::GetSuccess());

		ImGui::TableNextColumn();
		DrawPeriodDots(row);

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		Util::Text::Secondary("%zu", row.settingCount);
		Util::AddTooltip(T(TKEY("scene_copy_count_tooltip"), "Settings this context holds."));
		return doubleClicked;
	}

	/** @brief Draws the list and takes its clicks.
	 *  @param reservedHeight Height left below the list for the rows that follow it in the pane.
	 *  @return Whether a double-click asked to commit. */
	bool DrawList(const VisibleList& list, int cursorLine, float reservedHeight)
	{
		const bool to = filters.direction == CopyDirection::To;
		if (!BrowserUI::BeginListTable("##rows", to ? 4 : 3, ImVec2(0.0f, -reservedHeight), list.lines.empty(),
				T(TKEY("scene_copy_empty"), "Nothing matches.")))
			return false;

		if (to)
			ImGui::TableSetupColumn("##tick", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("##periods", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("##count", ImGuiTableColumnFlags_WidthFixed);

		bool commitRequested = false;
		// Every line is frame-high, which the clipper's uniform-height assumption needs.
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(list.lines.size()));
		if (cursorLine >= 0)
			clipper.IncludeItemByIndex(cursorLine);
		while (clipper.Step()) {
			for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
				const auto& line = list.lines[index];
				ImGui::PushID(index);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (line.kind != ListLine::Kind::Scene) {
					if (to)
						ImGui::TableNextColumn();
					ImGui::AlignTextToFramePadding();
				}
				switch (line.kind) {
				case ListLine::Kind::Header:
					BrowserUI::SectionLabel(line.text);
					break;
				case ListLine::Kind::ShowUnauthored:
					if (BrowserUI::Link("##showUnauthored",
							FormatCount(T(TKEY("scene_copy_show_unauthored"), "{} without settings, show all"),
								list.hiddenUnauthored)
								.c_str()))
						session.showUnauthored = true;
					break;
				case ListLine::Kind::Scene:
					commitRequested |= DrawSceneRow(*line.row, index == cursorLine, to);
					break;
				}
				ImGui::PopID();
			}
		}
		BrowserUI::EndListTable();
		return commitRequested;
	}

	/** @brief To's tick controls: tick every listed scene, and a count of the ticks, including hidden ones, that clears them. */
	void DrawSelectionFooter(const VisibleList& list)
	{
		auto sceneLines =
			list.lines | std::views::filter([](const ListLine& line) { return line.kind == ListLine::Kind::Scene; });
		const auto selectedShown = static_cast<size_t>(std::ranges::count_if(sceneLines,
			[](const ListLine& line) { return session.ticked.contains(line.row->scene); }));
		const auto action = BrowserUI::SelectionFooter(session.ticked.size(), selectedShown,
			T(TKEY("scene_copy_clear_ticks_tooltip"), "Unticks every scene, shown or hidden."));
		if (action == BrowserUI::SelectionAction::SelectShown)
			for (const auto& line : sceneLines)
				session.ticked.insert(line.row->scene);
		else if (action == BrowserUI::SelectionAction::Clear)
			session.ticked.clear();
	}

	/** @brief The shared period row: which periods receive in To, which one is copied out of in From. */
	void DrawPeriodChips(const SceneRow* cursorRow)
	{
		const bool from = filters.direction == CopyDirection::From;
		if (!from && session.allSourcePeriods)
			return;
		// From shows the period it will really copy, which falls back when the source lacks the ticked one.
		const auto sourcePeriod = from && cursorRow && !cursorRow->flat ?
		                              std::optional{ GetSourcePeriod(*cursorRow) } :
		                              std::nullopt;
		ImGui::AlignTextToFramePadding();
		Util::Text::Secondary("%s", from ? T(TKEY("scene_copy_source_period"), "Source period") : T(TKEY("scene_copy_periods"), "Destination periods"));
		Util::AddTooltip(from ? T(TKEY("scene_copy_periods_from_tooltip"),
									"The period the settings are read from. A source with one set for every period has nothing to pick.") :
								T(TKEY("scene_copy_periods_to_tooltip"), "The periods that receive the settings."));
		ImGui::PushID("periods");
		{
			// A flat source has one set for every period, so there is nothing to pick.
			const Util::DisableGuard flatGuard(from && (!cursorRow || cursorRow->flat));
			for (const auto period : SceneSettingsManager::kPeriods) {
				const auto bit = static_cast<size_t>(period);
				const auto* label = SceneSettingsContextRules::GetCopyPeriodName(period);
				BrowserUI::SameLineIfFits(BrowserUI::MeasureChip(label, false));
				ImGui::PushID(static_cast<int>(bit));
				const Util::DisableGuard missingGuard(from && cursorRow && !cursorRow->FindSet(period));
				const bool selected = sourcePeriod ? *sourcePeriod == period : session.periods.test(bit);
				if (FilterChip("##period", { label }, selected)) {
					if (from)
						session.periods.reset();
					session.periods.set(bit, from || !session.periods.test(bit));
				}
				ImGui::PopID();
			}
		}
		if (!from) {
			const auto* label = T(TKEY("filter_all"), "All");
			BrowserUI::SameLineIfFits(BrowserUI::MeasureChip(label, false));
			if (FilterChip("##all", { label }, session.periods.all()))
				session.periods.set();
		}
		ImGui::PopID();
	}

	/** @brief To's destination side: one set by name, else how many sets across how many scenes. */
	PreviewSide DescribeDestinations(const CopyPlan& plan, const std::vector<SceneRow>& rows)
	{
		const auto& first = plan.destinations.front();
		const auto* firstRow = FindRow(rows, GetSceneOf(first.context));
		assert(firstRow);
		if (plan.destinations.size() == 1)
			return { first.name, firstRow->icon };

		auto contextCount = plan.destinations.size();
		const auto scenes = plan.destinations |
		                    std::views::transform([](const Destination& destination) { return GetSceneOf(destination.context); }) |
		                    std::ranges::to<std::set>();
		if (scenes.size() == 1)
			return { std::vformat(T(TKEY("scene_copy_scene_periods"), "{}, {} periods"),
						 std::make_format_args(firstRow->name, contextCount)),
				firstRow->icon };
		auto sceneCount = scenes.size();
		return { std::vformat(T(TKEY("scene_copy_contexts_in_scenes"), "{} contexts in {} scenes"),
					 std::make_format_args(contextCount, sceneCount)),
			{ Icons::TI(ICON_TI_STACK_2), Util::Colors::GetSecondary() } };
	}

	/** @brief The dimmed arrow pointing from what a copy reads to what it writes. */
	void DrawArrow()
	{
		const Icons::GlyphRef arrow = Icons::TI(ICON_TI_CHEVRON_RIGHT);
		Icons::FontGuard font(arrow);
		Util::Text::Disabled("%s", arrow.utf8);
	}

	/** @brief Explicit copy direction and the source and destination names. */
	void DrawRoute(const CopyPlan& plan, const std::vector<SceneRow>& rows)
	{
		const bool from = filters.direction == CopyDirection::From;
		ImGui::Spacing();
		ImGui::Text("%s: %s", from ? T(TKEY("scene_copy_to"), "To") : T(TKEY("scene_copy_from"), "From"),
			from ? session.pageName.c_str() : plan.sourceName.c_str());
		if (!plan.destinations.empty()) {
			const auto name = from ? plan.sourceName : DescribeDestinations(plan, rows).label;
			ImGui::Text("%s: %s", from ? T(TKEY("scene_copy_from"), "From") : T(TKEY("scene_copy_to"), "To"), name.c_str());
		} else {
			Util::TextUnformattedDisabled(from ? T(TKEY("scene_copy_choose_source"), "Choose one source scene below.") : T(TKEY("scene_copy_choose_destinations"), "Select the scenes that will receive these settings."));
		}
		ImGui::Spacing();
	}

	void DrawEmptyDestinationMode()
	{
		const char* normal = T("feature.scene_manager.weather.normal", "Normal");
		const char* periodic = T("feature.scene_manager.tab.time_of_day", "Time of Day");
		const float labelWidth = std::max(ImGui::CalcTextSize(normal).x, ImGui::CalcTextSize(periodic).x);
		ImGui::SetNextItemWidth(labelWidth + ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().FramePadding.x);
		const Util::DisableGuard matchingPeriods(session.allSourcePeriods);
		const char* label = T(TKEY("scene_copy_empty_destination_mode"), "Mode for empty weather and location scenes");
		if (ImGui::BeginCombo(label, GetEmptyDestinationMode() ? periodic : normal)) {
			for (const auto& [mode, option] : { std::pair{ false, normal }, std::pair{ true, periodic } }) {
				if (Util::RoundedSelectable(option, session.timeOfDayForEmptyDestinations == mode)) {
					session.timeOfDayForEmptyDestinations = mode;
					session.rows = {};
				}
			}
			ImGui::EndCombo();
		}
		Util::AddTooltip(T(TKEY("scene_copy_empty_destination_mode_tooltip"), "Scenes with settings or an explicit mode keep their current mode."),
			Util::kTooltipWhenDisabled);
	}

	/** @brief One outcome's count as a chip that narrows the details to its rows; Dropped's tooltip lists why. */
	void DrawOutcomeChip(const CopySummary& summary, CopyOutcome outcome)
	{
		const auto count = summary.Count(outcome);
		const auto icon = GetOutcomeIcon(outcome);
		const bool active = session.detailsFilter == outcome;
		ImGui::PushID(static_cast<int>(outcome));
		{
			const Util::DisableGuard empty(count == 0);
			if (BrowserUI::Chip("##outcome", FormatCount(GetOutcomeCountFormat(outcome), count).c_str(), icon.glyph,
					&icon.color, active ? &icon.color : nullptr))
				session.detailsFilter = active ? std::nullopt : std::optional{ outcome };
		}
		ImGui::PopID();

		std::string tooltip;
		if (outcome == CopyOutcome::Dropped)
			for (const auto& [rejection, reasonCount] : summary.dropped) {
				const char* reason = GetRejectionText(rejection);
				tooltip += std::vformat(T(TKEY("scene_copy_dropped_reason"), "{} dropped: {}"),
					std::make_format_args(reasonCount, reason));
				tooltip += '\n';
			}
		tooltip += active ? T(TKEY("scene_copy_filter_clear_tooltip"), "Click to list every setting again.") :
		                    T(TKEY("scene_copy_filter_tooltip"), "Click to list only these in the details.");
		Util::AddTooltip(tooltip.c_str());
	}

	/** @brief One details row: outcome glyph and name, then the value, or the old value it replaces and the new.
	 *  @param merged Whether the plan has several destinations, so the row says how many it stands for. */
	void DrawDetailRow(const DetailRow& row, bool merged)
	{
		const char* tooltip = GetDetailTooltip(row);
		const bool dropped = row.outcome == CopyOutcome::Dropped;
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		DrawTintedGlyph(GetOutcomeIcon(row.outcome), ImGui::GetTextLineHeight());
		Util::AddTooltip(tooltip);
		const auto countLabel = merged ? std::format("×{}", row.destinationCount) : std::string{};
		const float countWidth = merged ? ImGui::CalcTextSize(countLabel.c_str()).x + ImGui::GetStyle().ItemSpacing.x : 0.0f;
		BrowserUI::EllipsizedText(row.name.c_str(), std::max(1.0f, ImGui::GetContentRegionAvail().x - countWidth),
			ImGui::GetStyleColorVec4(dropped ? ImGuiCol_TextDisabled : ImGuiCol_Text));
		if (merged) {
			ImGui::SameLine();
			Util::Text::Secondary("%s", countLabel.c_str());
			Util::AddTooltip(row.destinationNames.c_str());
		}

		ImGui::TableNextColumn();
		if (row.previous) {
			Util::TextUnformattedDisabled(row.previous->c_str());
			ImGui::SameLine();
			DrawArrow();
			ImGui::SameLine();
		}
		if (dropped) {
			Util::TextUnformattedDisabled(row.value.c_str());
			Util::AddTooltip(tooltip);
		} else {
			ImGui::TextUnformatted(row.value.c_str());
		}
	}

	/** @brief The chip-filtered rows under a heading per feature, filling the rest of the review pane. */
	void DrawDetails(const CopySummary& summary, size_t destinationCount)
	{
		if (destinationCount == 0) {
			BrowserUI::EmptyState(T(TKEY("scene_copy_pick"), "Pick a context to copy."),
				T(TKEY("scene_copy_pick_hint"), "Every setting it would write shows here first."));
			return;
		}
		if (!ImGui::BeginTable("##changes", 2, kDetailsTableFlags))
			return;
		ImGui::TableSetupColumn(T(TKEY("scene_page_copy_column_setting"), "Setting"), ImGuiTableColumnFlags_WidthStretch, kReviewSettingColumnWeight);
		ImGui::TableSetupColumn(T(TKEY("scene_page_copy_column_change"), "Change"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		std::vector<const DetailRow*> rows;
		for (const auto& row : summary.details)
			if (!session.detailsFilter || row.outcome == *session.detailsFilter)
				rows.push_back(&row);
		for (const auto section : rows | std::views::chunk_by([](const DetailRow* lhs, const DetailRow* rhs) {
				 return lhs->feature == rhs->feature;
			 })) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			BrowserUI::SectionLabel(section.front()->feature.c_str());
			for (const auto* row : section)
				DrawDetailRow(*row, destinationCount > 1);
		}
		ImGui::EndTable();
	}

	/** @brief A chip per outcome count, then the details table they filter. */
	void DrawSummary(const CopySummary& summary, size_t destinationCount)
	{
		// A filter whose rows went away would leave the table empty with nothing to click to clear it.
		if (session.detailsFilter && summary.Count(*session.detailsFilter) == 0)
			session.detailsFilter.reset();
		for (const auto outcome : kOutcomes) {
			// Unlike the other two, a zero drop count is not worth a chip.
			if (outcome == CopyOutcome::Dropped && summary.Count(outcome) == 0)
				continue;
			if (outcome != kOutcomes.front())
				ImGui::SameLine();
			DrawOutcomeChip(summary, outcome);
		}
		DrawDetails(summary, destinationCount);
	}

	/** @brief The guide at the left; Copy and Overwrite, each with the count it would write, then Cancel at the right.
	 *  @return The policy to run, if the user chose one. */
	std::optional<CopyConflictPolicy> DrawFooter(const CopySummary& summary, bool commitRequested)
	{
		ImGui::AlignTextToFramePadding();
		Util::HelpMarker(embedded ? T(TKEY("scene_copy_standalone_guide"),
										"Select destinations and review the values. Copy New keeps existing User Settings. Copy and Replace updates them.") :
									T(TKEY("scene_copy_guide"),
										"Choose a direction, select scenes, then review the settings. Copy New keeps existing values; Copy and Replace changes them too. Only your User Settings are written."));

		std::optional<CopyConflictPolicy> decision;
		const auto copyCount = summary.Count(CopyOutcome::Copy);
		const auto alreadySetCount = summary.Count(CopyOutcome::AlreadySet);
		const bool canCopy = copyCount != 0;
		const auto copyLabel = FormatCount(T(TKEY("scene_copy_copy_count"), "Copy New ({})"), copyCount) + "###copy";
		const auto overwriteLabel =
			FormatCount(T(TKEY("scene_copy_overwrite_count"), "Copy and Replace ({})"), copyCount + alreadySetCount) + "###overwrite";
		const char* cancelLabel = T(TKEY("cancel"), "Cancel");
		const auto& style = ImGui::GetStyle();
		float width = style.ItemSpacing.x * 2.0f;
		for (const char* label : { copyLabel.c_str(), overwriteLabel.c_str(), cancelLabel })
			width += ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
		BrowserUI::RightAlign(width);

		ImGui::BeginDisabled(!canCopy);
		if (Util::SuccessButton(copyLabel.c_str()))
			decision = CopyConflictPolicy::SkipExisting;
		ImGui::EndDisabled();
		Util::AddTooltip(T(TKEY("scene_copy_copy_tooltip"),
							 "Copies what is not already set. Enter or a double-click does the same."),
			Util::kTooltipWhenDisabled);

		ImGui::SameLine();
		ImGui::BeginDisabled(alreadySetCount == 0);
		if (Util::WarningButton(overwriteLabel.c_str()))
			decision = CopyConflictPolicy::OverwriteExisting;
		ImGui::EndDisabled();
		Util::AddTooltip(T(TKEY("scene_copy_overwrite_tooltip"),
							 "Also replaces the values already set at the destination."),
			Util::kTooltipWhenDisabled);

		ImGui::SameLine();
		if (Util::Button(cancelLabel)) {
			ImGui::CloseCurrentPopup();
			return std::nullopt;
		}

		// Keyboard and double-click never overwrite: replacing values always takes a deliberate click.
		const bool enterPressed = session.enterCommits &&
		                          (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
		if (!decision && canCopy && (enterPressed || commitRequested))
			decision = CopyConflictPolicy::SkipExisting;
		return decision;
	}

	bool DrawCopySettingChecks(const std::vector<CopyCandidate>& settings,
		std::set<SceneSettingsManager::SettingIdentity>& selected, std::string_view query = {})
	{
		bool changed = ImGui::SmallButton(T("feature.scene_manager.action.select_all", "Select All"));
		if (changed)
			for (const auto& setting : settings)
				selected.insert(setting.setting);
		ImGui::SameLine();
		if (ImGui::SmallButton(T("feature.scene_manager.action.select_none", "Select None"))) {
			selected.clear();
			changed = true;
		}
		const float height = std::min(ImGui::GetFontSize() * kPaneHeightEm,
			ImGui::GetFrameHeightWithSpacing() * static_cast<float>(std::max(settings.size(), size_t{ 1 })) + 2.0f * ImGui::GetStyle().WindowPadding.y);
		if (ImGui::BeginChild("##CopySourceSettings", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
			for (size_t index = 0; index < settings.size(); ++index) {
				const auto& setting = settings[index];
				const auto label = std::format("{} / {}", SceneSettingsManager::GetFeatureDisplayName(setting.setting.featureShortName), setting.displayName);
				if (!Util::StringMatchesSearch(label, query))
					continue;
				ImGui::PushID(static_cast<int>(index));
				bool checked = selected.contains(setting.setting);
				if (ImGui::Checkbox(label.c_str(), &checked)) {
					if (checked)
						selected.insert(setting.setting);
					else
						selected.erase(setting.setting);
					changed = true;
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
		return changed;
	}

	/** @brief The modal's body for one frame: the route and filters across the top, the pick pane beside the
	 *  review pane, then the footer. */
	void DrawContents()
	{
		if (EditorWindow::ClosePopupOnEscape())
			return;
		if (std::exchange(session.swapPending, false))
			SwapDirection();
		if (!embedded) {
			const char* directions[] = { T(TKEY("scene_copy_out"), "Copy to Other Scenes"), T(TKEY("scene_copy_in"), "Copy into This Scene") };
			int direction = filters.direction == CopyDirection::From ? 1 : 0;
			if (Util::SegmentedControl("##CopyDirection", directions, IM_ARRAYSIZE(directions), direction))
				session.swapPending = true;
		}

		auto* manager = SceneSettingsManager::GetSingleton();
		const auto revision = manager->GetEntryPresentationRevision();
		const bool featureCopy = !embedded && !session.featureShortName.empty();
		if (featureCopy) {
			const char* layers[] = { T("feature.scene_manager.section.user_settings", "User Settings"),
				T("feature.scene_manager.section.overwrite_files", "Preset Settings") };
			int layer = session.sourceLayer == EntrySource::User ? 0 : 1;
			if (Util::SegmentedControl("##FeatureCopySourceLayer", layers, IM_ARRAYSIZE(layers), layer)) {
				session.sourceLayer = layer == 0 ? EntrySource::User : EntrySource::Overwrite;
				session.selection = session.pageScope;
				session.sourceSettings = {};
				session.selectedSourceSettings = false;
				session.rows = {};
				session.summary = {};
			}
		}
		bool hasSourceSettings = true;
		if (featureCopy && filters.direction == CopyDirection::To) {
			if (session.page.period != TimeOfDayPeriod::Count && DrawAllSourcePeriods(session.allSourcePeriods)) {
				session.sourceSettings = {};
				session.selectedSourceSettings = false;
				session.rows = {};
			}
			const auto& settings = session.sourceSettings.Get(revision, GetFeatureCopySettings);
			hasSourceSettings = !settings.empty();
			if (!std::exchange(session.selectedSourceSettings, true)) {
				auto& selection = session.selection.emplace();
				for (const auto& candidate : settings)
					selection.insert(candidate.setting);
			}
			if (!hasSourceSettings) {
				Util::TextUnformattedDisabled(T("feature.scene_manager.copy.no_sources", "No sources match this filter."));
			} else if (ImGui::CollapsingHeader(T("feature.scene_manager.column.setting", "Setting")) && DrawCopySettingChecks(settings, *session.selection)) {
				session.rows = {};
				session.summary = {};
			}
		}
		if (!hasSourceSettings) {
			if (ImGui::Button(T("ui.cancel", "Cancel")))
				ImGui::CloseCurrentPopup();
			return;
		}
		if (filters.direction == CopyDirection::To)
			DrawEmptyDestinationMode();
		// Fetched after the swap, which drops the cached rows.
		const auto& rows = session.rows.Get(revision, [&] {
			auto contexts = filters.direction == CopyDirection::From ? manager->GetCopySources(session.page, session.featureShortName, GetSourceLayer()) :
			                                                           manager->GetCopyDestinations(session.page, session.featureShortName,
																		   GetSourceLayer(), session.selection, session.allSourcePeriods, GetEmptyDestinationMode());
			if (session.selection && filters.direction == CopyDirection::From) {
				for (auto& row : contexts) {
					const auto candidates = manager->GetCopyCandidates(row.context, session.page, session.featureShortName, session.selection, GetSourceLayer());
					row.settingCount = candidates.size();
				}
				std::erase_if(contexts, [](const auto& row) { return row.settingCount == 0; });
			}
			return BuildRows(contexts);
		});
		// Built from the search as it stood before this frame's typing, which lands next frame.
		const std::string query = session.search;
		const auto list = BuildVisibleList(rows, query);
		const int cursorLine = UpdateCursor(list);
		DrawRoute(BuildPlan(rows), rows);
		DrawFilterRow();

		const bool to = filters.direction == CopyDirection::To;
		const float paneHeight = ImGui::GetFontSize() * kPaneHeightEm;
		bool commitRequested = false;
		if (ImGui::BeginChild("##pick", ImVec2(ImGui::GetContentRegionAvail().x * kPickPaneShare, paneHeight),
				ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar)) {
			ImGui::TextUnformatted(to ? T(TKEY("scene_copy_destinations"), "Destination scenes") : T(TKEY("scene_copy_source"), "Source scene"));
			DrawContextRow(rows, query);
			// The list leaves room for the selection footer (To only).
			commitRequested = DrawList(list, cursorLine, to ? ImGui::GetFrameHeightWithSpacing() : 0.0f);
			if (to)
				DrawSelectionFooter(list);
		}
		ImGui::EndChild();

		// A double-click commits alongside whatever is ticked, so its row joins this plan without staying ticked.
		assert(!commitRequested || session.cursor);
		const bool tickAdded = commitRequested && filters.direction == CopyDirection::To &&
		                       session.ticked.insert(*session.cursor).second;
		const auto plan = BuildPlan(rows);
		if (tickAdded)
			session.ticked.erase(*session.cursor);
		const auto& summary = session.summary.Get(
			revision,
			[&] {
				session.summaryPlan = plan;
				return BuildSummary(plan);
			},
			plan != session.summaryPlan);

		Util::ToolbarDivider();
		if (ImGui::BeginChild("##review", ImVec2(0.0f, paneHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar)) {
			ImGui::TextUnformatted(T(TKEY("scene_copy_review"), "Settings to copy"));
			DrawSummary(summary, plan.destinations.size());
		}
		ImGui::EndChild();

		DrawPeriodChips(session.cursor ? FindRow(rows, *session.cursor) : nullptr);
		ImGui::Separator();
		if (const auto decision = DrawFooter(summary, commitRequested); decision && RunCopy(plan, *decision, session.featureShortName, session.selection))
			ImGui::CloseCurrentPopup();
	}

	CopyPlan BuildPresetToUserPlan(const SceneContextId& source)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		CopyPlan plan{ .source = source, .allSourcePeriods = standalone.allSourcePeriods };
		if (plan.allSourcePeriods) {
			for (const auto period : SceneSettingsManager::kPeriods) {
				auto destination = source;
				destination.period = period;
				plan.destinations.push_back({ destination, manager->GetSceneContextDisplayName(destination) });
			}
		} else {
			plan.destinations.push_back({ source, manager->GetSceneContextDisplayName(source) });
		}
		return plan;
	}

	void CopyPresetSelectionToUser(CopyConflictPolicy policy)
	{
		if (standalone.source)
			RunCopy(BuildPresetToUserPlan(*standalone.source), policy, {}, standalone.selected);
	}

	void DrawStandaloneSceneList(const std::vector<CopySource>& sources, std::vector<CopySource>::const_iterator& source)
	{
		const auto pick = [&](const CopySource& candidate) {
			standalone.source = candidate.context;
			standalone.allSourcePeriods = false;
			standalone.settings = {};
			standalone.selected.clear();
			source = sources.begin() + (&candidate - sources.data());
		};
		const auto matchesType = [&](const CopySource& candidate) {
			return !standalone.sourceType || candidate.context.type == *standalone.sourceType;
		};
		if (source != sources.end() && !matchesType(*source)) {
			standalone.source.reset();
			standalone.allSourcePeriods = false;
			standalone.selected.clear();
			source = sources.end();
		}
		if (standalone.sourceType == SceneContextType::Interior && source == sources.end()) {
			if (const auto interior = std::ranges::find_if(sources, matchesType); interior != sources.end()) {
				pick(*interior);
			}
		}
		BrowserUI::SearchField("##CopySceneSearch", standalone.sceneSearch, sizeof(standalone.sceneSearch), T("ui.search", "Search..."), -FLT_MIN, true);
		if (ImGui::BeginChild("##CopyScenes", ImVec2(0.0f, ImGui::GetFontSize() * kPaneHeightEm), ImGuiChildFlags_Borders)) {
			bool found = false;
			for (const auto& candidate : sources) {
				if (!matchesType(candidate) || !Util::StringMatchesSearch(candidate.displayName, standalone.sceneSearch))
					continue;
				found = true;
				ImGui::PushID(static_cast<int>(&candidate - sources.data()));
				const auto label = std::vformat(T("feature.scene_manager.copy.source_count", "{0} ({1})"),
					std::make_format_args(candidate.displayName, candidate.settingCount));
				const float rowHeight = ImGui::GetFrameHeight();
				if (Util::RoundedSelectable("##SourceScene", standalone.source == candidate.context,
						ImGuiSelectableFlags_NoPadWithHalfSpacing, ImVec2(0.0f, rowHeight)))
					pick(candidate);
				const auto min = ImGui::GetItemRectMin();
				const auto max = ImGui::GetItemRectMax();
				const float textX = min.x + Util::GetSelectableTextInset();
				const ImVec4 clip(min.x, min.y, max.x - ImGui::GetStyle().FramePadding.x, max.y);
				ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
					ImVec2(textX, Util::CenterTextTopY(min.y, rowHeight)), ImGui::GetColorU32(ImGuiCol_Text),
					label.c_str(), nullptr, 0.0f, &clip);
				if (textX + ImGui::CalcTextSize(label.c_str()).x > clip.z)
					Util::AddTooltip(label.c_str());
				ImGui::PopID();
			}
			if (!found)
				Util::TextUnformattedDisabled(T("feature.scene_manager.copy.no_sources", "No sources match this filter."));
		}
		ImGui::EndChild();
	}

	void DrawStandaloneSource()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const char* layers[] = { T("feature.scene_manager.section.user_settings", "User Settings"),
			T("feature.scene_manager.section.overwrite_files", "Preset Settings") };
		int layer = standalone.layer == EntrySource::User ? 0 : 1;
		if (Util::SegmentedControl("##SourceLayer", layers, IM_ARRAYSIZE(layers), layer)) {
			standalone = {};
			standalone.layer = layer == 0 ? EntrySource::User : EntrySource::Overwrite;
		}

		const auto revision = manager->GetEntryPresentationRevision();
		const auto& sources = standalone.sources.Get(revision, [&] { return manager->GetCopySources(std::nullopt, {}, standalone.layer); });
		auto source = standalone.source ? std::ranges::find(sources, *standalone.source, &CopySource::context) : sources.end();
		DrawFilterChips("##SourceType", standalone.sourceType, kContextTypes, DescribeContextType);
		if (ImGui::BeginTable("##CopyFromLayout", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_Borders | ImGuiTableFlags_NoSavedSettings)) {
			ImGui::TableSetupColumn(T(TKEY("scene_copy_source"), "Source scene"));
			ImGui::TableSetupColumn(T("feature.scene_manager.column.setting", "Setting"));
			ImGui::TableHeadersRow();
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			DrawStandaloneSceneList(sources, source);
			ImGui::TableNextColumn();
			if (source != sources.end()) {
				if (source->context.period != TimeOfDayPeriod::Count && DrawAllSourcePeriods(standalone.allSourcePeriods))
					standalone.settings = {};
				const auto& settings = standalone.settings.Get(revision, [&] {
					auto result = manager->GetCopySourceSettings(source->context, standalone.layer, standalone.allSourcePeriods);
					std::erase_if(standalone.selected, [&](const auto& identity) {
						return std::ranges::find(result, identity, &CopyCandidate::setting) == result.end();
					});
					return result;
				});
				BrowserUI::SearchField("##CopySettingSearch", standalone.search, sizeof(standalone.search), T("ui.search", "Search..."), -FLT_MIN, true);
				DrawCopySettingChecks(settings, standalone.selected, standalone.search);
			} else {
				Util::TextUnformattedDisabled(T(TKEY("scene_copy_source_prompt"), "Choose a source scene..."));
			}
			ImGui::EndTable();
		}
		const Util::DisableGuard disabled(source == sources.end() || standalone.selected.empty() || manager->HasPendingContextEdit());
		const bool fromPreset = standalone.layer == EntrySource::Overwrite;
		const auto* destinationLabel = fromPreset ? T(TKEY("scene_copy_out"), "Copy to Other Scenes") :
		                                            T("feature.scene_manager.copy.copy_selected", "Copy Selected Settings");
		if (fromPreset) {
			const auto* copyLabel = T("menu.presets.copy_to_user", "Copy to User Settings");
			if (Util::Button(copyLabel)) {
				const auto plan = BuildPresetToUserPlan(source->context);
				const bool hasConflicts = std::ranges::any_of(plan.destinations, [&](const auto& destination) {
					const auto candidates = manager->GetCopyCandidates(plan.source, destination.context, {}, standalone.selected, standalone.layer, plan.allSourcePeriods);
					return std::ranges::any_of(candidates, &CopyCandidate::conflicts);
				});
				if (hasConflicts) {
					standalone.confirmUserCopy = {
						copyLabel,
						T("menu.presets.copy_conflict_prompt", "Replace existing User Settings or skip them?"),
						T("menu.presets.copy_skip_existing", "Skip Existing"),
						T("ui.cancel", "Cancel")
					};
					standalone.confirmUserCopy.alternateLabel = T("menu.presets.copy_replace_existing", "Replace Existing");
					standalone.confirmUserCopy.focus = Util::ConfirmationPopup::Focus::Cancel;
					standalone.confirmUserCopy.Request();
				} else {
					CopyPresetSelectionToUser(CopyConflictPolicy::SkipExisting);
				}
			}
			BrowserUI::SameLineIfFits(ImGui::CalcTextSize(destinationLabel).x + 2.0f * ImGui::GetStyle().FramePadding.x);
		}
		if (Util::Button(destinationLabel)) {
			filters.direction = CopyDirection::To;
			session = { .page = source->context, .selection = standalone.selected, .allSourcePeriods = standalone.allSourcePeriods, .pageName = source->displayName, .pageIcon = GetSceneIcon(source->context), .focusSearch = true };
			ResetSelection();
			standalone.destinations.Request();
		}
	}
}

void SceneCopyModal::Open(const SceneContextId& page, std::string_view featureShortName, SceneSettingsManager::SettingSelection selection)
{
	if (!featureShortName.empty())
		filters.direction = CopyDirection::To;
	session = { .page = page,
		.featureShortName = std::string(featureShortName),
		.pageScope = selection,
		.selection = std::move(selection),
		.pageName = SceneSettingsManager::GetSingleton()->GetSceneContextDisplayName(page),
		.pageIcon = GetSceneIcon(page),
		.focusSearch = true };
	if (!featureShortName.empty()) {
		auto* feature = Feature::FindFeatureByShortName(session.featureShortName);
		session.featureName = feature ? feature->GetDisplayName() : session.featureShortName;
		if (GetFeatureCopySettings().empty())
			session.sourceLayer = EntrySource::Overwrite;
	}
	session.modal.Request();
	ResetSelection();
}

void SceneCopyModal::PrepareEmbedded()
{
	standalone = {};
}

void SceneCopyModal::DrawEmbedded()
{
	ImGui::PushID("SceneManagerCopy");
	embedded = true;
	DrawStandaloneSource();
	const auto title = std::format("{}###SceneCopyDestinations", T(TKEY("scene_copy_destinations"), "Destination scenes"));
	standalone.destinations.Draw(title.c_str(), ImGui::GetFontSize() * kModalWidthEm, DrawContents);
	const auto decision = standalone.confirmUserCopy.DrawChoice();
	if (decision == Util::ConfirmationPopup::Choice::Confirm)
		CopyPresetSelectionToUser(CopyConflictPolicy::SkipExisting);
	else if (decision == Util::ConfirmationPopup::Choice::Alternate)
		CopyPresetSelectionToUser(CopyConflictPolicy::OverwriteExisting);
	embedded = false;
	ImGui::PopID();
}

void SceneCopyModal::Draw(const SceneContextId& page, std::string_view featureShortName)
{
	if (!session.modal.IsActive() || session.page != page || session.featureShortName != featureShortName)
		return;
	const auto label = session.featureShortName.empty() ? std::string(T(TKEY("scene_page_copy_title"), "Copy settings")) :
	                                                      I18n::GetSingleton()->Format(TKEY("scene_feature_copy_title"), { { "feature", session.featureName } }, "Copy {feature} settings");
	const auto title = std::format("{}{}", label, kPopupId);
	session.modal.Draw(title.c_str(),
		std::min(ImGui::GetFontSize() * kModalWidthEm, ImGui::GetMainViewport()->WorkSize.x * kMaxViewportWidthShare),
		DrawContents);
}

bool SceneCopyModal::HasSources(const SceneContextId& page, std::string_view featureShortName)
{
	auto* manager = SceneSettingsManager::GetSingleton();
	const auto frame = ImGui::GetFrameCount();
	const SourceCheckKey key{ page, std::string(featureShortName) };
	std::erase_if(sourceChecks, [&](const auto& entry) {
		return entry.first != key && frame - entry.second.lastUsedFrame > kSourceCheckRetentionFrames;
	});
	auto& check = sourceChecks[key];
	check.lastUsedFrame = frame;
	return check.hasSources.Get(manager->GetEntryPresentationRevision(),
		[&] { return !manager->GetCopySources(page, featureShortName).empty(); });
}

#undef I18N_KEY_PREFIX
