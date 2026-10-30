#include "SceneEntryTable.h"

#include "../Browser/BrowserWidgets.h"
#include "IconsTabler.h"
#include "Menu.h"
#include "Menu/Icons/helpers/SceneActionIcons.h"
#include "SceneSettingsContextRules.h"
#include "SceneSettingsInternal.h"
#include "SceneTransitionField.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace
{
	using Manager = SceneSettingsManager;
	using Context = Manager::SceneContextId;
	using Period = Manager::TimeOfDayPeriod;
	using Source = Manager::EntrySource;
	using EntrySource = Source;
	using Entry = Manager::SettingEntry;
	using SettingEntry = Entry;
	using SceneSettingControlType = Manager::SettingControlType;
	using Metadata = SceneSettingsCatalog::SettingMetadata;
	using C = ThemeManager::Constants;
	constexpr int kPeriodCount = Manager::kPeriodCount;
	constexpr int kPeriodlessEntrySlot = 0;
	constexpr float kFloatDragSpeed = 0.01f;
	constexpr float kIntegerDragSpeed = 1.0f;
	constexpr float kSingleValueColumnScale = 1.25f;
	constexpr float kLabelOverflowTolerance = 0.5f;
	constexpr float kActionsColumnMinWidthEm = 4.5f;
	constexpr float kActionControlSpacingCount = 2.0f;
	constexpr float kCompactToggleWidthScale = 1.6f;
	constexpr float kCompactToggleHeightScale = 0.8f;
	constexpr float kSceneFlyoutRounding = 4.0f;
	constexpr float kSceneFlyoutPaddingX = 6.0f;
	constexpr float kSceneFlyoutPaddingY = 3.0f;
	constexpr float kSceneFlyoutBackgroundAlpha = 0.95f;
	constexpr float kStringEditorMaxWidthEm = 16.0f;
	constexpr float kChannelSelectorWidthEm = 4.0f;
	constexpr float kTableBorderWidth = 1.0f;
	constexpr const char* kEllipsis = "...";
	constexpr const char* kNumericWidthSample = "-000.000";
	constexpr std::string_view kDisplaySeparator = " / ";
	int s_addDialogFrame = -1;
	std::uint64_t s_transientGeneration = 1;

	struct TableFlyoutState
	{
		Util::FlyoutState cell;
		Util::FlyoutState row;
		Util::FlyoutState col;
		size_t cellSourceRow = SIZE_MAX;
		size_t rowSourceRow = SIZE_MAX;
		std::uint64_t transientGeneration = 0;
	};

	struct FlyoutResult
	{
		bool toggled = false;
		bool reverted = false;
		bool deleted = false;
	};

	const Metadata* GetMetadata(const Entry& entry)
	{
		return SceneSettingsInternal::FindAllowedCatalogSetting(entry.featureShortName, entry.settingPath, entry.settingKey);
	}

	Context EntryContext(Context context, const Entry& entry)
	{
		context.period = entry.period;
		return context;
	}

	bool IsTransitionEntry(const Entry& entry)
	{
		return entry.value.is_number_float();
	}

	const char* GetPeriodDisplayName(Period period)
	{
		return SceneSettingsContextRules::GetCopyPeriodName(period);
	}

	std::string GetAddPeriodLabel(Period period)
	{
		const auto* name = GetPeriodDisplayName(period);
		return std::vformat(T("feature.scene_manager.action.add_period", "Add {0}"), std::make_format_args(name));
	}

	std::string_view GetVisibleLabel(std::string_view label)
	{
		return label.substr(0, label.find("##"));
	}

	size_t PreviousUtf8CodepointBoundary(std::string_view text, size_t offset)
	{
		if (offset == 0)
			return 0;
		--offset;
		while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80)
			--offset;
		return offset;
	}

	/// Unique setting identifier for TOD table ordering.
	struct SettingId
	{
		std::string feature;
		std::vector<std::string> path;
		std::string key;
		std::string displayName;
		std::string categoryName;
		std::vector<std::string> parentPath;
		std::int8_t componentStart = -1;
		bool operator<(const SettingId& o) const { return std::tie(feature, path, key, componentStart) < std::tie(o.feature, o.path, o.key, o.componentStart); }
	};

	/// One logical setting row with cached entry membership.
	struct SourceRow
	{
		SettingId setting;
		std::array<std::vector<size_t>, kPeriodCount> cells;
		std::vector<size_t> indices;
		std::vector<size_t> addPeriodSourceIndices;
	};

	/// Contiguous rows sharing one category header.
	struct SourceCategoryRange
	{
		size_t begin = 0;
		size_t end = 0;
	};

	/// Cached table model built from a set of entries.
	struct SourceGroup
	{
		std::vector<SourceRow> rows;
		std::vector<SourceCategoryRange> categories;
		std::array<std::vector<size_t>, kPeriodCount> perColumn;
	};

	/// Measured table columns shared by a section header and its settings table.
	struct SourceTableLayout
	{
		int numValueColumns = 1;
		std::array<float, kPeriodCount> valueColumnWidths{};
		float auxiliaryColumnWidth = 0.0f;
		float sectionWidth = 0.0f;
		bool checkboxOnlyValueColumn = false;
	};

	/// Build a SourceGroup from entries, optionally filtered to a single source.
	SourceGroup BuildSourceGroup(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		EntrySource sourceFilter, bool filterBySource = true, bool transitionOnly = false,
		bool multiColumn = false, std::optional<bool> periodMode = std::nullopt);

	/// Remove entries by indices in reverse order.
	void RemoveIndicesReversed(const std::vector<size_t>& indices,
		std::function<void(size_t)> removeFn);

	/// Callbacks for the shared table renderer, abstracting manager operations.
	struct TableCallbacks
	{
		std::function<void(size_t idx, float width, bool readOnly)> drawEditor;
		std::function<void(const std::vector<size_t>& indices, float width, bool readOnly)> drawEditorMulti;
		std::function<void(size_t idx)> togglePause;
		std::function<void(size_t idx)> revert;
		std::function<void(size_t idx)> remove;
		// Optional: called when user clicks + in an empty period cell (multi-column only)
		std::function<void(const std::string& feature, const std::vector<std::string>& path, const std::string& key, int period)> onAddPeriod;
		const char* auxiliaryColumnLabel = nullptr;
		std::function<void(const std::vector<size_t>& indices, float width, bool readOnly)> drawAuxiliary;
	};
	static ImVec2 GetCompactFeatureToggleSize()
	{
		const float frameHeight = ImGui::GetFrameHeight();
		return ImVec2(
			frameHeight * kCompactToggleWidthScale * C::FLYOUT_TOGGLE_SCALE,
			frameHeight * kCompactToggleHeightScale * C::FLYOUT_TOGGLE_SCALE);
	}

	static float GetSceneActionButtonSize()
	{
		return ImGui::GetFrameHeight() * C::FLYOUT_BUTTON_SCALE;
	}
	static std::string GetEntryDisplayName(const SettingEntry& entry)
	{
		return entry.displayName.empty() ? SceneSettingsManager::GetSettingDisplayName(entry.settingKey) : entry.displayName;
	}

	static std::vector<std::string> SplitDisplayName(std::string_view displayName)
	{
		std::vector<std::string> parts;
		if (displayName.empty())
			return parts;
		for (size_t start = 0; start <= displayName.size();) {
			size_t end = displayName.find(kDisplaySeparator, start);
			if (end == std::string_view::npos) {
				parts.emplace_back(displayName.substr(start));
				break;
			}
			parts.emplace_back(displayName.substr(start, end - start));
			start = end + kDisplaySeparator.size();
		}
		return parts;
	}

	static std::string JoinDisplayParts(const std::vector<std::string>& parts)
	{
		std::string result;
		for (const auto& part : parts) {
			if (!result.empty())
				result += kDisplaySeparator;
			result += part;
		}
		return result;
	}
	static bool AreAllPaused(const std::vector<size_t>& indices, const std::vector<SettingEntry>& entries)
	{
		return std::all_of(indices.begin(), indices.end(),
			[&](size_t idx) { return idx < entries.size() && entries[idx].paused; });
	}

	struct FlyoutSource
	{
		ImGuiID id;
		bool pressed;
	};

	static FlyoutSource SubmitFlyoutSource(const char* id, const ImVec2& minimum, const ImVec2& maximum)
	{
		auto* window = ImGui::GetCurrentWindow();
		const ImGuiID itemId = window->GetID(id);
		const ImRect bounds(minimum,
			ImVec2(minimum.x + std::max(1.0f, maximum.x - minimum.x),
				minimum.y + std::max(1.0f, maximum.y - minimum.y)));
		bool pressed = false;
		if (ImGui::ItemAdd(bounds, itemId, nullptr, ImGuiItemFlags_NoNav)) {
			bool hovered = false;
			bool held = false;
			pressed = ImGui::ButtonBehavior(bounds, itemId, &hovered, &held);
		}
		return { itemId, pressed };
	}
	static std::vector<std::string> GetTableParentPath(const std::vector<std::string>& displayPath)
	{
		if (displayPath.empty())
			return {};

		auto first = std::prev(displayPath.end());
		const bool indexedChild = !first->empty() &&
		                          std::ranges::all_of(*first, [](const char ch) { return ch >= '0' && ch <= '9'; });
		if (indexedChild && first != displayPath.begin())
			--first;
		return std::vector<std::string>(first, displayPath.end());
	}

	static SettingId MakeSettingId(const SettingEntry& entry, const std::string& rootCategoryName)
	{
		SceneSettingsManager::SettingControlInfo info;
		if (SceneSettingsManager::GetSettingControlInfo(entry, info)) {
			auto settingName = info.displayName;
			if (info.controlType != SceneSettingControlType::Scalar &&
				!info.componentDisplayName.empty())
				settingName += std::format(" ({})", info.componentDisplayName);
			auto parentPath = GetTableParentPath(info.displayPath);
			return { entry.featureShortName, entry.settingPath, entry.settingKey,
				std::move(settingName), rootCategoryName, std::move(parentPath), -1 };
		}

		auto displayParts = SplitDisplayName(GetEntryDisplayName(entry));
		if (displayParts.empty())
			displayParts.push_back(SceneSettingsManager::GetSettingDisplayName(entry.settingKey));

		auto settingName = displayParts.back();
		displayParts.pop_back();

		auto parentPath = GetTableParentPath(displayParts);
		return { entry.featureShortName, entry.settingPath, entry.settingKey,
			std::move(settingName), rootCategoryName, std::move(parentPath), -1 };
	}

	static SettingId MakeControlSettingId(const SettingEntry& entry,
		const SceneSettingsManager::SettingControlInfo& info, const std::string& rootCategoryName)
	{
		auto parentPath = GetTableParentPath(info.displayPath);
		return {
			entry.featureShortName, info.settingPath, info.settingKey, info.displayName,
			rootCategoryName, std::move(parentPath), info.componentStart
		};
	}

	SourceGroup BuildSourceGroup(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		EntrySource sourceFilter, bool filterBySource, bool transitionOnly, bool multiColumn, std::optional<bool> periodMode)
	{
		SourceGroup group;
		(void)multiColumn;
		std::vector<SettingId> order;
		std::map<SettingId, std::array<size_t, kPeriodCount>> entryMap;
		std::map<SettingId, std::array<std::vector<size_t>, kPeriodCount>> memberMap;
		std::map<std::string, std::string> featureDisplayNames;
		auto getFeatureDisplayName = [&](const std::string& feature) -> const std::string& {
			auto it = featureDisplayNames.find(feature);
			if (it == featureDisplayNames.end())
				it = featureDisplayNames.emplace(feature, SceneSettingsManager::GetFeatureDisplayName(feature)).first;
			return it->second;
		};

		using AggregateKey = std::tuple<
			std::string, std::vector<std::string>, std::string, std::int8_t, std::uint8_t,
			SceneSettingControlType, EntrySource, int>;
		struct AggregateCandidate
		{
			SceneSettingsManager::SettingControlInfo info;
			std::map<std::int8_t, std::vector<size_t>> components;
			bool valid = true;
		};
		std::map<AggregateKey, AggregateCandidate> candidates;
		for (size_t idx = 0; idx < entries.size(); ++idx) {
			const auto& entry = entries[idx];
			if (filterBySource && entry.source != sourceFilter)
				continue;
			if (periodMode && (entry.period != Period::Count) != *periodMode)
				continue;
			if (transitionOnly && !IsTransitionEntry(entry))
				continue;
			int period = static_cast<int>(entry.period);
			if (period < 0)
				continue;
			if (period >= kPeriodCount)
				period = kPeriodlessEntrySlot;
			SceneSettingsManager::SettingControlInfo info;
			if (!SceneSettingsManager::GetSettingControlInfo(entry, info) ||
				info.controlType == SceneSettingControlType::Scalar || info.componentCount < 2 || info.aggregateAll)
				continue;
			auto [candidateIt, inserted] = candidates.try_emplace(AggregateKey{
				entry.featureShortName, info.settingPath, info.settingKey,
				info.componentStart, info.componentCount, info.controlType, entry.source, period });
			auto& candidate = candidateIt->second;
			if (inserted) {
				candidate.info = info;
			} else if (candidate.info.displayName != info.displayName ||
					   candidate.info.displayPath != info.displayPath) {
				candidate.valid = false;
			}
			candidate.components[info.componentIndex].push_back(idx);
		}

		std::map<size_t, SettingId> aggregateSettings;
		for (const auto& [candidateKey, candidate] : candidates) {
			(void)candidateKey;
			bool complete = candidate.valid && candidate.components.size() == candidate.info.componentCount;
			for (size_t component = 0; complete && component < candidate.info.componentCount; ++component)
				complete = candidate.components.contains(
					static_cast<std::int8_t>(candidate.info.componentStart + component));
			std::optional<bool> paused;
			std::optional<bool> deleted;
			for (const auto& [componentIndex, indices] : candidate.components) {
				(void)componentIndex;
				for (const auto index : indices) {
					if (!paused)
						paused = entries[index].paused;
					else if (*paused != entries[index].paused)
						complete = false;
					if (!deleted)
						deleted = entries[index].deleted;
					else if (*deleted != entries[index].deleted)
						complete = false;
				}
			}
			if (!complete)
				continue;
			const auto firstIndex = candidate.components.begin()->second.front();
			const auto setting = MakeControlSettingId(
				entries[firstIndex], candidate.info, getFeatureDisplayName(entries[firstIndex].featureShortName));
			for (const auto& [componentIndex, indices] : candidate.components) {
				(void)componentIndex;
				for (const auto index : indices)
					aggregateSettings.emplace(index, setting);
			}
		}

		for (size_t idx = 0; idx < entries.size(); ++idx) {
			const auto& e = entries[idx];
			if (filterBySource && e.source != sourceFilter)
				continue;
			if (periodMode && (e.period != Period::Count) != *periodMode)
				continue;
			if (transitionOnly && !IsTransitionEntry(e))
				continue;
			int p = static_cast<int>(e.period);
			if (p < 0)
				continue;
			if (p >= kPeriodCount)
				p = kPeriodlessEntrySlot;
			auto aggregate = aggregateSettings.find(idx);
			SettingId setting = aggregate != aggregateSettings.end() ?
			                        aggregate->second :
			                        MakeSettingId(e, getFeatureDisplayName(e.featureShortName));
			auto [it, inserted] = entryMap.try_emplace(setting);
			if (inserted) {
				it->second.fill(SIZE_MAX);
				order.push_back(setting);
			}
			if (it->second[p] == SIZE_MAX)
				it->second[p] = idx;
			memberMap[setting][p].push_back(idx);
		}
		std::sort(order.begin(), order.end());
		group.rows.reserve(order.size());
		for (auto& setting : order) {
			SourceRow row;
			row.setting = std::move(setting);
			const auto& perKey = entryMap.at(row.setting);
			auto members = memberMap.find(row.setting);
			for (int period = 0; period < kPeriodCount; ++period) {
				if (members != memberMap.end() && !members->second[period].empty())
					row.cells[period] = std::move(members->second[period]);
				else if (perKey[period] != SIZE_MAX)
					row.cells[period].push_back(perKey[period]);
				if (!row.cells[period].empty() && row.addPeriodSourceIndices.empty())
					row.addPeriodSourceIndices = row.cells[period];
				row.indices.insert(row.indices.end(), row.cells[period].begin(), row.cells[period].end());
				group.perColumn[period].insert(
					group.perColumn[period].end(), row.cells[period].begin(), row.cells[period].end());
			}
			if (group.categories.empty() ||
				group.rows[group.categories.back().begin].setting.feature != row.setting.feature ||
				group.rows[group.categories.back().begin].setting.categoryName != row.setting.categoryName) {
				if (!group.categories.empty())
					group.categories.back().end = group.rows.size();
				group.categories.push_back({ group.rows.size(), group.rows.size() });
			}
			group.rows.push_back(std::move(row));
		}
		if (!group.categories.empty())
			group.categories.back().end = group.rows.size();
		return group;
	}
	void SplitBySource(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		std::vector<size_t>& overwriteOut, std::vector<size_t>& userOut, bool transitionOnly, std::optional<bool> periodMode)
	{
		for (size_t i = 0; i < entries.size(); ++i) {
			if (periodMode && (entries[i].period != Period::Count) != *periodMode)
				continue;
			if (transitionOnly && !IsTransitionEntry(entries[i]))
				continue;
			(entries[i].source == EntrySource::Overwrite ? overwriteOut : userOut).push_back(i);
		}
	}

	bool DrawNumeric(const Entry& entry, const Metadata& metadata, json& changed, bool expanded)
	{
		double value = 0.0;
		if (!entry.value.is_number() ||
			!SceneSettingsInternal::ConvertCatalogNumericStoredToDisplay(metadata, entry.value.get<double>(), value))
			return false;
		double minimum = 0.0;
		double maximum = 0.0;
		bool bounded = SceneSettingsInternal::GetCatalogNumericDisplayBounds(metadata, minimum, maximum);
		if (!bounded)
			minimum = maximum = 0.0;
		const auto flags = metadata.clampNumericInput ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
		bool edited = false;
		if (metadata.valueType == SceneSettingsCatalog::ValueType::Integer) {
			const auto fits = [](double number) {
				const double limit = std::ldexp(1.0, std::numeric_limits<std::int64_t>::digits);
				return std::isfinite(number) && number >= -limit && number < limit;
			};
			if (!fits(value)) {
				ImGui::TextUnformatted(Util::Settings::FormatValue(entry.value).c_str());
				return false;
			}
			auto integer = static_cast<std::int64_t>(value);
			bounded = bounded && fits(minimum) && fits(maximum);
			const auto low = bounded ? static_cast<std::int64_t>(std::ceil(minimum)) : 0;
			const auto high = bounded ? static_cast<std::int64_t>(std::floor(maximum)) : 0;
			bounded &= low <= high;
			edited = expanded && bounded ?
			             ImGui::SliderScalar("##value", ImGuiDataType_S64, &integer, &low, &high, "%lld", flags) :
			             ImGui::DragScalar("##value", ImGuiDataType_S64, &integer, kIntegerDragSpeed,
							 bounded ? &low : nullptr, bounded ? &high : nullptr, "%lld", flags);
			value = static_cast<double>(integer);
		} else {
			float number = static_cast<float>(value);
			const float low = static_cast<float>(minimum);
			const float high = static_cast<float>(maximum);
			const float displayScale = static_cast<float>(SceneSettingsInternal::GetCatalogNumericDisplayScale(metadata));
			const float speed = kFloatDragSpeed * displayScale;
			const char* format = displayScale == 1.0f ? "%.3f" : "%.1f";
			edited = expanded && bounded ? ImGui::SliderFloat("##value", &number, low, high, format, flags) :
			                               ImGui::DragFloat("##value", &number, speed, low, high, format, flags);
			value = number;
		}
		if (!edited || !std::isfinite(value))
			return false;
		double stored = 0.0;
		if (!SceneSettingsInternal::ConvertCatalogNumericDisplayToStored(metadata, value, stored))
			return false;
		if (metadata.valueType == SceneSettingsCatalog::ValueType::Integer &&
			(stored < -std::ldexp(1.0, std::numeric_limits<std::int64_t>::digits) ||
				stored >= std::ldexp(1.0, std::numeric_limits<std::int64_t>::digits)))
			return false;
		changed = metadata.valueType == SceneSettingsCatalog::ValueType::Integer ? json(static_cast<std::int64_t>(stored)) : json(stored);
		return true;
	}

	bool DrawScalar(const Entry& entry, const Metadata& metadata, json& changed, bool expanded)
	{
		using enum SceneSettingsCatalog::ValueType;
		const bool toggle = metadata.valueType == Boolean ||
		                    SceneSettingsCatalog::HasFlag(metadata.flags, SceneSettingsCatalog::SettingFlag::BooleanControl);
		if (toggle) {
			const bool stored = entry.value.is_boolean() ? entry.value.get<bool>() : entry.value.get<std::int64_t>() != 0;
			bool value = stored != metadata.invertedDisplay;
			if (!ImGui::Checkbox("##value", &value))
				return false;
			value = value != metadata.invertedDisplay;
			changed = entry.value.is_boolean() ? json(value) : json(value ? 1 : 0);
			return true;
		}
		if (metadata.choiceCount != 0) {
			bool edited = false;
			const auto choiceLabel = [&metadata](size_t index) {
				if (!metadata.choiceDisplay)
					return std::to_string(metadata.choices[index]);
				const auto& display = metadata.choiceDisplay[index];
				return Util::Settings::StripImGuiId(display.displayNameKey.empty() ? display.displayName :
																					 T(display.displayNameKey, display.displayName.data()));
			};
			auto preview = Util::Settings::FormatValue(entry.value);
			for (size_t index = 0; index < metadata.choiceCount; ++index)
				if (entry.value == metadata.choices[index])
					preview = choiceLabel(index);
			if (ImGui::BeginCombo("##value", preview.c_str())) {
				for (size_t index = 0; index < metadata.choiceCount; ++index) {
					const auto choice = metadata.choices[index];
					if (Util::RoundedSelectable(choiceLabel(index).c_str(), entry.value == choice)) {
						changed = choice;
						edited = true;
					}
				}
				ImGui::EndCombo();
			}
			return edited;
		}
		if (metadata.valueType == Float || metadata.valueType == Integer)
			return DrawNumeric(entry, metadata, changed, expanded);
		if (metadata.valueType == String) {
			auto value = entry.value.get<std::string>();
			if (ImGui::InputText("##value", &value)) {
				changed = std::move(value);
				return true;
			}
		}
		return false;
	}

	bool DrawAggregate(std::span<const Entry> entries, const std::vector<size_t>& indices,
		std::vector<Manager::EntryValueUpdate>& updates, bool expanded)
	{
		if (indices.size() < 2 || indices.size() > 4)
			return false;
		auto ordered = indices;
		std::ranges::sort(ordered, {}, [&](size_t index) {
			const auto* metadata = GetMetadata(entries[index]);
			return metadata ? metadata->serializedComponent : -1;
		});
		std::array<float, 4> values{};
		const auto* first = GetMetadata(entries[ordered.front()]);
		if (!first || first->aggregateCount != ordered.size())
			return false;
		for (size_t component = 0; component < ordered.size(); ++component) {
			const auto& entry = entries[ordered[component]];
			const auto* metadata = GetMetadata(entry);
			if (!metadata || !entry.value.is_number() || entry.paused != entries[ordered.front()].paused ||
				metadata->serializedComponent != first->aggregateStart + component)
				return false;
			double value = 0.0;
			if (!SceneSettingsInternal::ConvertCatalogNumericStoredToDisplay(*metadata, entry.value.get<double>(), value))
				return false;
			values[component] = static_cast<float>(value);
		}
		bool edited = false;
		if (first->aggregateSemantic == SceneSettingsCatalog::AggregateSemantic::Color && ordered.size() >= 3) {
			const auto flags = ImGuiColorEditFlags_Float | (first->hdrColor ? ImGuiColorEditFlags_HDR : 0) |
			                   (expanded ? 0 : ImGuiColorEditFlags_NoInputs);
			edited = ordered.size() == 3 ? ImGui::ColorEdit3("##value", values.data(), flags) :
			                               ImGui::ColorEdit4("##value", values.data(), flags);
		} else {
			edited = ImGui::DragScalarN("##value", ImGuiDataType_Float, values.data(), static_cast<int>(ordered.size()), kFloatDragSpeed);
		}
		if (edited) {
			for (size_t component = 0; component < ordered.size(); ++component) {
				double stored = 0.0;
				if (!SceneSettingsInternal::ConvertCatalogNumericDisplayToStored(*GetMetadata(entries[ordered[component]]), values[component], stored)) {
					updates.clear();
					return true;
				}
				updates.push_back({ ordered[component], stored });
			}
		}
		return true;
	}

	void DrawEntryValue(const Context& context, std::span<const Entry> entries, const std::vector<size_t>& indices, Source source, bool expanded)
	{
		if (indices.empty())
			return;
		const auto& entry = entries[indices.front()];
		const auto* metadata = GetMetadata(entry);
		if (entry.deleted) {
			Util::Text::Disabled("%s", entry.paused ? T("feature.scene_manager.status.paused", "Paused") :
													  T("feature.scene_manager.entry_removed", "Removed"));
			return;
		}
		const bool readOnly = source == Source::Overwrite || !metadata ||
		                      Manager::GetSingleton()->IsContextEditTarget(context, entry.featureShortName);
		const Util::DisableGuard disabled(readOnly);
		ImGui::SetNextItemWidth(-FLT_MIN);
		std::vector<Manager::EntryValueUpdate> updates;
		if (!DrawAggregate(entries, indices, updates, expanded)) {
			if (indices.size() == 1 && metadata) {
				json value;
				if (DrawScalar(entry, *metadata, value, expanded))
					updates.push_back({ indices.front(), std::move(value) });
			} else {
				for (size_t component = 0; component < indices.size(); ++component) {
					ImGui::PushID(static_cast<int>(component));
					const auto& scalar = entries[indices[component]];
					const auto* scalarMetadata = GetMetadata(scalar);
					json value;
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (scalarMetadata && DrawScalar(scalar, *scalarMetadata, value, expanded))
						updates.push_back({ indices[component], std::move(value) });
					else if (!scalarMetadata)
						ImGui::TextUnformatted(Util::Settings::FormatValue(scalar.value).c_str());
					ImGui::PopID();
				}
			}
		}
		if (!readOnly && !updates.empty())
			Manager::GetSingleton()->UpdateContextEntryValues(context, updates, true);
		if (ImGui::IsItemActive())
			Manager::GetSingleton()->HoldDeferredSceneChanges();
		if (entry.paused)
			Util::AddTooltip(T("feature.scene_manager.action.resume", "Resume"));
	}
	void DrawLocationTransition(const Context& context, const std::vector<Entry>& entries,
		const std::vector<size_t>& indices, Source source)
	{
		if (indices.empty() || std::ranges::any_of(indices, [&](size_t index) {
				return entries[index].deleted || !entries[index].value.is_number() ||
				       !SceneSettingsInternal::FindAllowedCatalogSetting(entries[index].featureShortName,
						   entries[index].settingPath, entries[index].settingKey, true);
			})) {
			ImGui::TextDisabled("--");
			return;
		}
		auto* manager = Manager::GetSingleton();
		auto seconds = manager->GetLocationEntryTransitionSeconds(context.locationType, context.locationFormKey, indices.front());
		const bool mixed = std::ranges::any_of(indices, [&](size_t index) {
			return entries[index].transitionSeconds != seconds;
		});
		const bool editable = source == Source::User && std::ranges::none_of(indices, [&](size_t index) {
			return manager->IsContextEditTarget(EntryContext(context, entries[index]), entries[index].featureShortName);
		});
		if (SceneTransitionField::Draw("##LocationTransition", seconds,
				manager->GetTransitionControl(Manager::TransitionControl::LocationSeconds), editable, mixed, ImGui::GetContentRegionAvail().x))
			manager->SetLocationEntryTransitionSeconds(context.locationType, context.locationFormKey, indices, seconds);
	}

	void RemoveIndicesReversed(const std::vector<size_t>& indices, std::function<void(size_t)> removeFn)
	{
		auto sorted = indices;
		std::ranges::sort(sorted, std::greater<>{});
		sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
		for (const auto index : sorted)
			removeFn(index);
	}

	bool OwnsRetainedEdit(const Context& context, const std::vector<Entry>& entries, const std::vector<size_t>& indices)
	{
		return std::ranges::any_of(indices, [&](size_t index) {
			return Manager::GetSingleton()->IsContextEditTarget(EntryContext(context, entries[index]), entries[index].featureShortName);
		});
	}

	FlyoutResult DrawFlyoutControls(bool paused, bool isGroup, bool isOverwrite, const Context& context,
		const std::vector<Entry>& entries, const std::vector<size_t>& indices)
	{
		FlyoutResult result;
		const Util::DisableGuard retained(OwnsRetainedEdit(context, entries, indices));
		const float buttonH = GetSceneActionButtonSize();
		const ImVec2 toggleSize = GetCompactFeatureToggleSize();
		const float toggleH = toggleSize.y;
		const float toggleOffsetY = (buttonH - toggleH) * 0.5f;

		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + toggleOffsetY));
		bool active = !paused;
		result.toggled = Util::FeatureToggle(isGroup ? "##groupActive" : "##active", &active, toggleSize);
		Util::AddTooltip(paused ? T("feature.scene_manager.action.resume", "Resume") : T("feature.scene_manager.action.pause", "Pause"));

		ImGui::SameLine();
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cursor.y));
		{
			const Util::DisableGuard disable(isOverwrite);
			result.reverted = Icons::Button("##revert", Icons::TI(ICON_TI_ERASER), ImVec2(buttonH, buttonH));
		}
		Util::AddTooltip(T("feature.scene_manager.action.revert", "Revert to default"), Util::kTooltipWhenDisabled);

		ImGui::SameLine();
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cursor.y));
		result.deleted = Icons::Button("##remove", SceneActionIcons::kDelete, ImVec2(buttonH, buttonH));
		Util::AddTooltip(isOverwrite ? T("feature.scene_manager.entry_suppress_tooltip", "Remove this context's value and use the settings beneath it.") :
									   T("feature.scene_manager.action.remove", "Remove"));
		return result;
	}

	void ApplyGroupControlResult(const FlyoutResult& result, const std::vector<size_t>& indices, bool allPaused,
		const std::vector<Entry>& entries, const TableCallbacks& callbacks, Util::FlyoutState* flyout)
	{
		if (result.toggled)
			for (const auto index : indices)
				if (entries[index].paused == allPaused)
					callbacks.togglePause(index);
		if (result.reverted)
			for (const auto index : indices)
				callbacks.revert(index);
		if (!result.deleted)
			return;
		RemoveIndicesReversed(indices, callbacks.remove);
		if (flyout)
			Util::CloseFlyout(*flyout);
	}

	float GetActionsColumnWidth()
	{
		const float controlWidth = GetCompactFeatureToggleSize().x + GetSceneActionButtonSize() * 2.0f +
		                           ImGui::GetStyle().ItemSpacing.x * kActionControlSpacingCount;
		return std::max(C::Em(kActionsColumnMinWidthEm), controlWidth);
	}

	bool HasInlineActionColumn(int valueColumns)
	{
		return valueColumns == 1;
	}

	float GetMinimumValueColumnWidth(int valueColumns)
	{
		const float width = C::Em(C::SCENE_TOD_PERIOD_COL_EM);
		return valueColumns > 1 ? width : width * kSingleValueColumnScale;
	}

	float GetExpandedValueColumnWidth()
	{
		float componentWidth = 0.0f;
		for (const char* component : { "R", "G", "B", "A" }) {
			const auto sample = std::format("{} {}", kNumericWidthSample, component);
			componentWidth = std::max(componentWidth, ImGui::CalcTextSize(sample.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f);
		}
		return componentWidth * 4.0f + ImGui::GetStyle().ItemInnerSpacing.x * 4.0f + ImGui::GetFrameHeight();
	}

	bool IsCheckboxOnlyGroup(const SourceGroup& group, const std::vector<Entry>& entries)
	{
		return !group.rows.empty() && std::ranges::all_of(group.rows, [&](const auto& row) {
			return std::ranges::all_of(row.indices, [&](size_t index) {
				const auto* metadata = GetMetadata(entries[index]);
				return metadata && metadata->choiceCount == 0 && (metadata->valueType == SceneSettingsCatalog::ValueType::Boolean || SceneSettingsCatalog::HasFlag(metadata->flags, SceneSettingsCatalog::SettingFlag::BooleanControl));
			});
		});
	}

	float MeasureValueEditorWidth(const std::vector<Entry>& entries, const std::vector<size_t>& indices,
		float minimum, bool expanded)
	{
		float width = minimum;
		for (const auto index : indices) {
			const auto* metadata = GetMetadata(entries[index]);
			if (!metadata)
				continue;
			for (size_t choice = 0; choice < metadata->choiceCount; ++choice) {
				auto label = std::to_string(metadata->choices[choice]);
				if (metadata->choiceDisplay) {
					const auto& display = metadata->choiceDisplay[choice];
					label = Util::Settings::StripImGuiId(display.displayNameKey.empty() ? display.displayName :
																						  T(display.displayNameKey, display.displayName.data()));
				}
				width = std::max(width, ImGui::CalcTextSize(label.c_str()).x +
											ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight());
			}
			if (metadata->valueType == SceneSettingsCatalog::ValueType::String)
				width = std::max(width, C::Em(kStringEditorMaxWidthEm));
			if (expanded && metadata->aggregateCount > 1)
				width = std::max(width, GetExpandedValueColumnWidth());
		}
		return width;
	}

	void CenterCursorY(float rowHeight, float itemHeight)
	{
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (rowHeight - itemHeight) * 0.5f));
	}

	void DrawInlineControls(const Context& context, const std::vector<size_t>& indices, bool overwrite,
		const std::vector<Entry>& entries, const TableCallbacks& callbacks)
	{
		if (indices.empty())
			return;
		const bool paused = AreAllPaused(indices, entries);
		const auto result = DrawFlyoutControls(paused, indices.size() > 1, overwrite, context, entries, indices);
		ApplyGroupControlResult(result, indices, paused, entries, callbacks, nullptr);
	}
	static int GetSettingLabelMaxLines()
	{
		return static_cast<int>(C::SCENE_SETTING_MAX_LINES);
	}

	static std::string GetSettingLabel(const SettingId& setting)
	{
		return setting.displayName.empty() ? SceneSettingsManager::GetSettingDisplayName(setting.key) : setting.displayName;
	}

	static float GetSettingLabelVisualHeight()
	{
		return ImGui::GetTextLineHeight() * C::SCENE_SETTING_MAX_LINES;
	}

	static std::string TruncateTextToFitWidth(std::string text, float width)
	{
		if (text.empty() || width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= width)
			return text;

		size_t visibleLen = text.size();
		while (visibleLen > 0 &&
			   ImGui::CalcTextSize((text.substr(0, visibleLen) + kEllipsis).c_str()).x > width)
			visibleLen = PreviousUtf8CodepointBoundary(text, visibleLen);
		return text.substr(0, visibleLen) + kEllipsis;
	}

	static std::string TruncateTextMiddleToFitWidth(std::string text, float width)
	{
		if (text.empty() || width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= width)
			return text;

		std::vector<size_t> boundaries{ 0 };
		for (size_t offset = 0; offset < text.size();) {
			++offset;
			while (offset < text.size() &&
				   (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80)
				++offset;
			boundaries.push_back(offset);
		}
		for (size_t visible = boundaries.size() - 1; visible > 0; --visible) {
			const auto prefixCount = (visible + 1) / 2;
			const auto suffixCount = visible / 2;
			auto candidate = text.substr(0, boundaries[prefixCount]) + kEllipsis +
			                 text.substr(boundaries[boundaries.size() - 1 - suffixCount]);
			if (ImGui::CalcTextSize(candidate.c_str()).x <= width)
				return candidate;
		}
		return ImGui::CalcTextSize(kEllipsis).x <= width ? std::string(kEllipsis) : std::string{};
	}

	static std::string TruncateWrappedTextToLines(std::string text, float wrapWidth, int maxLines)
	{
		assert(maxLines > 0);
		if (text.empty() || wrapWidth <= 0.0f)
			return text;

		const float fixedH = ImGui::GetTextLineHeight() * maxLines;
		if (ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapWidth).y <= fixedH + kLabelOverflowTolerance)
			return text;

		auto* font = ImGui::GetFont();
		const float fontSize = ImGui::GetFontSize();
		const char* textBegin = text.c_str();
		const char* textEnd = textBegin + text.size();
		if (maxLines == 1)
			return TruncateTextMiddleToFitWidth(std::move(text), wrapWidth);

		const char* lineStart = textBegin;
		for (int line = 0; line < maxLines - 1 && lineStart < textEnd; ++line) {
			const char* nextLine = font->CalcWordWrapPosition(fontSize, lineStart, textEnd, wrapWidth);
			if (nextLine <= lineStart)
				break;
			lineStart = nextLine;
		}

		auto prefix = text.substr(0, static_cast<size_t>(lineStart - textBegin));
		while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back())))
			prefix.pop_back();
		auto suffix = text.substr(static_cast<size_t>(lineStart - textBegin));
		suffix.erase(0, suffix.find_first_not_of(" \t\r\n"));
		return std::format("{}\n{}", prefix,
			TruncateTextMiddleToFitWidth(std::move(suffix), wrapWidth));
	}

	static ImRect DrawSettingLabel(const SettingId& setting)
	{
		const float wrapWidth = ImGui::GetContentRegionAvail().x;
		const float fixedH = GetSettingLabelVisualHeight();

		if (setting.parentPath.empty()) {
			auto text = TruncateWrappedTextToLines(GetSettingLabel(setting), wrapWidth, GetSettingLabelMaxLines());
			ImGui::TextWrapped("%s", text.c_str());
		} else {
			auto parent = TruncateTextToFitWidth(JoinDisplayParts(setting.parentPath), wrapWidth);
			auto leaf = TruncateTextMiddleToFitWidth(GetSettingLabel(setting), wrapWidth);
			auto text = std::format("{}\n{}", parent, leaf);
			ImGui::TextUnformatted(text.c_str());
		}

		const ImRect labelRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		const float usedH = ImGui::GetItemRectSize().y;
		if (usedH < fixedH) {
			const float pad = fixedH - usedH - ImGui::GetStyle().ItemSpacing.y;
			if (pad > 0.0f)
				ImGui::Dummy(ImVec2(0, pad));
		}
		return labelRect;
	}
	static SourceTableLayout GetSourceTableLayout(const SourceGroup& group,
		const std::vector<SettingEntry>& entries, int numValueColumns,
		bool showExpandedAggregateControls = false, float auxiliaryColumnWidth = 0.0f)
	{
		SourceTableLayout layout;
		layout.numValueColumns = std::clamp(numValueColumns, 1, kPeriodCount);
		layout.auxiliaryColumnWidth = std::max(0.0f, auxiliaryColumnWidth);
		const bool multiColumn = layout.numValueColumns > 1;
		layout.checkboxOnlyValueColumn = !multiColumn && IsCheckboxOnlyGroup(group, entries);
		const float regularMinimumWidth = !multiColumn && showExpandedAggregateControls ?
		                                      std::max(GetMinimumValueColumnWidth(layout.numValueColumns),
												  GetExpandedValueColumnWidth()) :
		                                      GetMinimumValueColumnWidth(layout.numValueColumns);
		const float minimumWidth = layout.checkboxOnlyValueColumn ?
		                               ImGui::GetFrameHeight() :
		                               regularMinimumWidth;
		std::fill_n(layout.valueColumnWidths.begin(), layout.numValueColumns, minimumWidth);
		if (multiColumn)
			for (int column = 0; column < layout.numValueColumns; ++column)
				layout.valueColumnWidths[column] = std::max(layout.valueColumnWidths[column],
					ImGui::CalcTextSize(GetPeriodDisplayName(static_cast<Period>(column))).x);
		if (!multiColumn && !layout.checkboxOnlyValueColumn)
			for (const auto& row : group.rows)
				layout.valueColumnWidths[0] = std::max(layout.valueColumnWidths[0],
					MeasureValueEditorWidth(entries, row.indices, minimumWidth, showExpandedAggregateControls));

		auto& style = ImGui::GetStyle();
		const bool inlineActions = HasInlineActionColumn(layout.numValueColumns);
		const bool hasAuxiliaryColumn = layout.auxiliaryColumnWidth > 0.0f;
		const int totalColumns = 1 + layout.numValueColumns + (hasAuxiliaryColumn ? 1 : 0) +
		                         (inlineActions ? 1 : 0);
		float columnWidth = C::Em(C::SCENE_TOD_PARAM_COL_EM);
		for (int column = 0; column < layout.numValueColumns; ++column)
			columnWidth += layout.valueColumnWidths[column];
		if (hasAuxiliaryColumn)
			columnWidth += layout.auxiliaryColumnWidth;
		if (inlineActions)
			columnWidth += GetActionsColumnWidth();
		layout.sectionWidth = columnWidth + totalColumns * style.CellPadding.x * 2.0f +
		                      (totalColumns + 1) * kTableBorderWidth;
		return layout;
	}
	struct CachedSourceTable
	{
		SourceGroup group;
		SourceTableLayout layout;
		std::vector<size_t> indices;
		bool allPaused = false;
	};

	struct SourcePanelCache
	{
		std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
		std::string locale;
		const ImFont* font = nullptr;
		float fontSize = 0.0f;
		ImVec2 framePadding{};
		ImVec2 itemSpacing{};
		ImVec2 itemInnerSpacing{};
		ImVec2 cellPadding{};
		int numValueColumns = 0;
		bool showExpandedAggregateControls = false;
		bool transitionOnly = false;
		std::optional<bool> periodMode;
		float auxiliaryColumnWidth = 0.0f;
		CachedSourceTable overwrite;
		CachedSourceTable user;
	};

	static bool EqualStyleMetric(const ImVec2& lhs, const ImVec2& rhs)
	{
		return lhs.x == rhs.x && lhs.y == rhs.y;
	}

	static void RefreshSourcePanelCache(SourcePanelCache& cache,
		const std::vector<SettingEntry>& entries, int numValueColumns,
		bool showExpandedAggregateControls = false, bool transitionOnly = false,
		float auxiliaryColumnWidth = 0.0f, std::optional<bool> periodMode = std::nullopt)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto revision = manager->GetEntryPresentationRevision();
		const auto locale = I18n::GetSingleton()->GetCurrentLocale();
		const auto* font = ImGui::GetFont();
		const auto fontSize = ImGui::GetFontSize();
		const auto& style = ImGui::GetStyle();
		const int clampedColumns = std::clamp(numValueColumns, 1, kPeriodCount);
		if (cache.revision == revision && cache.locale == locale &&
			cache.font == font && cache.fontSize == fontSize &&
			EqualStyleMetric(cache.framePadding, style.FramePadding) &&
			EqualStyleMetric(cache.itemSpacing, style.ItemSpacing) &&
			EqualStyleMetric(cache.itemInnerSpacing, style.ItemInnerSpacing) &&
			EqualStyleMetric(cache.cellPadding, style.CellPadding) &&
			cache.numValueColumns == clampedColumns &&
			cache.showExpandedAggregateControls == showExpandedAggregateControls &&
			cache.transitionOnly == transitionOnly && cache.periodMode == periodMode &&
			cache.auxiliaryColumnWidth == auxiliaryColumnWidth)
			return;

		const bool multiColumn = clampedColumns > 1;
		cache.overwrite.group = BuildSourceGroup(
			entries, EntrySource::Overwrite, true, transitionOnly, multiColumn, periodMode);
		cache.user.group = BuildSourceGroup(
			entries, EntrySource::User, true, transitionOnly, multiColumn, periodMode);
		cache.overwrite.layout = GetSourceTableLayout(
			cache.overwrite.group, entries, clampedColumns, showExpandedAggregateControls,
			auxiliaryColumnWidth);
		cache.user.layout = GetSourceTableLayout(
			cache.user.group, entries, clampedColumns, showExpandedAggregateControls,
			auxiliaryColumnWidth);
		if (!multiColumn &&
			cache.overwrite.layout.checkboxOnlyValueColumn ==
				cache.user.layout.checkboxOnlyValueColumn) {
			const float commonValueWidth = std::max(
				cache.overwrite.layout.valueColumnWidths[0], cache.user.layout.valueColumnWidths[0]);
			for (auto* layout : { &cache.overwrite.layout, &cache.user.layout }) {
				layout->sectionWidth += commonValueWidth - layout->valueColumnWidths[0];
				layout->valueColumnWidths[0] = commonValueWidth;
			}
		}
		cache.overwrite.indices.clear();
		cache.user.indices.clear();
		SplitBySource(entries, cache.overwrite.indices, cache.user.indices, transitionOnly, periodMode);
		cache.overwrite.allPaused = AreAllPaused(cache.overwrite.indices, entries);
		cache.user.allPaused = AreAllPaused(cache.user.indices, entries);

		cache.revision = revision;
		cache.locale = locale;
		cache.font = font;
		cache.fontSize = fontSize;
		cache.framePadding = style.FramePadding;
		cache.itemSpacing = style.ItemSpacing;
		cache.itemInnerSpacing = style.ItemInnerSpacing;
		cache.cellPadding = style.CellPadding;
		cache.numValueColumns = clampedColumns;
		cache.showExpandedAggregateControls = showExpandedAggregateControls;
		cache.transitionOnly = transitionOnly;
		cache.periodMode = periodMode;
		cache.auxiliaryColumnWidth = auxiliaryColumnWidth;
	}
	void DrawSourceTable(
		const Context& context,
		const SourceGroup& group,
		const std::vector<SceneSettingsManager::SettingEntry>& entries,
		const char* tableId,
		EntrySource source,
		const SourceTableLayout& layout,
		TableFlyoutState& flyout,
		const TableCallbacks& cb)
	{
		const int numValueColumns = layout.numValueColumns;
		bool isOverwrite = source == EntrySource::Overwrite;
		bool multiColumn = numValueColumns > 1;
		bool inlineActions = HasInlineActionColumn(numValueColumns);
		const bool hasAuxiliaryColumn = layout.auxiliaryColumnWidth > 0.0f &&
		                                cb.auxiliaryColumnLabel && cb.drawAuxiliary;
		if (flyout.transientGeneration != s_transientGeneration) {
			Util::CloseFlyout(flyout.cell);
			Util::CloseFlyout(flyout.row);
			Util::CloseFlyout(flyout.col);
			flyout.cellSourceRow = SIZE_MAX;
			flyout.rowSourceRow = SIZE_MAX;
			flyout.transientGeneration = s_transientGeneration;
		}
		const bool suppressFlyouts = s_addDialogFrame == ImGui::GetFrameCount();
		if (suppressFlyouts) {
			Util::CloseFlyout(flyout.cell);
			Util::CloseFlyout(flyout.row);
			Util::CloseFlyout(flyout.col);
			flyout.cellSourceRow = SIZE_MAX;
			flyout.rowSourceRow = SIZE_MAX;
		}
		const auto flyoutStyle = SceneEntryTable::GetActionFlyoutStyle();
		constexpr int kSettingColumn = 0;
		constexpr int kFirstValueColumn = 1;
		const int auxiliaryColumn = kFirstValueColumn + numValueColumns;
		const int actionsColumn = auxiliaryColumn + (hasAuxiliaryColumn ? 1 : 0);
		int totalCols = actionsColumn + (inlineActions ? 1 : 0);
		struct PendingAddPeriod
		{
			std::string feature;
			std::vector<std::string> path;
			std::string key;
			int period;
		};
		std::vector<size_t> pendingRemoveIndices;
		std::vector<PendingAddPeriod> pendingAddPeriods;
		auto deferredCallbacks = cb;
		deferredCallbacks.remove = [&](size_t index) {
			if (std::find(pendingRemoveIndices.begin(), pendingRemoveIndices.end(), index) == pendingRemoveIndices.end())
				pendingRemoveIndices.push_back(index);
		};
		if (cb.onAddPeriod) {
			deferredCallbacks.onAddPeriod = [&](const std::string& feature, const std::vector<std::string>& path,
												const std::string& key, int period) {
				pendingAddPeriods.push_back({ feature, path, key, period });
			};
		}

		const ImGuiTableFlags tableFlags =
			ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
		if (!ImGui::BeginTable(tableId, totalCols, tableFlags, ImVec2(layout.sectionWidth, 0.0f)))
			return;

		// Column setup
		bool columnWidthsChanged = false;
		const auto setupColumn = [&](const char* label, float width) {
			const auto* table = GImGui->CurrentTable;
			columnWidthsChanged |= table->Columns[table->DeclColumnsCount].InitStretchWeightOrWidth != width;
			ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthFixed, width);
		};
		setupColumn(T("feature.scene_manager.column.setting", "Setting"), C::Em(C::SCENE_TOD_PARAM_COL_EM));
		if (multiColumn) {
			for (int i = 0; i < numValueColumns; ++i)
				setupColumn(GetPeriodDisplayName(static_cast<Period>(i)), layout.valueColumnWidths[i]);
		} else {
			setupColumn(T("feature.scene_manager.column.value", "Value"), layout.valueColumnWidths[0]);
		}
		if (hasAuxiliaryColumn)
			setupColumn(cb.auxiliaryColumnLabel, layout.auxiliaryColumnWidth);
		if (inlineActions)
			setupColumn(T("feature.scene_manager.column.actions", "Actions"), GetActionsColumnWidth());
		// Previously clipped columns also need their new measured widths applied.
		if (columnWidthsChanged)
			ImGui::TableSetColumnWidthAutoAll(GImGui->CurrentTable);

		if (multiColumn || hasAuxiliaryColumn) {
			// Header row
			auto* table = GImGui->CurrentTable;
			if (!table->IsLayoutLocked)
				ImGui::TableUpdateLayout(table);
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers, ImGui::TableGetHeaderRowHeight());
			ImGui::TableSetColumnIndex(kSettingColumn);
			ImGui::TableHeader(T("feature.scene_manager.column.setting", "Setting"));

			if (!multiColumn) {
				ImGui::TableSetColumnIndex(kFirstValueColumn);
				ImGui::TableHeader(T("feature.scene_manager.column.value", "Value"));
			}
			for (int i = 0; multiColumn && i < numValueColumns; ++i) {
				const bool columnVisible = ImGui::TableSetColumnIndex(kFirstValueColumn + i);
				if (!columnVisible && !flyout.col.isOpen)
					continue;
				const ImVec2 contentMin = ImGui::GetCursorScreenPos();
				float colW = ImGui::GetContentRegionAvail().x;
				ImGui::TextUnformatted(GetPeriodDisplayName(static_cast<Period>(i)));
				const ImVec2 contentMax(contentMin.x + colW, ImGui::GetItemRectMax().y);
				const ImRect cellRect = ImGui::TableGetCellBgRect(table, kFirstValueColumn + i);

				const auto& indices = group.perColumn[i];
				if (!indices.empty() && !suppressFlyouts) {
					ImGui::PushID(i);
					const auto flyoutSource = SubmitFlyoutSource("##colFlyout", contentMin, contentMax);
					{
						Util::FlyoutScope flyoutScope(
							flyout.col, flyoutSource.id, flyoutSource.pressed,
							cellRect.Min, cellRect.Max, flyoutStyle);
						if (flyoutScope) {
							bool allPaused = AreAllPaused(indices, entries);
							auto result = DrawFlyoutControls(allPaused, true, isOverwrite, context, entries, indices);
							ApplyGroupControlResult(result, indices, allPaused,
								entries, deferredCallbacks, &flyout.col);
						}
					}
					ImGui::PopID();
				}
			}
			if (hasAuxiliaryColumn) {
				ImGui::TableSetColumnIndex(auxiliaryColumn);
				ImGui::TableHeader(cb.auxiliaryColumnLabel);
			}
			if (inlineActions) {
				ImGui::TableSetColumnIndex(actionsColumn);
				ImGui::TableHeader(T("feature.scene_manager.column.actions", "Actions"));
			}
		}

		auto& theme = globals::menu->GetSettings().Theme;
		const auto drawRow = [&](const SourceRow& row, size_t rowIndex) {
			const auto& sid = row.setting;
			const auto& rowIndices = row.indices;

			const float controlHeight = ImGui::GetFrameHeight();
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(kSettingColumn);

			ImGui::PushID(sid.key.c_str());
			ImGui::PushID(static_cast<int>(sid.componentStart));
			for (const auto& part : sid.path)
				ImGui::PushID(part.c_str());
			ImGui::PushID(sid.feature.c_str());

			ImGui::Indent(C::Em(C::SCENE_ENTRY_INDENT_EM));
			ImGui::SetWindowFontScale(C::SCENE_TOD_FEATURE_TEXT_SCALE);
			const auto labelRect = DrawSettingLabel(sid);
			const float labelVisualH = GetSettingLabelVisualHeight();
			ImGui::SetWindowFontScale(1.0f);
			const float rowVisualH = std::max(labelVisualH, controlHeight);

			// Row-level flyout (only for multi-column to avoid duplicate controls)
			if (multiColumn && !suppressFlyouts) {
				const auto flyoutSource = SubmitFlyoutSource("##rowFlyout", labelRect.Min, labelRect.Max);
				{
					Util::FlyoutScope flyoutScope(
						flyout.row, flyoutSource.id, flyoutSource.pressed, flyoutStyle);
					if (flyout.row.isOpen && flyout.row.activeId == flyoutSource.id)
						flyout.rowSourceRow = rowIndex;
					if (flyoutScope) {
						bool allPaused = AreAllPaused(rowIndices, entries);
						auto result = DrawFlyoutControls(allPaused, true, isOverwrite, context, entries, rowIndices);
						ApplyGroupControlResult(result, rowIndices, allPaused,
							entries, deferredCallbacks, &flyout.row);
					}
				}
			}

			ImGui::Unindent(C::Em(C::SCENE_ENTRY_INDENT_EM));

			// Value columns
			if (multiColumn) {
				for (int p = 0; p < numValueColumns; ++p) {
					const bool columnVisible = ImGui::TableSetColumnIndex(kFirstValueColumn + p);
					const auto& cellIndices = row.cells[p];
					if (!columnVisible) {
						bool ownsActiveFlyout = false;
						if (flyout.cell.isOpen && !cellIndices.empty()) {
							ImGui::PushID(p);
							ownsActiveFlyout = flyout.cell.activeId == ImGui::GetID("##cellFlyout");
							ImGui::PopID();
						}
						if (!ownsActiveFlyout)
							continue;
					}

					if (cellIndices.empty()) {
						if (source == EntrySource::User && deferredCallbacks.onAddPeriod) {
							ImGui::PushID(p);
							const float btnSz = C::Em(C::SCENE_ADD_PERIOD_BTN_EM);
							const float cellW = ImGui::GetContentRegionAvail().x;
							ImGui::SetCursorPos(ImVec2(
								ImGui::GetCursorPosX() + std::max(0.f, (cellW - btnSz) * 0.5f),
								ImGui::GetCursorPosY() + std::max(0.f, (rowVisualH - btnSz) * 0.5f)));
							if (Icons::Button("##addPeriod", Icons::TI(ICON_TI_PLUS), ImVec2(btnSz, btnSz))) {
								for (const auto sourceIndex : row.addPeriodSourceIndices) {
									if (sourceIndex >= entries.size())
										continue;
									const auto& sourceEntry = entries[sourceIndex];
									deferredCallbacks.onAddPeriod(
										sourceEntry.featureShortName, sourceEntry.settingPath,
										sourceEntry.settingKey, p);
								}
							}
							ImGui::PopID();
						} else {
							ImGui::TextDisabled("--");
						}
						continue;
					}

					const auto entryIndex = cellIndices.front();
					const auto& entry = entries[entryIndex];
					ImGui::PushID(p);
					const bool allPaused = AreAllPaused(cellIndices, entries);
					CenterCursorY(rowVisualH, ImGui::GetFrameHeight());

					if (allPaused)
						ImGui::BeginDisabled();

					if (cellIndices.size() > 1 && deferredCallbacks.drawEditorMulti)
						deferredCallbacks.drawEditorMulti(
							cellIndices, ImGui::GetContentRegionAvail().x, entry.source == EntrySource::Overwrite);
					else
						deferredCallbacks.drawEditor(
							entryIndex, ImGui::GetContentRegionAvail().x, entry.source == EntrySource::Overwrite);

					if (allPaused)
						ImGui::EndDisabled();

					// Cell flyout
					const ImVec2 flyoutSourceMin = ImGui::GetItemRectMin();
					const ImVec2 flyoutSourceMax = ImGui::GetItemRectMax();
					const bool sourcePressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
					const ImGuiID cellId = ImGui::GetID("##cellFlyout");
					if (!suppressFlyouts) {
						Util::FlyoutScope flyoutScope(
							flyout.cell, cellId, sourcePressed, flyoutSourceMin, flyoutSourceMax, flyoutStyle);
						if (flyout.cell.isOpen && flyout.cell.activeId == cellId)
							flyout.cellSourceRow = rowIndex;
						if (flyoutScope) {
							auto result = DrawFlyoutControls(allPaused, cellIndices.size() > 1, isOverwrite, context, entries, cellIndices);

							if (result.toggled) {
								for (const auto index : cellIndices)
									if (entries[index].paused == allPaused)
										deferredCallbacks.togglePause(index);
							}
							if (result.reverted)
								for (const auto index : cellIndices)
									deferredCallbacks.revert(index);
							if (result.deleted) {
								for (const auto index : cellIndices)
									deferredCallbacks.remove(index);
								Util::CloseFlyout(flyout.cell);
							}
						}
					}

					ImGui::PopID();
				}
			} else {
				// Single-column: collapsed view of all entries for this key
				ImGui::TableSetColumnIndex(kFirstValueColumn);
				CenterCursorY(rowVisualH, ImGui::GetFrameHeight());

				if (rowIndices.empty()) {
					ImGui::TextDisabled("--");
					if (inlineActions) {
						ImGui::TableSetColumnIndex(actionsColumn);
						CenterCursorY(rowVisualH, ImGui::GetFrameHeight());
						ImGui::TextDisabled("--");
					}
				} else {
					size_t displayIndex = rowIndices[0];
					bool anyPaused = std::any_of(rowIndices.begin(), rowIndices.end(),
						[&](size_t i) { return i < entries.size() && entries[i].paused; });

					ImGui::PushID(static_cast<int>(displayIndex));

					if (anyPaused)
						ImGui::BeginDisabled();

					if (deferredCallbacks.drawEditorMulti)
						deferredCallbacks.drawEditorMulti(rowIndices, ImGui::GetContentRegionAvail().x, isOverwrite);
					else
						deferredCallbacks.drawEditor(displayIndex, ImGui::GetContentRegionAvail().x, isOverwrite);

					if (anyPaused)
						ImGui::EndDisabled();

					if (inlineActions) {
						ImGui::TableSetColumnIndex(actionsColumn);
						CenterCursorY(rowVisualH, GetSceneActionButtonSize());
						DrawInlineControls(context, rowIndices, isOverwrite, entries, deferredCallbacks);
					}

					ImGui::PopID();
				}
			}

			if (hasAuxiliaryColumn && ImGui::TableSetColumnIndex(auxiliaryColumn)) {
				CenterCursorY(rowVisualH, ImGui::GetFrameHeight());
				auto disabled = Util::DisableGuard(!isOverwrite && AreAllPaused(rowIndices, entries));
				deferredCallbacks.drawAuxiliary(rowIndices, ImGui::GetContentRegionAvail().x, isOverwrite);
			}

			// Suppress row flyout when a cell flyout is active to prevent accidental whole-row deletion
			if (multiColumn && flyout.cell.isOpen && !flyout.cell.closing && flyout.row.isOpen && !flyout.row.flyoutHovered)
				flyout.row.closing = true;

			ImGui::PopID();
			for (size_t i = 0; i < sid.path.size(); ++i)
				ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
		};

		constexpr auto kNoActiveFlyoutSource = std::numeric_limits<size_t>::max();
		if (!flyout.cell.isOpen)
			flyout.cellSourceRow = kNoActiveFlyoutSource;
		if (!flyout.row.isOpen)
			flyout.rowSourceRow = kNoActiveFlyoutSource;
		const size_t activeCellFlyoutSourceRow =
			flyout.cellSourceRow < group.rows.size() ? flyout.cellSourceRow : kNoActiveFlyoutSource;
		const size_t activeRowFlyoutSourceRow =
			flyout.rowSourceRow < group.rows.size() ? flyout.rowSourceRow : kNoActiveFlyoutSource;

		float measuredSettingRowHeight = -1.0f;
		for (const auto& category : group.categories) {
			if (category.begin >= category.end || category.end > group.rows.size())
				continue;
			const auto& categorySetting = group.rows[category.begin].setting;
			ImGui::TableNextRow();
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
			ImGui::TableSetColumnIndex(kSettingColumn);
			ImGui::SetWindowFontScale(C::SCENE_TOD_FEATURE_TEXT_SCALE);
			const auto visibleCategory = GetVisibleLabel(categorySetting.categoryName);
			ImGui::TextColored(theme.FeatureHeading.ColorDefault, "%.*s:",
				static_cast<int>(visibleCategory.size()), visibleCategory.data());
			ImGui::SetWindowFontScale(1.0f);

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(category.end - category.begin), measuredSettingRowHeight);
			if (activeCellFlyoutSourceRow >= category.begin && activeCellFlyoutSourceRow < category.end)
				clipper.IncludeItemByIndex(static_cast<int>(activeCellFlyoutSourceRow - category.begin));
			if (activeRowFlyoutSourceRow != activeCellFlyoutSourceRow &&
				activeRowFlyoutSourceRow >= category.begin && activeRowFlyoutSourceRow < category.end)
				clipper.IncludeItemByIndex(static_cast<int>(activeRowFlyoutSourceRow - category.begin));
			while (clipper.Step()) {
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
					const size_t rowIndex = category.begin + static_cast<size_t>(row);
					drawRow(group.rows[rowIndex], rowIndex);
				}
			}
			if (measuredSettingRowHeight < 0.0f && clipper.ItemsHeight > 0.0f)
				measuredSettingRowHeight = clipper.ItemsHeight;
		}

		ImGui::EndTable();
		RemoveIndicesReversed(pendingRemoveIndices, cb.remove);
		for (const auto& pending : pendingAddPeriods)
			cb.onAddPeriod(pending.feature, pending.path, pending.key, pending.period);
	}
	bool DrawSectionHeader(const char* label, const char* idSuffix,
		bool allPaused, std::function<void()> onTogglePause, std::function<void()> onDeleteAll,
		const SourceTableLayout& layout)
	{
		ImGui::Spacing();
		const float sectionWidth = std::min(layout.sectionWidth, ImGui::GetContentRegionAvail().x);
		if (!ImGui::BeginChild(std::format("##sec{}", idSuffix).c_str(), ImVec2(sectionWidth, 0),
				ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_HorizontalScrollbar))
			return false;
		auto headerLabel = std::format("{}{}", label, idSuffix);
		bool open = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

		if (open) {
			if (ImGui::SmallButton(std::format("{}{}", allPaused ? T("feature.scene_manager.action.unpause_all", "Unpause All") : T("feature.scene_manager.action.pause_all", "Pause All"), idSuffix).c_str()))
				onTogglePause();
			ImGui::SameLine();
			if (ImGui::SmallButton(std::format("{}{}",
					T("feature.scene_manager.action.delete_all", "Delete All"), idSuffix)
						.c_str()))
				onDeleteAll();
		}
		return open;
	}

	struct AddNode
	{
		std::map<std::string, AddNode> children;
		std::vector<size_t> settings;
	};

	struct AddControl
	{
		std::string label;
		std::vector<const Metadata*> members;
		bool selected = false;
		int member = -1;
	};

	struct ViewState
	{
		SourcePanelCache cache;
		std::array<TableFlyoutState, 2> flyouts;
		bool addRequested = false;
		bool addOpen = false;
		bool addAllPeriods = false;
		std::optional<Period> addPeriod;
		std::string feature;
		std::vector<int> selectedPath;
		std::vector<AddControl> controls;
		AddNode settingTree;
		std::map<ImGuiID, float> pickerScroll;
		Util::ConfirmationPopup removeSource;
		Source pendingSource = Source::User;
	};
	std::map<Context, ViewState> viewStates;

	void RebuildAddSettings(const Context& context, ViewState& state)
	{
		state.controls.clear();
		state.settingTree = {};
		state.selectedPath.clear();
		const auto rules = SceneSettingsContextRules::GetSceneContextRules(context);
		using ControlKey = std::tuple<std::vector<std::string>, std::string, int>;
		std::map<ControlKey, size_t> groups;
		for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
			if (setting.featureShortName != state.feature)
				continue;
			const auto path = Manager::SplitSettingPath(setting.settingPath);
			const std::string key{ setting.settingKey };
			if (!Manager::IsSettingAllowedForType(rules.sceneType, state.feature, path, key, rules.requireNumeric))
				continue;
			Entry entry{ .featureShortName = state.feature, .settingPath = path, .settingKey = key };
			Manager::SettingControlInfo info;
			const bool grouped = Manager::GetSettingControlInfo(entry, info) &&
			                     info.controlType != SceneSettingControlType::Scalar && info.componentCount > 1 && !info.aggregateAll;
			const ControlKey identity{ grouped ? info.settingPath : path, grouped ? info.settingKey : key, grouped ? info.componentStart : -1 };
			const auto [found, inserted] = groups.try_emplace(identity, state.controls.size());
			if (inserted) {
				state.controls.push_back({ .label = grouped ? info.displayName : Util::Settings::GetCatalogLeafDisplayName(setting),
					.member = grouped && info.controlType != SceneSettingControlType::Color ? 0 : -1 });
				auto* node = &state.settingTree;
				for (const auto& part : Util::Settings::GetCatalogSelectorPath(setting))
					node = &node->children[part];
				node->settings.push_back(found->second);
			}
			state.controls[found->second].members.push_back(&setting);
		}
		for (auto& control : state.controls)
			std::ranges::sort(control.members, {}, [](const Metadata* metadata) { return metadata->serializedComponent; });
	}

	const AddNode* DrawAddSubfeatures(ViewState& state)
	{
		const auto* node = &state.settingTree;
		for (size_t level = 0; !node->children.empty(); ++level) {
			int selected = level < state.selectedPath.size() ? state.selectedPath[level] : -1;
			const bool selectedChild = selected >= 0 && selected < static_cast<int>(node->children.size());
			auto child = node->children.begin();
			if (selectedChild)
				std::advance(child, selected);
			const auto label = selectedChild ? child->first : std::string(T("feature.scene_manager.select_subfeature", "Select Sub Feature..."));
			ImGui::Spacing();
			const float backWidth = selectedChild ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
			ImGui::SetNextItemWidth(std::max(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().x - backWidth));
			ImGui::PushID(static_cast<int>(level));
			if (Util::BeginSearchableCombo("##SubFeatureSelect", label.c_str(), ImGuiComboFlags_None, nullptr, 0,
					&state.pickerScroll[ImGui::GetID("##SubFeatureSelect")])) {
				int index = 0;
				for (const auto& [name, ignored] : node->children) {
					if (Util::SearchableComboMatches(name) && Util::RoundedSelectable(name.c_str(), index == selected)) {
						state.selectedPath.resize(level + 1, -1);
						state.selectedPath[level] = index;
						selected = index;
					}
					++index;
				}
				Util::EndSearchableCombo();
			}
			bool goBack = false;
			if (selectedChild) {
				ImGui::SameLine();
				goBack = Icons::Button("##SubFeatureBack", Icons::TI(ICON_TI_ARROW_BACK_UP), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
				Util::AddTooltip(T("feature.scene_manager.action.show_parent_settings", "Show parent settings"));
			}
			ImGui::PopID();
			if (goBack) {
				state.selectedPath.resize(level);
				return node;
			}
			if (selected < 0 || selected >= static_cast<int>(node->children.size()))
				return node;
			child = node->children.begin();
			std::advance(child, selected);
			node = &child->second;
		}
		return node;
	}

	template <class Visit>
	void VisitAddMembers(const AddControl& control, Visit&& visit)
	{
		if (control.member >= 0 && control.member < static_cast<int>(control.members.size())) {
			visit(*control.members[control.member]);
			return;
		}
		for (const auto* member : control.members)
			visit(*member);
	}

	template <class Visit>
	void VisitAddPeriods(Context context, const ViewState& state, Visit&& visit)
	{
		if (state.addAllPeriods) {
			for (const auto period : Manager::kPeriods) {
				context.period = period;
				visit(context);
			}
		} else {
			if (state.addPeriod)
				context.period = *state.addPeriod;
			visit(context);
		}
	}

	bool IsAddSelectionUnavailable(const Context& context, const ViewState& state, const AddControl& control, bool* added = nullptr)
	{
		bool allAdded = true;
		bool retained = false;
		VisitAddMembers(control, [&](const Metadata& member) {
			const auto path = Manager::SplitSettingPath(member.settingPath);
			const std::string key{ member.settingKey };
			VisitAddPeriods(context, state, [&](const Context& target) {
				allAdded &= Manager::GetSingleton()->FindContextUserEntry(target, state.feature, path, key).has_value();
				retained |= Manager::GetSingleton()->IsContextEditTarget(target, state.feature);
			});
		});
		if (added)
			*added = allAdded;
		return allAdded || retained;
	}

	void DrawAddDialog(const Context& context, ViewState& state)
	{
		if (std::exchange(state.addRequested, false)) {
			for (auto& [key, view] : viewStates)
				view.addOpen = false;
			state.addOpen = true;
			state.feature.clear();
			state.controls.clear();
			state.settingTree = {};
			state.selectedPath.clear();
			state.pickerScroll.clear();
			++s_transientGeneration;
			ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
		}
		if (!state.addOpen)
			return;
		s_addDialogFrame = ImGui::GetFrameCount();
		const float width = std::min(C::Em(C::SCENE_ADD_DIALOG_WIDTH_EM), ImGui::GetMainViewport()->WorkSize.x - ImGui::GetStyle().WindowPadding.x * 2.0f);
		ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, ImGui::GetMainViewport()->WorkSize.y));
		const auto title = std::format("{}##{:x}", T("feature.scene_manager.add_dialog.title", "Add Feature Settings"),
			reinterpret_cast<uintptr_t>(&state));
		if (!Util::BeginWithRoundedClose(title.c_str(), &state.addOpen,
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}
		if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
			ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
		const auto preview = state.feature.empty() ? std::string(T("feature.scene_manager.select_feature", "Select Feature...")) :
		                                             Manager::GetFeatureDisplayName(state.feature);
		const auto rules = SceneSettingsContextRules::GetSceneContextRules(context);
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (Util::BeginSearchableCombo("##FeatureSelect", preview.c_str(), ImGuiComboFlags_None, nullptr, 0,
				&state.pickerScroll[ImGui::GetID("##FeatureSelect")])) {
			for (const auto& name : Manager::GetLocationRelevantFeatureNames()) {
				if (!Manager::IsFeatureAllowedForType(rules.sceneType, name))
					continue;
				const auto label = Manager::GetFeatureDisplayName(name);
				if (!Util::SearchableComboMatches(label))
					continue;
				if (Util::RoundedSelectable(label.c_str(), state.feature == name)) {
					state.feature = name;
					RebuildAddSettings(context, state);
				}
			}
			Util::EndSearchableCombo();
		}
		const auto* node = DrawAddSubfeatures(state);
		if (!state.feature.empty() && !node->settings.empty()) {
			ImGui::Spacing();
			ImGui::Separator();
			if (ImGui::SmallButton(T("feature.scene_manager.action.select_all", "Select All")))
				for (const auto index : node->settings)
					state.controls[index].selected = !IsAddSelectionUnavailable(context, state, state.controls[index]);
			ImGui::SameLine();
			if (ImGui::SmallButton(T("feature.scene_manager.action.select_none", "Select None")))
				for (const auto index : node->settings)
					state.controls[index].selected = false;
			ImGui::Spacing();
			if (ImGui::BeginChild("##SettingList", ImVec2(-FLT_MIN, C::Em(C::SCENE_ADD_LIST_HEIGHT_EM)), ImGuiChildFlags_Borders)) {
				for (const auto index : node->settings) {
					auto& control = state.controls[index];
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::BeginTable("##AddControl", control.members.size() > 1 ? 2 : 1,
							ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
						ImGui::TableSetupColumn("##setting", ImGuiTableColumnFlags_WidthStretch);
						if (control.members.size() > 1)
							ImGui::TableSetupColumn("##member", ImGuiTableColumnFlags_WidthFixed, C::Em(kChannelSelectorWidthEm));
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						bool alreadyAdded = false;
						const bool unavailable = IsAddSelectionUnavailable(context, state, control, &alreadyAdded);
						if (unavailable)
							control.selected = false;
						bool checked = alreadyAdded || control.selected;
						{
							const Util::DisableGuard disabled(unavailable);
							if (Util::CheckboxWithClippedText(control.label.c_str(), &checked))
								control.selected = checked;
						}
						if (control.members.size() > 1) {
							ImGui::TableNextColumn();
							const auto memberName = [&](int member) {
								return member < 0 ? std::string(T("feature.scene_manager.channel.all", "All")) :
								                    SceneSettingsInternal::GetCatalogComponentDisplayName(*control.members[member],
														SceneSettingsInternal::GetCatalogControlType(*control.members[member]));
							};
							ImGui::SetNextItemWidth(-FLT_MIN);
							if (Util::BeginSearchableCombo("##Channel", memberName(control.member).c_str())) {
								for (int member = -1; member < static_cast<int>(control.members.size()); ++member) {
									const auto label = memberName(member);
									if (Util::SearchableComboMatches(label) && Util::RoundedSelectable(label.c_str(), control.member == member))
										control.member = member;
								}
								Util::EndSearchableCombo();
							}
						}
						ImGui::EndTable();
					}
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
			ImGui::Spacing();
			int selected = 0;
			for (auto& control : state.controls) {
				if (IsAddSelectionUnavailable(context, state, control))
					control.selected = false;
				selected += control.selected ? 1 : 0;
			}
			const auto addLabel = std::vformat(T("feature.scene_manager.action.add_count", "Add ({0})"), std::make_format_args(selected));
			const Util::DisableGuard disabled(selected == 0);
			if (Util::Button(addLabel.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
				for (const auto& control : state.controls)
					if (control.selected)
						VisitAddMembers(control, [&](const Metadata& member) {
							VisitAddPeriods(context, state, [&](const Context& target) {
								Manager::GetSingleton()->AddContextSetting(target, state.feature,
									Manager::SplitSettingPath(member.settingPath), std::string(member.settingKey), true);
							});
						});
				Manager::GetSingleton()->SaveAllUserSettings();
				state.addOpen = false;
			}
		}
		ImGui::End();
	}

	void DrawEmptySettings(const Context& context)
	{
		using Type = Manager::SceneContextType;
		ImGui::Spacing();
		switch (context.type) {
		case Type::Interior:
			Util::Text::Disabled("%s", T("feature.scene_manager.interior.empty", "No interior settings configured."));
			Util::Text::Disabled("%s", T("feature.scene_manager.empty_add_hint", "Use the Add Setting button above to add overrides."));
			ImGui::Spacing();
			ImGui::TextWrapped("%s", T("feature.scene_manager.interior.description",
										 "Settings added here override feature values in interior cells and revert automatically outside."));
			break;
		case Type::TimeOfDay:
			Util::Text::Disabled("%s", T("feature.scene_manager.time_of_day.empty", "No time-of-day settings configured."));
			Util::Text::Disabled("%s", T("feature.scene_manager.time_of_day.empty_hint", "Use the Add buttons above to add overrides for each period."));
			break;
		case Type::Location:
			Util::Text::Disabled("%s", T("feature.scene_manager.location.empty", "No settings are configured for this target."));
			break;
		case Type::Weather:
			Util::Text::Disabled("%s", T("feature.scene_manager.weather.empty", "No weather settings configured."));
			break;
		}
	}
}

Util::FlyoutStyle SceneEntryTable::GetActionFlyoutStyle()
{
	const float scale = Util::GetUIScale();
	const float paddingY = ImGui::GetStyle().WindowBorderSize + kSceneFlyoutPaddingY * scale;
	return {
		{ std::max(kSceneFlyoutPaddingX * scale, paddingY), paddingY },
		kSceneFlyoutRounding * scale,
		kSceneFlyoutBackgroundAlpha,
		1.0f,
		0.0f,
		true,
		true
	};
}

float SceneEntryTable::GetActionButtonSize()
{
	return GetSceneActionButtonSize();
}

ImVec2 SceneEntryTable::GetActionToggleSize()
{
	return GetCompactFeatureToggleSize();
}

void SceneEntryTable::Draw(const Context& context)
{
	auto* manager = Manager::GetSingleton();
	auto key = context;
	key.period = Period::Count;
	auto& state = viewStates[key];
	ImGui::PushID(&state);
	const SKSE::stl::scope_exit restoreId([] { ImGui::PopID(); });
	if (manager->HasPendingContextEdit())
		Util::Text::WrappedWarning("%s", T("feature.scene_manager.edit.finish_pending",
											 "Save or discard unsaved scene settings before copying them, changing presets, or reloading."));
	const bool periodic = manager->IsSceneTimeOfDayEnabled(context);
	if (periodic) {
		for (const auto period : Manager::kPeriods) {
			const auto label = GetAddPeriodLabel(period);
			if (period != Manager::kPeriods.front())
				BrowserUI::SameLineIfFits(ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f);
			ImGui::PushID(static_cast<int>(period));
			if (ImGui::SmallButton(label.c_str())) {
				state.addPeriod = period;
				state.addAllPeriods = false;
				state.addRequested = true;
			}
			ImGui::PopID();
		}
		const auto* addAll = T("feature.scene_manager.action.add_all", "Add All");
		BrowserUI::SameLineIfFits(ImGui::CalcTextSize(addAll).x + ImGui::GetStyle().FramePadding.x * 2.0f);
		if (ImGui::SmallButton(addAll)) {
			state.addPeriod.reset();
			state.addAllPeriods = true;
			state.addRequested = true;
		}
	} else if (ImGui::SmallButton(T("feature.scene_manager.action.add_setting", "Add Setting"))) {
		state.addPeriod.reset();
		state.addAllPeriods = false;
		state.addRequested = true;
	}
	DrawAddDialog(context, state);
	if (context.type == Manager::SceneContextType::TimeOfDay)
		ImGui::Separator();
	const auto stored = manager->GetContextEntries(context);
	const std::vector<Entry> entries(stored.begin(), stored.end());
	const bool transitionColumn = context.type == Manager::SceneContextType::Location &&
	                              std::ranges::any_of(entries, [&](const Entry& entry) { return IsTransitionEntry(entry) && (entry.period != Period::Count) == periodic; });
	const char* transitionLabel = transitionColumn ? T("feature.scene_manager.location.transition.column", "Transition") : nullptr;
	const float transitionWidth = transitionLabel ?
	                                  std::max(SceneTransitionField::GetWidth(), ImGui::CalcTextSize(transitionLabel).x + ImGui::GetStyle().ItemInnerSpacing.x) :
	                                  0.0f;
	RefreshSourcePanelCache(state.cache, entries, periodic ? kPeriodCount : 1, !periodic, false,
		transitionWidth, periodic);
	if (state.cache.overwrite.group.rows.empty() && state.cache.user.group.rows.empty()) {
		DrawEmptySettings(context);
		return;
	}
	std::vector<size_t> removals;
	const TableCallbacks callbacks{
		[&](size_t index, float, bool) { DrawEntryValue(EntryContext(context, entries[index]), entries, { index }, entries[index].source, !periodic); },
		[&](const std::vector<size_t>& indices, float, bool) { DrawEntryValue(EntryContext(context, entries[indices.front()]), entries, indices, entries[indices.front()].source, !periodic); },
		[&](size_t index) { manager->TogglePauseContextEntry(EntryContext(context, entries[index]), index); },
		[&](size_t index) {
			if (entries[index].deleted)
				removals.push_back(index);
			else
				manager->RevertContextEntryToDefault(EntryContext(context, entries[index]), index);
		},
		[&](size_t index) { removals.push_back(index); },
		[&](const std::string& feature, const std::vector<std::string>& path, const std::string& setting, int period) {
			auto target = context;
			target.period = static_cast<Period>(period);
			manager->AddContextSetting(target, feature, path, setting);
		},
		transitionLabel,
		transitionColumn ? std::function<void(const std::vector<size_t>&, float, bool)>([&](const std::vector<size_t>& indices, float, bool readOnly) {
			DrawLocationTransition(context, entries, indices, readOnly ? Source::Overwrite : Source::User);
		}) :
						   std::function<void(const std::vector<size_t>&, float, bool)>{}
	};
	for (const auto source : { Source::Overwrite, Source::User }) {
		auto& cached = source == Source::Overwrite ? state.cache.overwrite : state.cache.user;
		if (cached.group.rows.empty())
			continue;
		const char* suffix = source == Source::Overwrite ? "##Overwrite" : "##User";
		const char* label = source == Source::Overwrite ? T("feature.scene_manager.section.overwrite_files", "Overwrite Files") :
		                                                  T("feature.scene_manager.section.user_settings", "User Settings");
		if (DrawSectionHeader(label, suffix, cached.allPaused, [&] {
				if (!OwnsRetainedEdit(context, entries, cached.indices))
					for (const auto index : cached.indices)
						if (entries[index].paused == cached.allPaused)
							callbacks.togglePause(index); }, [&] {
				state.pendingSource = source;
				state.removeSource.title = T("feature.scene_manager.action.delete_all", "Delete All");
				state.removeSource.message = source == Source::Overwrite ?
					T("feature.scene_manager.entry_suppress_tooltip", "Remove this context's value and use the settings beneath it.") :
					T("feature.scene_manager.confirm.delete_all_user", "Are you sure you want to remove all user-added settings?");
				state.removeSource.confirmLabel = T("feature.scene_manager.action.delete_all", "Delete All");
				state.removeSource.cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");
				state.removeSource.Request(); }, cached.layout))
			DrawSourceTable(context, cached.group, entries, suffix, source, cached.layout, state.flyouts[static_cast<size_t>(source)], callbacks);
		ImGui::EndChild();
	}
	if (state.removeSource.Draw()) {
		const auto& cached = state.pendingSource == Source::Overwrite ? state.cache.overwrite : state.cache.user;
		if (!OwnsRetainedEdit(context, entries, cached.indices))
			removals.insert(removals.end(), cached.indices.begin(), cached.indices.end());
	}
	RemoveIndicesReversed(removals, [&](size_t index) {
		const auto& entry = entries[index];
		const auto target = EntryContext(context, entry);
		if (entry.source == Source::Overwrite)
			manager->TombstoneContextSetting(target, entry.featureShortName, entry.settingPath, entry.settingKey);
		else if (entry.deleted)
			manager->ClearContextTombstone(target, entry.featureShortName, entry.settingPath, entry.settingKey);
		else if (const auto current = manager->FindContextUserEntry(target, entry.featureShortName, entry.settingPath, entry.settingKey))
			manager->RemoveContextSetting(target, *current);
	});
}

bool SceneEntryTable::HasActiveDialog()
{
	return std::ranges::any_of(viewStates, [](const auto& pair) {
		return pair.second.addOpen || pair.second.removeSource.IsOpen();
	});
}
