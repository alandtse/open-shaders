#include "SceneWidgetBinding.h"
#include "Menu/SettingReveal.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <tuple>
#include <utility>

#include <imgui_internal.h>

#include "../../I18n/I18n.h"
#include "../EditorWindow.h"
#include "BaselineManager.h"
#include "Feature.h"
#include "Globals.h"
#include "Menu.h"
#include "Menu/FeatureListRenderer.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/Icons/helpers/SceneActionIcons.h"
#include "SceneEntryTable.h"
#include "SceneSettingsContextRules.h"
#include "SceneSettingsInternal.h"
#include "SceneSettingsUI.h"
#include "SceneWidgetInterceptor.h"
#include "State.h"
#include "Utils/Game.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/SettingsControl.h"
#include "Utils/SettingsPatch.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using Kind = SceneWidgetBinding::Value::Kind;
	using BaselineLayer = SceneWidgetBinding::BaselineLayer;
	using SettingMetadata = SceneSettingsCatalog::SettingMetadata;

	bool baselineScenePresentationHidden = false;

	bool IsSceneBaselineLayer(BaselineLayer layer)
	{
		return layer == BaselineLayer::Scene || layer == BaselineLayer::Sketch;
	}

	constexpr int kPeriodCount = SceneSettingsManager::kPeriodCount;
	using TimeOfDayPeriod = SceneSettingsManager::TimeOfDayPeriod;
	using SceneSettingsContextRules::EntryBelongsToContext;

	/// Reads and writes one ImGui scalar through a double. A zero size means the type is not one
	/// the scalar widgets accept, which the binding treats as an unresolvable control.
	struct ScalarTraits
	{
		std::size_t size = 0;
		double (*read)(const void*) = nullptr;
		void (*write)(void*, double) = nullptr;
	};

	template <typename T>
	constexpr ScalarTraits MakeScalarTraits()
	{
		return { sizeof(T),
			[](const void* source) { return static_cast<double>(*static_cast<const T*>(source)); },
			[](void* destination, double number) { *static_cast<T*>(destination) = static_cast<T>(number); } };
	}

	ScalarTraits GetScalarTraits(ImGuiDataType type)
	{
		switch (type) {
		case ImGuiDataType_S8:
			return MakeScalarTraits<std::int8_t>();
		case ImGuiDataType_U8:
			return MakeScalarTraits<std::uint8_t>();
		case ImGuiDataType_S16:
			return MakeScalarTraits<std::int16_t>();
		case ImGuiDataType_U16:
			return MakeScalarTraits<std::uint16_t>();
		case ImGuiDataType_S32:
			return MakeScalarTraits<std::int32_t>();
		case ImGuiDataType_U32:
			return MakeScalarTraits<std::uint32_t>();
		case ImGuiDataType_S64:
			return MakeScalarTraits<std::int64_t>();
		case ImGuiDataType_U64:
			return MakeScalarTraits<std::uint64_t>();
		case ImGuiDataType_Float:
			return MakeScalarTraits<float>();
		case ImGuiDataType_Double:
			return MakeScalarTraits<double>();
		default:
			return {};
		}
	}

	/// Bytes the caller's storage occupies, or zero when the control cannot be bound.
	std::size_t ValueSize(const SceneWidgetBinding::Value& value)
	{
		switch (value.kind) {
		case Kind::Bool:
			return sizeof(bool);
		case Kind::Int:
			return sizeof(int);
		case Kind::Float:
		case Kind::FloatVector:
			return value.componentCount <= SceneWidgetBinding::Value::kMaxComponents ?
			           sizeof(float) * value.componentCount :
			           0;
		case Kind::Scalar:
			return GetScalarTraits(value.scalarType).size;
		case Kind::String:
			return sizeof(std::string);
		default:
			return 0;
		}
	}

	void CopyValue(const SceneWidgetBinding::Value& value, void* destination, const void* source, std::size_t size)
	{
		if (value.kind == Kind::String)
			*static_cast<std::string*>(destination) = *static_cast<const std::string*>(source);
		else
			std::memcpy(destination, source, size);
	}

	using ComponentGroup = std::vector<const SettingMetadata*>;

	// Same grouping the manager uses to fold stored scalars back into one aggregate control, plus
	// settingPath so two controls over one serialized member stay apart.
	using AggregateKey = std::tuple<std::string_view, std::string_view, std::string_view,
		std::string_view, SceneSettingsCatalog::AggregateSemantic, std::int8_t, std::uint8_t>;

	AggregateKey MakeAggregateKey(const SettingMetadata& setting)
	{
		return { setting.featureShortName, setting.settingPath, setting.serializedPath,
			setting.serializedKey, setting.aggregateSemantic, setting.aggregateStart,
			setting.aggregateCount };
	}

	/// The catalog rows one control drives: its own row alone, or every sibling of its aggregate.
	ComponentGroup GetControlComponents(const SettingMetadata& setting)
	{
		// A single-component row is its own control even when it sits inside a vector member, so
		// the four sliders over one float4 must not be folded into one aggregate.
		if (setting.aggregateCount <= 1 || setting.aggregateAll)
			return { &setting };

		static const std::map<AggregateKey, ComponentGroup> groups = [] {
			std::map<AggregateKey, ComponentGroup> built;
			for (const auto& candidate : SceneSettingsCatalog::GetSettings()) {
				if (candidate.aggregateCount <= 1 || candidate.aggregateAll)
					continue;
				built[MakeAggregateKey(candidate)].push_back(&candidate);
			}
			for (auto& [key, group] : built)
				std::ranges::sort(group, {}, &SettingMetadata::serializedComponent);
			return built;
		}();

		const auto found = groups.find(MakeAggregateKey(setting));
		return found != groups.end() ? found->second : ComponentGroup{ &setting };
	}

	bool BeginSettingContextMenu()
	{
		constexpr auto popup = "##SettingContextMenu";
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			ImGui::OpenPopup(popup);
		return ImGui::BeginPopup(popup);
	}

	using SceneContextId = SceneSettingsManager::SceneContextId;
	using SceneContextType = SceneSettingsManager::SceneContextType;
	using SettingLayer = SceneSettingsManager::SettingLayer;

	/// Which layer supplies one address, per period. An aperiodic context fills every slot, so a
	/// periodic page above it reads its own period out of the same row.
	using PeriodLayers = std::array<SettingLayer, kPeriodCount>;
	using LayerIndex = std::map<SceneSettingsManager::SettingIdentity, PeriodLayers>;

	/** @brief A stacked weather or location is read through the saved set it has active; the period
	 *  only picks the set, since the layer walk reads every period of a per-period one. */
	SceneContextId WithActiveSet(SceneContextId a_context)
	{
		a_context.period = SceneSettingsManager::GetSingleton()->IsSceneTimeOfDayEnabled(a_context) ?
		                       SceneSettingsManager::kPeriods[0] :
		                       TimeOfDayPeriod::Count;
		return a_context;
	}

	/** @brief Whether an entry sits in the saved set a context reads: flat for Count, else per-period. */
	bool IsInContextSet(const SceneSettingsManager::SettingEntry& a_entry, const SceneContextId& a_context)
	{
		return (a_entry.period == TimeOfDayPeriod::Count) == (a_context.period == TimeOfDayPeriod::Count);
	}

	/// One collected stack per page per frame. Which layers sit under or over a page depends on the
	/// live scene rather than on the control, so every widget on the page shares one build.
	template <auto Collect>
	const std::vector<SceneContextId>& GetFrameContextStack(const SceneContextId& a_page)
	{
		static std::map<SceneContextId, std::vector<SceneContextId>> cache;
		static int cachedFrame = -1;

		if (const auto frame = ImGui::GetFrameCount(); frame != cachedFrame) {
			cachedFrame = frame;
			cache.clear();
		}
		const auto found = cache.find(a_page);
		return found != cache.end() ? found->second : cache.emplace(a_page, Collect(a_page)).first->second;
	}

	/// The layers a page sits on top of, highest first: weather resolves over time of day, and a
	/// location over whichever stack is running. Interior and the exterior stack never resolve at the
	/// same time, so the live cell picks the one a location sits on, exactly as the resolver does.
	std::vector<SceneContextId> CollectLowerContexts(const SceneContextId& a_page)
	{
		std::vector<SceneContextId> lower;
		if (a_page.type == SceneContextType::Location && Util::IsInterior()) {
			lower.push_back({ .type = SceneContextType::Interior });
			return lower;
		}

		switch (a_page.type) {
		case SceneContextType::Location:
			if (const auto* sky = globals::game::sky; sky && sky->currentWeather)
				lower.push_back(WithActiveSet({ .type = SceneContextType::Weather,
					.weatherId = sky->currentWeather->GetFormID() }));
			[[fallthrough]];
		case SceneContextType::Weather:
			lower.push_back({ .type = SceneContextType::TimeOfDay, .period = SceneSettingsManager::kPeriods[0] });
			break;
		default:
			break;
		}
		return lower;
	}

	const std::vector<SceneContextId>& GetLowerContexts(const SceneContextId& a_page)
	{
		return GetFrameContextStack<CollectLowerContexts>(a_page);
	}

	/// Fills one feature's rows in resolver order so the winning layer supplies the highlight.
	void CollectContextLayers(const SceneContextId& a_context, const std::string& a_feature, LayerIndex& a_index)
	{
		using EntrySource = SceneSettingsManager::EntrySource;
		auto* manager = SceneSettingsManager::GetSingleton();

		SceneSettingsInternal::ForEachEntryByPriority(manager->GetContextEntries(a_context), [&](const auto& entry) {
			if (entry.paused || entry.featureShortName != a_feature || !IsInContextSet(entry, a_context))
				return;
			auto layer = entry.source == EntrySource::Overwrite ? SettingLayer::Overwrite : SettingLayer::User;
			if (entry.deleted)
				layer = SettingLayer::Deleted;

			auto& row = a_index[{ entry.featureShortName, entry.settingPath, entry.settingKey }];
			if (const auto period = static_cast<size_t>(entry.period); period < static_cast<size_t>(kPeriodCount))
				row[period] = layer;
			else
				row.fill(layer);
		});
	}

	/// Layers supplying one feature's addresses in one context. Every control on a page asks this of
	/// the same entries, and the walk is the whole context, so it is cached until the entries change.
	const LayerIndex& GetContextLayerIndex(const SceneContextId& a_context, const std::string& a_feature)
	{
		static std::map<std::pair<SceneContextId, std::string>,
			SceneSettingsManager::RevisionCache<LayerIndex>>
			cache;

		auto* manager = SceneSettingsManager::GetSingleton();
		return cache[{ a_context, a_feature }].Get(manager->GetEntryPresentationRevision(), [&] {
			LayerIndex layers;
			CollectContextLayers(a_context, a_feature, layers);
			return layers;
		});
	}

	/// Where one feature's user entries sit in a context, per period. Same walk and same lifetime as
	/// the layer index: an entry's position only moves when the entries themselves do.
	using PeriodEntries = std::array<std::optional<size_t>, kPeriodCount>;
	using UserEntryIndex = std::map<SceneSettingsManager::SettingIdentity, PeriodEntries>;

	const UserEntryIndex& GetContextUserEntryIndex(const SceneContextId& a_context, const std::string& a_feature)
	{
		static std::map<std::pair<SceneContextId, std::string>,
			SceneSettingsManager::RevisionCache<UserEntryIndex>>
			cache;

		auto* manager = SceneSettingsManager::GetSingleton();
		return cache[{ a_context, a_feature }].Get(manager->GetEntryPresentationRevision(), [&] {
			UserEntryIndex index;
			const auto entries = manager->GetContextEntries(a_context);
			for (size_t position = 0; position < entries.size(); ++position) {
				const auto& entry = entries[position];
				if (entry.source != SceneSettingsManager::EntrySource::User ||
					entry.featureShortName != a_feature || !IsInContextSet(entry, a_context))
					continue;
				// A flat entry has no period of its own, so it fills slot 0.
				const auto slot = entry.period == TimeOfDayPeriod::Count ? 0 : static_cast<size_t>(entry.period);
				assert(slot < static_cast<size_t>(kPeriodCount));
				index[{ entry.featureShortName, entry.settingPath, entry.settingKey }][slot] = position;
			}
			return index;
		});
	}

	/// The location targets resolving after a page: the whole running chain, or for a location page
	/// only the narrower links that follow its own target. A page off the live chain has none above it.
	void AppendUpperLocationContexts(const SceneContextId& a_page, std::vector<SceneContextId>& a_upper)
	{
		const auto& targets = SceneSettingsManager::GetSingleton()->GetCurrentLocationTargets();
		auto target = targets.begin();
		if (a_page.type == SceneContextType::Location) {
			target = std::ranges::find_if(targets, [&a_page](const auto& candidate) {
				return candidate.type == a_page.locationType && candidate.formKey == a_page.locationFormKey;
			});
			if (target == targets.end())
				return;
			++target;
		}
		for (; target != targets.end(); ++target)
			a_upper.push_back(WithActiveSet({ .type = SceneContextType::Location,
				.locationType = target->type,
				.locationFormKey = target->formKey }));
	}

	/// The layers a page is resolved under, lowest first: weather resolves over time of day, and the
	/// location chain over whichever stack is running. Mirror image of CollectLowerContexts, so a page
	/// belonging to the stack the live scene is not running has nothing above it.
	std::vector<SceneContextId> CollectUpperContexts(const SceneContextId& a_page)
	{
		std::vector<SceneContextId> upper;
		const bool interior = Util::IsInterior();

		switch (a_page.type) {
		case SceneContextType::Interior:
			if (!interior)
				return upper;
			break;
		case SceneContextType::TimeOfDay:
			if (interior)
				return upper;
			if (const auto* sky = globals::game::sky; sky && sky->currentWeather)
				upper.push_back(WithActiveSet({ .type = SceneContextType::Weather,
					.weatherId = sky->currentWeather->GetFormID() }));
			break;
		case SceneContextType::Weather:
			if (interior)
				return upper;
			break;
		case SceneContextType::Location:
			break;
		default:
			return upper;
		}

		AppendUpperLocationContexts(a_page, upper);
		return upper;
	}

	const std::vector<SceneContextId>& GetUpperContexts(const SceneContextId& a_page)
	{
		return GetFrameContextStack<CollectUpperContexts>(a_page);
	}

	/// One bit per period slot, so the layer walk can be told which periods to read without being
	/// handed the control that reads them.
	using PeriodMask = std::uint8_t;
	static_assert(kPeriodCount <= 8, "PeriodMask holds one bit per period");
	constexpr PeriodMask kAllPeriods = static_cast<PeriodMask>((1u << kPeriodCount) - 1u);

	/// The one period running right now, or every period while the game has no answer yet.
	PeriodMask LivePeriodMask()
	{
		const auto period = static_cast<size_t>(SceneSettingsManager::GetCurrentPeriod());
		return period < static_cast<size_t>(kPeriodCount) ? static_cast<PeriodMask>(1u << period) : kAllPeriods;
	}

	/// Finds the winning layer at an address for the selected period.
	SettingLayer FoldContextLayer(const SceneContextId& a_context, const std::string& a_feature,
		const SceneSettingsManager::SettingIdentity& a_identity, PeriodMask a_periods)
	{
		const auto& index = GetContextLayerIndex(a_context, a_feature);
		const auto row = index.find(a_identity);
		if (row == index.end())
			return SettingLayer::None;

		for (int slot = 0; slot < kPeriodCount; ++slot)
			if (a_periods & static_cast<PeriodMask>(1u << slot))
				return row->second[static_cast<size_t>(slot)];
		return SettingLayer::None;
	}

	/** @brief Finds the entry the resolver applies at an address in the selected period. */
	const SceneSettingsManager::SettingEntry* FindSupplyingEntry(const SceneContextId& a_context,
		const SceneSettingsManager::SettingIdentity& a_setting, PeriodMask a_periods, bool includePausedPresets = false)
	{
		const SceneSettingsManager::SettingEntry* supplier = nullptr;
		for (const auto& entry : SceneSettingsManager::GetSingleton()->GetContextEntries(a_context)) {
			const auto period = static_cast<size_t>(entry.period);
			if ((entry.paused && (!includePausedPresets || entry.source != SceneSettingsManager::EntrySource::Overwrite || entry.deleted)) ||
				!IsInContextSet(entry, a_context) ||
				(period < static_cast<size_t>(kPeriodCount) && !(a_periods & static_cast<PeriodMask>(1u << period))) ||
				entry.featureShortName != a_setting.featureShortName || entry.settingPath != a_setting.settingPath ||
				entry.settingKey != a_setting.settingKey)
				continue;
			if (!supplier || SceneSettingsInternal::GetEntryRank(entry) >= SceneSettingsInternal::GetEntryRank(*supplier))
				supplier = &entry;
		}
		return supplier;
	}

	/** @brief The value the last Baseline pack applying to a setting gives it, or null.
	 *  Later packs merge over earlier ones, so the last to hold the key is the one in effect.
	 *  @param a_packId Receives the pack supplying the value. */
	const json* FindBaselineValue(const SettingMetadata& a_setting, std::string& a_packId)
	{
		auto* baselines = BaselineManager::GetSingleton();
		using Source = std::pair<const json*, std::string>;
		static SceneSettingsManager::RevisionCache<std::map<const SettingMetadata*, Source>> cache;
		cache.Get(baselines->GetRevision(), [] { return std::map<const SettingMetadata*, Source>{}; });
		auto [source, inserted] = cache.value.try_emplace(&a_setting);
		if (inserted) {
			const auto featureFiles = baselines->GetFeatureFiles(std::string{ a_setting.featureShortName });
			for (auto info = featureFiles.rbegin(); info != featureFiles.rend(); ++info) {
				// A file naming keys the feature lacks is never applied.
				if (!(*info)->unknownKeys.empty())
					continue;
				if (const auto* value = SceneSettingsInternal::GetCatalogSerializedValue((*info)->values, a_setting)) {
					source->second = { value, (*info)->packId };
					break;
				}
			}
		}
		a_packId = source->second.second;
		return source->second.first;
	}

	/** @brief The setting's value in one of the settings layers State holds for its feature. */
	template <class Layer>
	const json* FindStateLayerValue(const SettingMetadata& a_setting, Layer a_layer)
	{
		auto* feature = Feature::FindFeatureByShortName(std::string{ a_setting.featureShortName });
		const auto* layer = feature ? (globals::state->*a_layer)(feature->GetName()) : nullptr;
		return layer ? SceneSettingsInternal::GetCatalogSerializedValue(*layer, a_setting) : nullptr;
	}

	/** @brief The setting's value in the layers beneath the user's edits: defaults with Baseline packs applied. */
	const json* FindLayeredBaseValue(const SettingMetadata& a_setting)
	{
		return FindStateLayerValue(a_setting, &State::GetFeatureBase);
	}

}

SceneWidgetBinding::HideBaselineScenePresentationGuard::HideBaselineScenePresentationGuard() :
	previous(std::exchange(baselineScenePresentationHidden, true))
{}

SceneWidgetBinding::HideBaselineScenePresentationGuard::~HideBaselineScenePresentationGuard()
{
	baselineScenePresentationHidden = previous;
}

SceneWidgetBinding::Guard::Guard(const char* a_label, const Value& a_value) :
	label(a_label), value(a_value)
{
	disabledByCaller = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
	assert(value.data && "intercepted widget bound a null address");
	valueSize = ValueSize(value);
	assert(value.kind == Kind::String || valueSize <= sizeof(ValueStorage::bytes));
	if (!value.data || valueSize == 0 || (value.kind != Kind::String && valueSize > sizeof(ValueStorage::bytes)))
		return;

	// Captured before any early return, so entry creation always has the base value to restore.
	CopyValue(value, preCall.Data(value.kind), value.data, valueSize);

	const auto* context = SceneWidgetInterceptor::GetArmedContext();
	if (!context)
		return;

	// A Util:: wrapper may hand ImGui a rescaled temporary, so the member it stands for is what
	// resolves and what gets persisted.
	const auto* proxy = SceneWidgetInterceptor::GetArmedProxy();
	if (proxy)
		widgetScale = proxy->displayScale;

	metadata = SceneSettingsCatalog::FindSettingForControl(
		context->feature, proxy ? proxy->member : value.data);
	if (!metadata && !proxy && (value.kind == Kind::Float || value.kind == Kind::FloatVector)) {
		static const auto transformedSettings = [] {
			std::vector<const SettingMetadata*> settings;
			for (const auto& setting : SceneSettingsCatalog::GetSettings())
				if (setting.numericTransform != SceneSettingsCatalog::NumericTransform::Identity &&
					(setting.aggregateCount <= 1 || setting.serializedComponent == setting.aggregateStart))
					settings.push_back(&setting);
			return settings;
		}();
		const auto featureName = context->feature->GetShortName();
		bool matchedTreeScope = false;
		for (const auto* setting : transformedSettings) {
			if (setting->featureShortName != featureName ||
				std::max<int>(1, setting->aggregateCount) != value.componentCount ||
				Util::Settings::StripImGuiId(label) != Util::Settings::GetCatalogLeafDisplayName(*setting))
				continue;
			const bool hasTreeScope = !setting->treeScope.empty();
			if (hasTreeScope) {
				const auto scope = setting->treeScopeKey.empty() ? setting->treeScope.data() :
				                                                   T(setting->treeScopeKey, setting->treeScope.data());
				const auto& stack = ImGui::GetCurrentWindow()->IDStack;
				bool scopeActive = false;
				for (int index = 1; index < stack.Size; ++index)
					scopeActive |= stack[index] == ImHashStr(scope, 0, stack[index - 1]);
				if (!scopeActive)
					continue;
			} else if (matchedTreeScope) {
				continue;
			}
			if (hasTreeScope && !matchedTreeScope)
				metadata = nullptr;
			if (metadata) {
				metadata = nullptr;
				break;
			}
			metadata = setting;
			matchedTreeScope = hasTreeScope;
		}
		transformedTemporary = metadata != nullptr;
	}
	if (!metadata) {
		state = State::Unsupported;
		if (!context->baseline && !(value.kind == Kind::Bool &&
									  SceneWidgetInterceptor::IsUiOnlyControl(label, "Checkbox"))) {
			replicaReadOnly = true;
			boundToHolding = true;
			CopyValue(value, holding.Data(value.kind), preCall.Data(value.kind), valueSize);
			OpenDisabled();
		}
		return;
	}
	if (context->baseline) {
		BindBaseline();
		return;
	}
	if (!SceneSettingsManager::IsSceneSettingAllowed(
			metadata->featureShortName, metadata->settingPath, metadata->settingKey)) {
		// Barred by policy, so no scene will ever hold it. Greyed rather than left live, because an
		// edit here would rewrite the feature's base value from a panel that only promises overrides.
		state = State::Unbound;
		identity.featureShortName = std::string{ metadata->featureShortName };
		metadata = nullptr;
		OpenDisabled();
		return;
	}

	contextId = context->contextId;
	identity.featureShortName = std::string{ metadata->featureShortName };
	identity.settingPath = SceneSettingsManager::SplitSettingPath(metadata->settingPath);

	if (contextId.period != TimeOfDayPeriod::Count)
		armedSlot = static_cast<int>(contextId.period);

	ResolveComponents();
	if (components.empty()) {
		// A scene setting this context cannot hold, e.g. a non-transitionable one under time of day.
		state = State::Unavailable;
		OpenDisabled();
		return;
	}

	ResolveState();
	BindDisplayValue();
	CopyValue(value, displayedBeforeEdit.Data(value.kind), holding.Data(value.kind), valueSize);

	if (mixedAcrossPeriods && value.kind == Kind::Bool) {
		// Only Checkbox honours the flag.
		ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
		mixedFlagPushed = true;
	}

	// The control tint shows which layer supplies its value.
	if (const auto tint = ResolveProvenanceColor()) {
		Util::PushTintedFrameStyle(*tint);
		tintPushed = true;
	}
}

SceneWidgetBinding::Guard::~Guard()
{
	// Finish always runs on the happy path; this only closes scopes an exception skipped.
	if (actionOutlinePushed) {
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}
	if (tintPushed)
		Util::PopTintedFrameStyle();
	if (mixedFlagPushed)
		ImGui::PopItemFlag();
	if (disabledOpened)
		ImGui::EndDisabled();
}

void* SceneWidgetBinding::Guard::Raw()
{
	return const_cast<void*>(BoundData());
}

const void* SceneWidgetBinding::Guard::BoundData() const
{
	return boundToHolding ? static_cast<const void*>(holding.Data(value.kind)) : value.data;
}

bool* SceneWidgetBinding::Guard::Bool()
{
	return static_cast<bool*>(Raw());
}

int* SceneWidgetBinding::Guard::Int()
{
	return static_cast<int*>(Raw());
}

float* SceneWidgetBinding::Guard::Float()
{
	return static_cast<float*>(Raw());
}

void SceneWidgetBinding::Guard::ResolveComponents()
{
	// The resolver answers a control's base address with its first component, so an aggregate has
	// to walk out to its siblings: each one is a separate entry keyed by its own settingKey.
	const auto start = std::max<int>(metadata->aggregateStart, 0);
	// The rules AddContextSetting judges by, so a control never shows as allowed and then fails to
	// gain an entry on editing. The main menu edits the base, which has no scene rules.
	const auto rules = state == State::Baseline ?
	                       std::nullopt :
	                       std::optional{ SceneSettingsContextRules::GetSceneContextRules(contextId) };

	for (const auto* setting : GetControlComponents(*metadata)) {
		// Siblings share featureShortName/settingPath (see MakeAggregateKey), so the guard's own
		// resolved values apply to every component here; only settingKey varies.
		if (rules && !SceneSettingsManager::IsSettingAllowedForType(rules->sceneType, identity.featureShortName,
						 identity.settingPath, std::string{ setting->settingKey }, rules->requireNumeric))
			continue;

		const auto slot = setting->aggregateCount <= 1 ?
		                      0 :
		                      static_cast<int>(setting->serializedComponent) - start;
		if (slot < 0 || slot >= value.componentCount)
			continue;

		components.push_back(
			Component{ setting, std::string{ setting->settingKey }, static_cast<std::uint8_t>(slot), {} });
	}

	// The layer queries speak for the whole aggregate through its first component's address.
	if (!components.empty())
		identity.settingKey = components.front().settingKey;
}

SceneSettingsManager::SettingIdentity SceneWidgetBinding::Guard::ComponentIdentity(const Component& a_component) const
{
	return { identity.featureShortName, identity.settingPath, a_component.settingKey };
}

void SceneWidgetBinding::Guard::BindBaseline()
{
	state = State::Baseline;
	identity.featureShortName = std::string{ metadata->featureShortName };
	identity.settingPath = SceneSettingsManager::SplitSettingPath(metadata->settingPath);
	ResolveComponents();
	if (components.empty()) {
		state = State::Unsupported;
		return;
	}

	baselineLayer = ResolveBaselineLayer(baselinePackId);
	if (IsSceneBaselineLayer(baselineLayer)) {
		replicaReadOnly = true;
		boundToHolding = true;
		CopyValue(value, holding.Data(value.kind), preCall.Data(value.kind), valueSize);
		OpenDisabled();
	}

	if (const auto tint = ResolveBaselineColor()) {
		Util::PushTintedFrameStyle(*tint);
		tintPushed = true;
	}
	if (std::ranges::any_of(components, [](const auto& component) { return SettingsControl::IsHighlighted(*component.setting); })) {
		ImGui::PushStyleColor(ImGuiCol_Border, Util::Colors::GetInfo());
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(ImGui::GetStyle().FrameBorderSize, Util::GetUIScale()));
		actionOutlinePushed = true;
	}
}

SceneWidgetBinding::BaselineLayer SceneWidgetBinding::Guard::ResolveBaselineLayer(std::string& a_packId) const
{
	a_packId.clear();
	auto* manager = SceneSettingsManager::GetSingleton();
	const auto anyComponent = [&](auto&& a_predicate) { return std::ranges::any_of(components, a_predicate); };

	if (anyComponent([&](const Component& component) { return manager->IsSketched(ComponentIdentity(component)); }))
		return BaselineLayer::Sketch;
	if (anyComponent([&](const Component& component) {
			return manager->IsActiveSceneSetting(identity.featureShortName, identity.settingPath, component.settingKey);
		}))
		return BaselineLayer::Scene;

	bool fromPack = false;
	for (const auto& component : components) {
		std::string packId;
		const auto* packValue = FindBaselineValue(*component.setting, packId);
		const auto* beneath = packValue ? packValue : FindLayeredBaseValue(*component.setting);
		if (packValue && a_packId.empty())
			a_packId = std::move(packId);
		if (!beneath)
			continue;
		if (!SceneSettingsManager::AppliedValuesEqual(*beneath, ReadEditedValue(component)))
			return BaselineLayer::Edited;
		fromPack |= packValue != nullptr;
	}
	return fromPack ? BaselineLayer::Pack : BaselineLayer::Base;
}

std::optional<ImVec4> SceneWidgetBinding::Guard::ResolveBaselineColor() const
{
	if (baselineScenePresentationHidden && IsSceneBaselineLayer(baselineLayer))
		return std::nullopt;
	switch (baselineLayer) {
	case BaselineLayer::Pack:
		return Util::Colors::GetWarning();
	case BaselineLayer::Edited:
	case BaselineLayer::Sketch:
		return Util::Colors::GetSuccess();
	case BaselineLayer::Scene:
		return Util::Colors::GetInfo();
	default:
		return std::nullopt;
	}
}

std::string SceneWidgetBinding::Guard::ResolveBaselineTooltip() const
{
	if (baselineScenePresentationHidden && IsSceneBaselineLayer(baselineLayer))
		return {};
	switch (baselineLayer) {
	case BaselineLayer::Pack:
		return std::vformat(T(TKEY("baseline_from_named_pack"), "Orange: Overridden by a preset ({})."),
			std::make_format_args(baselinePackId));
	case BaselineLayer::Edited:
	case BaselineLayer::Sketch:
		return T(TKEY("baseline_edited"), "Green: Your edited value.");
	case BaselineLayer::Scene:
		return T(TKEY("baseline_from_scene"),
			"Blue: Scene Manager active setting.\nToggle the switch above on the sidebar to make changes to this setting.");
	default:
		return {};
	}
}

void SceneWidgetBinding::Guard::ReportSettingHint(std::string message) const
{
	if (!ImGui::IsItemHovered(Util::kTooltipWhenDisabled))
		return;
	FeatureListRenderer::SetSettingHint(std::move(message));
}

void SceneWidgetBinding::Guard::ResolveState()
{
	auto* manager = SceneSettingsManager::GetSingleton();
	const auto entries = manager->GetContextEntries(contextId);

	entryIndex.reset();
	mixedAcrossPeriods = false;
	lowerLayer = SceneSettingsManager::SettingLayer::None;
	upperLayer = SceneSettingsManager::SettingLayer::None;

	bool anyActive = false;
	bool anyPaused = false;
	bool anyMissing = false;
	bool anyDeleted = false;
	bool anyLiveValue = false;  // a covered slot holding a real value: unpaused and not a tombstone

	// Every component shares the guard's feature and path, so one key swap per component walks the
	// index the whole page already built.
	const auto& userEntries = GetContextUserEntryIndex(contextId, identity.featureShortName);
	auto lookup = identity;

	for (auto& component : components) {
		lookup.settingKey = component.settingKey;
		const auto found = userEntries.find(lookup);
		component.periodEntries = found != userEntries.end() ? found->second : PeriodEntries{};

		const json* reference = nullptr;
		for (int slot = 0; slot < kPeriodCount; ++slot) {
			auto& resolved = component.periodEntries[static_cast<size_t>(slot)];
			if (!IsCoveredSlot(slot)) {
				resolved.reset();
				continue;
			}
			if (!resolved || *resolved >= entries.size()) {
				resolved.reset();
				anyMissing = true;
				continue;
			}

			const auto& entry = entries[*resolved];
			// A paused tombstone suppresses nothing, so the state must not claim the value is gone.
			if (entry.deleted && !entry.paused)
				anyDeleted = true;
			if (entry.paused)
				anyPaused = true;
			else
				anyActive = true;
			if (!entry.paused && !entry.deleted)
				anyLiveValue = true;
			// A tombstone's value is stale and meaningless; never compare against it.
			if (!entry.deleted) {
				if (!reference)
					reference = &entry.value;
				else if (*reference != entry.value)
					mixedAcrossPeriods = true;
			}
		}
	}

	// Nothing left to resolve, and the provenance query below needs a component to key off of.
	if (components.empty())
		return;

	// Provenance is a separate axis from state: it says who wins, state says what the user's own
	// entry is doing.
	winningLayer = ResolveWinningLayer();

	if (!anyActive && !anyPaused) {
		state = winningLayer == SceneSettingsManager::SettingLayer::Overwrite ? State::Overwritten : State::Absent;
		// Nothing in this context holds the address, so a layer under this page may be driving it and
		// the page has to say so.
		if (winningLayer == SceneSettingsManager::SettingLayer::None)
			lowerLayer = ResolveLowerLayer();
	} else {
		// A partly covered or partly paused control is as mixed as one whose periods hold two values.
		state = anyDeleted ? State::Deleted : (anyActive ? State::Active : State::Paused);
		mixedAcrossPeriods = mixedAcrossPeriods || anyMissing || (anyActive && anyPaused) || (anyDeleted && anyLiveValue);

		for (const auto& component : components) {
			entryIndex = PrimaryEntry(component);
			if (entryIndex)
				break;
		}
	}

	// A control showing a value can lose it, and an empty one still has to say when the number in its
	// box belongs to a layer above. A tombstone or a paused entry applies either way, so neither asks.
	if (SuppliesValue(winningLayer) || state == State::Absent)
		upperLayer = ResolveUpperLayer();
	for (auto& component : components)
		component.preset = FindPresetTarget(ComponentIdentity(component));
}

SceneSettingsManager::SettingLayer SceneWidgetBinding::Guard::ResolveWinningLayer() const
{
	return FoldContextLayer(contextId, identity.featureShortName, identity, CoveredPeriodMask());
}

std::uint8_t SceneWidgetBinding::Guard::CoveredPeriodMask() const
{
	// A flat page spans the whole day, but only the running period of a periodic layer reaches the
	// scene, so folding all six would let an off-hours entry answer for right now.
	if (contextId.period == TimeOfDayPeriod::Count)
		return LivePeriodMask();
	return static_cast<PeriodMask>(1u << armedSlot);
}

SceneSettingsManager::SettingLayer SceneWidgetBinding::Guard::ResolveLowerLayer() const
{
	const auto periods = CoveredPeriodMask();

	for (const auto& lower : GetLowerContexts(contextId)) {
		switch (FoldContextLayer(lower, identity.featureShortName, identity, periods)) {
		case SettingLayer::User:
			return SettingLayer::User;
		case SettingLayer::Overwrite:
			return SettingLayer::Overwrite;
		// A tombstone only takes the mod's value out of its own context, so the walk goes on beneath it.
		default:
			break;
		}
	}
	return SettingLayer::None;
}

SceneSettingsManager::SettingLayer SceneWidgetBinding::Guard::ResolveUpperLayer() const
{
	const auto periods = CoveredPeriodMask();

	// Highest first: the topmost supplier is the one that reaches the scene. A tombstone up there only
	// empties its own context, so it outranks nothing.
	const auto& upper = GetUpperContexts(contextId);
	for (auto context = upper.rbegin(); context != upper.rend(); ++context)
		if (const auto layer = FoldContextLayer(*context, identity.featureShortName, identity, periods);
			SuppliesValue(layer))
			return layer;
	return SettingLayer::None;
}

bool SceneWidgetBinding::Guard::IsCoveredSlot(int a_slot) const
{
	return a_slot == armedSlot;
}

bool SceneWidgetBinding::Guard::HasAllCoveredEntries() const
{
	for (const auto& component : components)
		for (int slot = 0; slot < kPeriodCount; ++slot)
			if (IsCoveredSlot(slot) && !component.periodEntries[static_cast<size_t>(slot)])
				return false;
	return true;
}

std::optional<size_t> SceneWidgetBinding::Guard::PrimaryEntry(const Component& a_component) const
{
	if (const auto armed = a_component.periodEntries[static_cast<size_t>(armedSlot)])
		return armed;
	for (int slot = 0; slot < kPeriodCount; ++slot)
		if (const auto index = a_component.periodEntries[static_cast<size_t>(slot)])
			return index;
	return std::nullopt;
}

std::optional<size_t> SceneWidgetBinding::Guard::DisplayEntry(const Component& a_component) const
{
	const auto owned = PrimaryEntry(a_component);
	const auto entries = SceneSettingsManager::GetSingleton()->GetContextEntries(contextId);
	// During a drag, use the user's draft so it doesn't snap back to the winning preset between frames.
	if (owned && ImGui::GetActiveID() == ImGui::GetID(label))
		return owned;
	if (const auto* supplier = FindSupplyingEntry(contextId, ComponentIdentity(a_component), CoveredPeriodMask());
		supplier && !supplier->deleted)
		return static_cast<size_t>(supplier - entries.data());
	return owned;
}

std::vector<size_t> SceneWidgetBinding::Guard::CollectOwnedEntries() const
{
	std::vector<size_t> owned;
	for (const auto& component : components)
		for (int slot = 0; slot < kPeriodCount; ++slot)
			if (const auto index = component.periodEntries[static_cast<size_t>(slot)])
				owned.push_back(*index);
	return owned;
}

void SceneWidgetBinding::Guard::ForgetEntries()
{
	for (auto& component : components) {
		component.periodEntries = {};
		component.preset = {};
	}
	entryIndex.reset();
	state = State::Absent;
	mixedAcrossPeriods = false;
}

bool SceneWidgetBinding::Guard::EnsureEntries(bool a_deferSave)
{
	auto* manager = SceneSettingsManager::GetSingleton();

	// AddContextSetting may snapshot the feature as its base, so an edit an unbound widget already wrote
	// into the member is undone for the call. A holding-bound widget never touched it.
	ValueStorage live;
	if (!boundToHolding) {
		CopyValue(value, live.Data(value.kind), value.data, valueSize);
		CopyValue(value, value.data, preCall.Data(value.kind), valueSize);
	}

	bool added = false;
	ForEachCoveredSlot([&](const Component& a_component, int a_slot, const SceneContextId& a_slotContext) {
		if (a_component.periodEntries[static_cast<size_t>(a_slot)])
			return;
		const auto created = manager->AddContextSetting(a_slotContext, identity.featureShortName,
			identity.settingPath, a_component.settingKey, a_deferSave);
		added = added || created.has_value();
	});

	if (!boundToHolding)
		CopyValue(value, value.data, live.Data(value.kind), valueSize);

	// Insertion renumbers the entries behind it, so nothing may reuse the indices resolved earlier.
	if (added)
		ResolveState();
	return entryIndex.has_value();
}

void SceneWidgetBinding::Guard::BindDisplayValue()
{
	// The live member carries what the scene resolved for right now, not this page's value, and a
	// write into it would be snapshotted as the feature's base. Edits reach the scene via the resolver.
	boundToHolding = true;
	StoreHoldingValue();
}

void SceneWidgetBinding::Guard::StoreHoldingValue()
{
	// An address the scene leaves alone reads its base straight from the member.
	CopyValue(value, holding.Data(value.kind), value.data, valueSize);

	const auto entries = SceneSettingsManager::GetSingleton()->GetContextEntries(contextId);
	for (const auto& component : components) {
		const auto setting = ComponentIdentity(component);
		if (const auto preset = component.preset) {
			const auto* draft = SceneEntryTable::GetFeaturePresetDraft(*preset.entry);
			WriteComponent(holding.Data(value.kind), component, draft ? *draft : preset.entry->value);
		} else if (const auto* draft = SceneEntryTable::GetFeatureUserDraft(setting)) {
			WriteComponent(holding.Data(value.kind), component, *draft);
		} else if (const auto index = DisplayEntry(component); index && *index < entries.size()) {
			WriteComponent(holding.Data(value.kind), component, entries[*index].value);
		} else if (const auto* fallback = ResolveFallbackValue(component)) {
			WriteComponent(holding.Data(value.kind), component, *fallback);
		}
	}
}

const json* SceneWidgetBinding::Guard::ResolveFallbackValue(const Component& a_component) const
{
	auto* manager = SceneSettingsManager::GetSingleton();
	const auto setting = ComponentIdentity(a_component);

	const auto periods = CoveredPeriodMask();
	for (const auto& lower : GetLowerContexts(contextId)) {
		// A tombstone below only empties its own context, so the layers under it still supply.
		const auto* supplier = FindSupplyingEntry(lower, setting, periods);
		if (supplier && !supplier->deleted)
			return &supplier->value;
	}
	return manager->FindAppliedBaseline(setting);
}

void SceneWidgetBinding::Guard::WriteComponent(void* a_storage, const Component& a_component, const json& a_stored) const
{
	if (value.kind == Kind::String) {
		if (a_stored.is_string())
			*static_cast<std::string*>(a_storage) = a_stored.get_ref<const std::string&>();
		return;
	}
	if (value.kind == Kind::Bool) {
		bool checked;
		if (a_stored.is_boolean())
			checked = a_stored.get<bool>();
		else if (a_stored.is_number_integer())
			checked = a_stored.get<std::int64_t>() != 0;
		else
			return;
		*static_cast<bool*>(a_storage) = checked != a_component.setting->invertedDisplay;
		return;
	}
	if (!a_stored.is_number())
		return;

	double number = a_stored.get<double>() * widgetScale;
	if (transformedTemporary && !SceneSettingsInternal::ConvertCatalogNumericStoredToDisplay(*a_component.setting, a_stored.get<double>(), number))
		return;
	switch (value.kind) {
	case Kind::Int:
		*static_cast<int*>(a_storage) = static_cast<int>(number);
		break;
	case Kind::Float:
	case Kind::FloatVector:
		static_cast<float*>(a_storage)[a_component.widgetComponent] = static_cast<float>(number);
		break;
	case Kind::Scalar:
		GetScalarTraits(value.scalarType).write(a_storage, number);
		break;
	default:
		break;
	}
}

json SceneWidgetBinding::Guard::ReadEditedValue(const Component& a_component, const void* storage) const
{
	// The edit landed wherever the control was bound, which is not the caller's storage once the
	// member was carrying another layer's value.
	const auto* edited = storage ? storage : BoundData();
	if (value.kind == Kind::String)
		return *static_cast<const std::string*>(edited);

	if (value.kind == Kind::Bool) {
		// A checkbox over an int-backed member (uint flags cast to bool*) persists as an integer, or
		// validation rejects the edit and the entry keeps the base value the scene then re-applies.
		const bool checked = *static_cast<const bool*>(edited) != a_component.setting->invertedDisplay;
		return a_component.setting->valueType == SceneSettingsCatalog::ValueType::Integer ?
		           json(static_cast<std::int64_t>(checked)) :
		           json(checked);
	}

	double number = 0.0;
	switch (value.kind) {
	case Kind::Int:
		number = *static_cast<const int*>(edited);
		break;
	case Kind::Float:
	case Kind::FloatVector:
		number = static_cast<const float*>(edited)[a_component.widgetComponent];
		break;
	case Kind::Scalar:
		number = GetScalarTraits(value.scalarType).read(edited);
		break;
	default:
		return {};
	}
	number /= widgetScale;
	if (transformedTemporary && !SceneSettingsInternal::ConvertCatalogNumericDisplayToStored(*a_component.setting, number, number))
		return {};

	// The catalog owns the persisted type, so an integer setting never lands as a float literal.
	return a_component.setting->valueType == SceneSettingsCatalog::ValueType::Integer ?
	           json(static_cast<std::int64_t>(number)) :
	           json(number);
}

std::vector<SceneSettingsManager::EntryValueUpdate> SceneWidgetBinding::Guard::BuildEntryValueUpdates() const
{
	std::vector<SceneSettingsManager::EntryValueUpdate> updates;
	for (const auto& component : components) {
		const auto edited = ReadEditedValue(component);
		if (edited.is_null())
			continue;
		for (int slot = 0; slot < kPeriodCount; ++slot)
			if (const auto index = component.periodEntries[static_cast<size_t>(slot)])
				updates.push_back({ *index, edited });
	}
	return updates;
}

SceneSettingsManager::SettingLayer SceneWidgetBinding::Guard::ResolveVisibleLayer() const
{
	if (SuppliesValue(upperLayer))
		return upperLayer;
	return SuppliesValue(winningLayer) ? winningLayer : lowerLayer;
}

std::optional<ImVec4> SceneWidgetBinding::Guard::ResolveProvenanceColor() const
{
	if (std::ranges::any_of(components, [&](const Component& component) {
			return static_cast<bool>(component.preset);
		}))
		return Util::Colors::GetWarning();
	switch (ResolveVisibleLayer()) {
	case SettingLayer::User:
		return Util::Colors::GetSuccess();
	default:
		return std::nullopt;
	}
}

SceneWidgetBinding::Guard::PresetTarget SceneWidgetBinding::Guard::FindPresetTarget(
	const SceneSettingsManager::SettingIdentity& setting) const
{
	const auto periods = CoveredPeriodMask();
	const auto find = [&](const SceneContextId& context) -> PresetTarget {
		const auto* entry = FindSupplyingEntry(context, setting, periods, true);
		return entry && !entry->deleted ? PresetTarget{ entry, &context } : PresetTarget{};
	};
	const auto asPreset = [](PresetTarget target) {
		return target && target.entry->source == SceneSettingsManager::EntrySource::Overwrite ? target : PresetTarget{};
	};
	for (const auto& upper : GetUpperContexts(contextId) | std::views::reverse)
		if (const auto target = find(upper))
			return asPreset(target);
	if (const auto target = find(contextId))
		return asPreset(target);
	const auto& owned = GetContextUserEntryIndex(contextId, setting.featureShortName);
	if (const auto found = owned.find(setting); found != owned.end() && found->second[static_cast<size_t>(armedSlot)])
		return {};
	for (const auto& lower : GetLowerContexts(contextId))
		if (const auto target = find(lower))
			return asPreset(target);
	return {};
}

std::string SceneWidgetBinding::Guard::ResolveStatusTooltip() const
{
	switch (state) {
	case State::Unbound:
		return T("cs_editor.scene_override_unbound", "This setting cannot be part of a scene.");
	case State::Unavailable:
		if (metadata && SceneSettingsContextRules::GetSceneContextRules(contextId).requireNumeric &&
			!SceneSettingsCatalog::HasFlag(metadata->flags, SceneSettingsCatalog::SettingFlag::Transitionable))
			return T(TKEY("scene_override_requires_blending"), "This scene only supports blendable floating-point settings.");
		return T(TKEY("scene_override_unavailable"), "This setting isn't supported for this scene type.");
	default:
		break;
	}
	for (const auto& component : components)
		if (const auto preset = component.preset)
			return preset.entry->paused ? SceneEntryTable::GetPresetSourceHint(preset.entry) + "\n" +
			                                  T("feature.scene_manager.status.paused", "Paused") :
			                              SceneEntryTable::GetPresetSourceHint(preset.entry);
	return ResolveVisibleLayer() == SettingLayer::User ? T(TKEY("baseline_edited"), "Green: Your edited value.") : "";
}

void SceneWidgetBinding::Guard::EnableOverride()
{
	auto* manager = SceneSettingsManager::GetSingleton();
	// Any gesture on a killed value means the user wants it back, so it captures like an absent one.
	if (state == State::Deleted) {
		ClearSuppressions();
		ForgetEntries();
	}
	if (state == State::Absent || state == State::Deleted) {
		// Enabling captures the feature's current value as the override.
		// An aggregate fanned over six periods is 24 entries, so it saves once, not per entry.
		if (EnsureEntries(true))
			manager->SaveAllUserSettings();
		return;
	}
	if (state != State::Paused)
		return;
	// One click normalises a partly paused aggregate instead of inverting each entry.
	for (const auto index : CollectOwnedEntries()) {
		const auto entries = manager->GetContextEntries(contextId);
		if (index < entries.size() && entries[index].paused)
			manager->TogglePauseContextEntry(contextId, index);
	}
	ResolveState();
}

bool SceneWidgetBinding::Guard::TryEnableOverrideFromGesture()
{
	if (state != State::Absent && state != State::Deleted)
		return false;
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		return false;

	const ImGuiIO& io = ImGui::GetIO();
	const bool activate = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
	                      (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (io.KeyCtrl || io.KeyShift));
	if (!activate)
		return false;

	EnableOverride();
	return true;
}

bool SceneWidgetBinding::Guard::TryResetFromGesture()
{
	assert(state == State::Baseline);
	// An active item still owns its value (InputText keeps its own buffer), and MarkItemEdited expects none.
	if (ImGui::IsAnyItemActive() || !ImGui::IsItemHovered() || !ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		return false;
	return ResetToSource(ResetSource::Defaults);
}

const json* SceneWidgetBinding::Guard::FindResetValue(const Component& component, ResetSource source) const
{
	switch (source) {
	case ResetSource::Defaults:
		return FindStateLayerValue(*component.setting, &::State::GetFeatureDefaults);
	case ResetSource::BaseSettings:
		{
			std::string packId;
			return FindBaselineValue(*component.setting, packId);
		}
	}
	return nullptr;
}

bool SceneWidgetBinding::Guard::ResetToSource(ResetSource source)
{
	bool reset = false;
	for (const auto& component : components) {
		const auto* defaultValue = FindResetValue(component, source);
		if (!defaultValue || SceneSettingsManager::AppliedValuesEqual(*defaultValue, ReadEditedValue(component)))
			continue;
		WriteComponent(value.data, component, *defaultValue);
		reset = true;
	}
	if (!reset)
		return false;

	// The click never activates the item, so neither the edit flag nor an interaction end would report it.
	ImGui::MarkItemEdited(ImGui::GetItemID());
	if (const auto* context = SceneWidgetInterceptor::GetArmedContext())
		globals::state->InvalidateEditsUnderBaselines(context->feature);
	return true;
}

bool SceneWidgetBinding::Guard::DrawBaselineContextMenu()
{
	const auto controlItem = ImGui::GetCurrentContext()->LastItemData;
	std::optional<ResetSource> resetRequested;
	ImGui::PushID(label);
	const bool disabled = (controlItem.ItemFlags & ImGuiItemFlags_Disabled) != 0;
	if (BeginSettingContextMenu()) {
		const std::array resetSources{
			std::pair{ ResetSource::Defaults, T("menu.settings.reset_to_default", "Reset to default") },
			std::pair{ ResetSource::BaseSettings, T("menu.settings.reset_to_base_settings", "Reset to Base Settings preset") }
		};
		for (const auto& [source, resetCaption] : resetSources) {
			bool canReset = false;
			bool hasSource = false;
			std::string defaultText;
			for (const auto& component : components) {
				const auto* defaultValue = FindResetValue(component, source);
				if (!defaultValue)
					continue;
				hasSource = true;
				canReset |= !SceneSettingsManager::AppliedValuesEqual(*defaultValue, ReadEditedValue(component));
				std::string text;
				if (defaultValue->is_boolean()) {
					text = defaultValue->get<bool>() ? T("menu.settings.value_on", "On") : T("menu.settings.value_off", "Off");
				} else if (defaultValue->is_number_integer()) {
					for (std::size_t index = 0; index < component.setting->choiceCount; ++index) {
						const auto& choice = component.setting->choices[index];
						if (choice == defaultValue->get<std::int64_t>()) {
							if (component.setting->choiceDisplay) {
								const auto& display = component.setting->choiceDisplay[index];
								const std::string fallback(display.displayName);
								text = Util::Settings::StripImGuiId(T(display.displayNameKey, fallback.c_str()));
							} else {
								text = std::to_string(choice);
							}
							break;
						}
					}
				}
				if (text.empty() && defaultValue->is_number() &&
					component.setting->numericTransform == SceneSettingsCatalog::NumericTransform::Identity)
					text = std::format("{:.4g}", defaultValue->get<double>() * component.setting->displayScale);
				if (!text.empty()) {
					if (!defaultText.empty())
						defaultText += ", ";
					defaultText += text;
				}
			}
			if (!hasSource && source != ResetSource::Defaults)
				continue;
			auto resetLabel = std::string(resetCaption);
			if (!defaultText.empty())
				resetLabel += std::format(" ({})", defaultText);
			if (Util::RoundedMenuItem(resetLabel.c_str(), nullptr, false, canReset && !disabled))
				resetRequested = source;
		}
		if (!resetRequested) {
			if (const auto* context = SceneWidgetInterceptor::GetArmedContext(); context && context->feature) {
				for (const auto& component : components) {
					ImGui::PushID(component.setting);
					SettingsControl::DrawContextActions(*context->feature, *component.setting, disabled);
					ImGui::PopID();
				}
			}
		}
		ImGui::EndPopup();
	}
	ImGui::PopID();
	ImGui::GetCurrentContext()->LastItemData = controlItem;
	return resetRequested && ResetToSource(*resetRequested);
}

void SceneWidgetBinding::Guard::NavigateGreyedSetting() const
{
	if (state == State::Unbound) {
		if (auto* menu = globals::menu) {
			menu->IsEnabled = true;
			if (!identity.featureShortName.empty())
				menu->SelectFeatureMenu(identity.featureShortName);
		}
		return;
	}
	if (state == State::Unavailable) {
		// Prefer the catalog address: Unavailable leaves components empty so identity.settingKey
		// may never have been filled in ResolveComponents.
		const std::string feature = !identity.featureShortName.empty() ? identity.featureShortName :
		                                                                 (metadata ? std::string{ metadata->featureShortName } : std::string{});
		const auto path = !identity.settingPath.empty() ? identity.settingPath :
		                                                  (metadata ? SceneSettingsManager::SplitSettingPath(metadata->settingPath) :
																	  std::vector<std::string>{});
		const std::string key = !identity.settingKey.empty() ? identity.settingKey :
		                                                       (metadata ? std::string{ metadata->settingKey } : std::string{});
		if (feature.empty()) {
			if (auto* editor = EditorWindow::GetSingleton())
				editor->ShowNotification(
					T(TKEY("scene_override_setting_unknown"),
						"Couldn't identify this setting, so it can't be overridden for this place."),
					Util::Colors::GetWarning(), EditorWindow::kLongNotificationDuration);
			return;
		}
		SceneSettingsUI::OpenCurrentLocationForSetting(feature, path, key);
	}
}

void SceneWidgetBinding::Guard::Commit(bool editingPreset)
{
	if (editingPreset) {
		for (const auto& component : components) {
			auto edited = ReadEditedValue(component);
			if (SceneSettingsManager::AppliedValuesEqual(edited, ReadEditedValue(component, displayedBeforeEdit.Data(value.kind))))
				continue;
			const auto setting = ComponentIdentity(component);
			if (const auto preset = component.preset)
				SceneEntryTable::StageFeaturePresetEdit(*preset.entry, std::move(edited));
			else
				SceneEntryTable::StageFeatureUserEdit(setting, std::move(edited));
		}
		return;
	}
	// Editing a killed value revives it: the tombstone goes first, or it would block the new entry.
	if (state == State::Deleted) {
		ClearSuppressions();
		ForgetEntries();
	}

	// Nothing will hold the edit, so keeping it would silently rewrite the feature's base.
	if (!HasAllCoveredEntries() && !EnsureEntries(true)) {
		state = State::Unsupported;
		return;
	}

	SceneSettingsManager::GetSingleton()->UpdateContextEntryValues(
		contextId, BuildEntryValueUpdates(), commitDeferred);
}

bool SceneWidgetBinding::Guard::CanUseSceneActions() const
{
	if (state == State::Absent)
		return true;
	const bool editsPreset = state != State::Deleted && std::ranges::any_of(components, [](const Component& component) {
		return static_cast<bool>(component.preset);
	});
	return editsPreset ? !SceneSettingsManager::GetSingleton()->HasPendingContextEdit() : !CollectOwnedEntries().empty();
}

void SceneWidgetBinding::Guard::DrawSceneActions()
{
	if (state == State::Absent) {
		const auto padding = ImGui::GetStyle().FramePadding;
		const float height = std::ceil(std::max(ImGui::GetFontSize(), SceneEntryTable::GetActionButtonSize()));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(std::min(padding.x, ImGui::CalcTextSize(" ").x)),
															(height - ImGui::GetFontSize()) * 0.5f));
		const SKSE::stl::scope_exit restorePadding([] { ImGui::PopStyleVar(); });
		if (Util::Button(T("feature.scene_manager.action.add_setting", "Add Setting"), ImVec2(0.0f, height)))
			EnableOverride();
		return;
	}
	using Manager = SceneSettingsManager;
	auto* manager = Manager::GetSingleton();
	auto owned = CollectOwnedEntries();
	const bool suppressed = state == State::Deleted;
	std::vector<PresetTarget> presets;
	if (!suppressed)
		for (const auto& component : components)
			if (const auto target = component.preset)
				presets.push_back(target);

	const auto entries = manager->GetContextEntries(contextId);
	const bool editsPreset = !presets.empty();
	const bool presetBlocked = editsPreset && manager->HasPendingContextEdit();
	const bool available = CanUseSceneActions();
	bool active = editsPreset ? std::ranges::any_of(presets, [](const auto& target) { return !target.entry->paused; }) :
	                            std::ranges::any_of(owned, [&](size_t index) { return !entries[index].paused; });
	const auto toggleSize = SceneEntryTable::GetActionToggleSize();
	const float size = SceneEntryTable::GetActionButtonSize();
	const auto row = ImGui::GetCursorScreenPos();
	const auto nextAction = [&] {
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, row.y));
	};

	const auto* pauseLabel = available && !active ? T("feature.scene_manager.action.resume", "Resume") :
	                                                T("feature.scene_manager.action.pause", "Pause");
	{
		const Util::DisableGuard disabled(!available || presetBlocked || suppressed);
		ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + (size - toggleSize.y) * 0.5f));
		if (Util::FeatureToggle("##SceneOverrideActive", &active, toggleSize)) {
			if (editsPreset) {
				for (const auto& target : presets) {
					auto scene = *target.context;
					scene.period = target.entry->period;
					if (target.entry->paused == active)
						manager->TogglePauseContextEntry(scene, static_cast<size_t>(target.entry - manager->GetContextEntries(scene).data()));
				}
			} else {
				for (const auto index : owned)
					if (entries[index].paused == active)
						manager->TogglePauseContextEntry(contextId, index);
			}
		}
		Util::AddTooltip(presetBlocked ? SceneSettingsUI::PendingContextEditMessage() : pauseLabel, Util::kTooltipWhenDisabled);
	}
	nextAction();

	{
		const Util::DisableGuard disabled(!available || presetBlocked);
		if (Icons::Button("##SceneOverrideReset", Icons::TI(ICON_TI_ERASER), ImVec2(size, size))) {
			if (suppressed) {
				ClearSuppressions();
			} else if (editsPreset) {
				for (const auto& target : presets) {
					auto scene = *target.context;
					scene.period = target.entry->period;
					if (auto defaultValue = manager->ResolveContextEntryDefault(scene, *target.entry))
						SceneEntryTable::StageFeaturePresetEdit(*target.entry, std::move(*defaultValue));
				}
			} else {
				for (const auto index : owned)
					manager->RevertContextEntryToDefault(contextId, index);
			}
		}
	}
	Util::AddTooltip(T("feature.scene_manager.action.revert", "Revert to default"), Util::kTooltipWhenDisabled);
	nextAction();

	{
		const Util::DisableGuard disabled(!available || presetBlocked);
		const auto icon = suppressed ? Icons::TI(ICON_TI_RESTORE) : SceneActionIcons::kDelete;
		if (Icons::Button("##SceneOverrideDelete", icon, ImVec2(size, size))) {
			if (suppressed) {
				ClearSuppressions();
			} else if (editsPreset) {
				std::vector<const Manager::SettingEntry*> targets;
				for (const auto& target : presets)
					targets.push_back(target.entry);
				SceneEntryTable::RequestFeaturePresetDeletion(targets);
			} else {
				std::ranges::sort(owned, std::greater{});
				for (const auto index : owned)
					manager->RemoveContextSetting(contextId, index);
				ForgetEntries();
				return;
			}
		}
	}
	Util::AddTooltip(suppressed ? T(TKEY("scene_restore_preset_value"), "Restore Preset Value") :
								  T("feature.scene_manager.action.delete", "Delete"),
		Util::kTooltipWhenDisabled);

	if (contextId.type == Manager::SceneContextType::Location && !editsPreset && !suppressed && !owned.empty())
		SceneEntryTable::DrawFeatureTransition(contextId, owned);
}

void SceneWidgetBinding::Guard::DrawSceneActionFlyout()
{
	static std::map<ImGuiID, Util::FlyoutState> flyouts;
	auto& flyout = flyouts[ImGui::GetCurrentWindow()->ID];
	const auto controlItem = ImGui::GetCurrentContext()->LastItemData;
	if ((controlItem.ItemFlags & ImGuiItemFlags_Disabled) || !CanUseSceneActions())
		return;
	const auto id = controlItem.ID != 0 ? controlItem.ID : ImGui::GetID(label);
	{
		const auto style = SceneEntryTable::GetActionFlyoutStyle();
		if (auto popup = Util::FlyoutScope(flyout, id, false, controlItem.NavRect.Min, controlItem.NavRect.Max,
				style))
			DrawSceneActions();
	}
	ImGui::GetCurrentContext()->LastItemData = controlItem;
}

void SceneWidgetBinding::Guard::ClearSuppressions()
{
	auto* manager = SceneSettingsManager::GetSingleton();

	ForEachCoveredSlot([&](const Component& a_component, int, const SceneContextId& a_slotContext) {
		manager->ClearContextTombstone(a_slotContext, identity.featureShortName,
			identity.settingPath, a_component.settingKey);
	});
}

bool SceneWidgetBinding::Guard::Finish(bool a_changed)
{
	if (actionOutlinePushed) {
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
		actionOutlinePushed = false;
	}
	if (metadata && state == State::Baseline)
		SettingReveal::NotifyControl(*metadata);
	if (tintPushed) {
		Util::PopTintedFrameStyle();
		tintPushed = false;
	}
	if (mixedFlagPushed) {
		ImGui::PopItemFlag();
		mixedFlagPushed = false;
	}
	if (disabledOpened) {
		ImGui::EndDisabled();
		disabledOpened = false;
	}
	if (disabledByCaller) {
		ReportSettingHint(T(TKEY("setting_disabled_by_requirement"), "This setting is disabled by another option or requirement."));
		return false;
	}

	if (state == State::Unsupported) {
		if (replicaReadOnly) {
			ReportSettingHint(T(TKEY("scene_override_setting_unknown"),
				"Couldn't identify this setting, so it can't be overridden for this place."));
			return false;
		}
		if (a_changed)
			SceneWidgetInterceptor::RequestExternalCapture();
		return a_changed;
	}
	if (state == State::Baseline) {
		if (!IsSceneBaselineLayer(baselineLayer)) {
			a_changed |= TryResetFromGesture();
			a_changed |= DrawBaselineContextMenu();
		}
		if (ImGui::IsItemHovered(Util::kTooltipWhenDisabled)) {
			if (auto tooltip = ResolveBaselineTooltip(); !tooltip.empty()) {
				if (!IsSceneBaselineLayer(baselineLayer))
					tooltip.append("\n").append(T(TKEY("baseline_reset_hint"), "Right-click for options. Middle-click to restore the default."));
				ReportSettingHint(std::move(tooltip));
			}
		}
		return !IsSceneBaselineLayer(baselineLayer) && a_changed;
	}
	// Both were greyed and cannot commit a value in this scene.
	if (state == State::Unbound || state == State::Unavailable) {
		ReportSettingHint(ResolveStatusTooltip());
		if (ImGui::IsItemHovered(Util::kTooltipWhenDisabled)) {
			const bool ctrlClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			                       (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl));
			if (ctrlClick || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				NavigateGreyedSetting();
		}
		return false;
	}

	if (ImGui::IsItemHovered(Util::kTooltipWhenDisabled))
		ReportSettingHint(ResolveStatusTooltip());

	// A gesture on a paused or missing override starts editing its own value.
	const bool editingPreset = std::ranges::any_of(components, [&](const Component& component) {
		return static_cast<bool>(component.preset);
	});
	if (!editingPreset && TryEnableOverrideFromGesture()) {
		ResolveState();
		BindDisplayValue();
	}

	// Read the drag state before drawing the flyout.
	const bool dragging = ImGui::IsItemActive();
	const bool settled = ImGui::IsItemDeactivatedAfterEdit();

	if (a_changed) {
		commitDeferred = dragging;
		Commit(editingPreset);
	} else if (!editingPreset && settled && state == State::Active) {
		// One non-deferred pass so the debounced save always lands on release.
		commitDeferred = false;
		Commit(false);
	}
	// A drag held still would otherwise outlive the debounce and write the file mid-gesture.
	if (dragging)
		SceneSettingsManager::GetSingleton()->HoldDeferredSceneChanges();

	if (!dragging)
		DrawSceneActionFlyout();

	// The preset draft is detached from the feature until its transaction commits.
	return !editingPreset && a_changed;
}

void SceneWidgetBinding::Guard::OpenDisabled()
{
	ImGui::BeginDisabled();
	disabledOpened = true;
}

#undef I18N_KEY_PREFIX
