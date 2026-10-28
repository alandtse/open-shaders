#include "SnowCover.h"

#include "Profiler.h"
#include "SnowCoverMath.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
#include "Utils/FileSystem.h"
#include "Utils/MathUtils.h"
#include "Utils/Serialize.h"
#include "Utils/StringUtils.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.snow_cover."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SnowCover::UserSettings,
	Enable,
	EnableExpensiveFoliage,
	AffectHavok,
	AffectFloraTint,
	SnowHeightOffset,
	RooftopSnow,
	RooftopHeight,
	RooftopExposure)

namespace
{
	constexpr float MaxFrameSeconds = 0.1f;
	constexpr float MinAngleRange = 0.001f;
	constexpr float MaxHeightOffset = 20000.0f;
	constexpr float SnowBoundsPadding = 512.0f;
	constexpr float MapFilterPadding = 2.0f / 255.0f;
	constexpr float DisplacementInfluence = 5.0f;
	constexpr float FoliageTransitionWidth = 5000.0f;
	constexpr float SurvivalSnowOn = 0.5f;
	constexpr float SurvivalSnowOff = 0.4f;
	constexpr RE::FormID SurvivalRegionSpell = 0x87F;

	std::filesystem::path GetConfigPath(std::string_view name)
	{
		if (!Util::IsSinglePathComponent(name))
			throw std::invalid_argument("Invalid worldspace editor ID");
		return (Util::PathHelpers::GetShadersPath() / "SnowCover") / (std::string(name) + ".json");
	}
}

SnowCover::PerFrame SnowCover::GetCommonBufferData(bool inWorld)
{
	if (!loaded || !active.load(std::memory_order_relaxed) || (!inWorld && !globals::state->activeReflections) || globals::state->isLoadingMenuOpen)
		return {};
	if (frame.IsNewFrame())
		UpdateFrame();
	return perFrame;
}

void SnowCover::UpdateFrame()
{
	if (!active.load(std::memory_order_relaxed))
		return;
	ZoneScopedN("SnowCover::UpdateFrame");
	CS_PROFILE_CPU_SCOPE(globals::profiler, "SnowCover::UpdateFrameCPU");
	Reload();
	bool snowing = false;
	bool raining = false;
	snowingDensity = 0.0f;
	if (wsettings.EnableSnowCover && texturesLoaded && !Util::IsInterior()) {
		if (auto* sky = globals::game::sky) {
			if (auto* weather = sky->currentWeather) {
				snowing = weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
				raining = !snowing && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy);
				snowingDensity = Util::GetPrecipitationIntensity(weather);
			}
		}
	}
	if (auto* calendar = globals::game::calendar) {
		const float hours = calendar->GetHoursPassed();
		const float elapsed = lastGameHours >= 0.0f ? std::max(0.0f, hours - lastGameHours) : 0.0f;
		lastGameHours = hours;
		timeSnowing = SnowCoverMath::Accumulate(timeSnowing, elapsed, snowing, raining, snowingSpeed, meltingSpeed);
		perFrame.Month = Util::GetFractionalGameMonth(*calendar);
	}
	perFrame.SeasonalAltitude = SnowCoverMath::SeasonalAltitude(perFrame.Month, MaxSummerMonth, MaxWinterMonth, SummerHeightOffset, WinterHeightOffset);
	perFrame.TimeSnowing = timeSnowing;
	perFrame.SnowingDensity = snowingDensity;
	perFrame.settings = settings;
	perFrame.wsettings = wsettings;
	perFrame.wsettings.EnableSnowCover = wsettings.EnableSnowCover && texturesLoaded;
	UpdateFireMelt();
	UpdateSurvival();
}

void SnowCover::SetupResources()
{
	resourcesReady = true;
	if (!active.load(std::memory_order_relaxed))
		return;
	lastWorldspace.clear();
	LoadGeometryLists();
	UpdateFrame();
}

void SnowCover::Reset()
{
	const bool requested = settings.Enable != 0;
	if (active.load(std::memory_order_relaxed) == requested)
		return;
	active.store(requested, std::memory_order_relaxed);
	ResetRuntimeState();
	if (requested) {
		LoadGeometryLists();
		if (!lastWorldspace.empty() && Util::GetCurrentWorldspaceOrCellEditorID() == lastWorldspace)
			TryLoadTextures();
	}
}

void SnowCover::OnSceneTransitionReset(bool opening)
{
	if (!opening)
		return;
	lastWorldspace.clear();
	ResetRuntimeState();
}

void SnowCover::ResetRuntimeState()
{
	lastGameHours = -1.0f;
	timeSnowing = 0.0f;
	perFrame = {};
	frame = {};
	trackedFires.clear();
	fireBaseCache.clear();
	buildingBaseCache.clear();
	survivalSnowy.store(false, std::memory_order_relaxed);
	survivalSnowAmount = 0.0f;
	ReleaseTextures();
	fireScanTimer = FIRE_MELT_SCAN_INTERVAL;
	fireMeltSnap = true;
}

void SnowCover::LoadGeometryLists()
{
	whitelist.clear();
	blacklist.clear();
	geometryNames.clear();
	auto load = [&](std::string_view name) {
		std::ifstream file((Util::PathHelpers::GetShadersPath() / "SnowCover") / name);
		Util::ReadConfigLines(file, geometryNames);
	};
	load("whitelist.txt");
	const size_t whiteCount = geometryNames.size();
	load("blacklist.txt");
	for (size_t i = 0; i < geometryNames.size(); ++i)
		(i < whiteCount ? whitelist : blacklist).insert(geometryNames[i]);
}

void SnowCover::ResetWorldConfig()
{
	wsettings = WorldSettings{};
	MaxSummerMonth = DEFAULT_PEAK_SUMMER_MONTH;
	MaxWinterMonth = DEFAULT_PEAK_WINTER_MONTH;
	SummerHeightOffset = DEFAULT_SUMMER_HEIGHT_OFFSET;
	WinterHeightOffset = DEFAULT_WINTER_HEIGHT_OFFSET;
	snowingSpeed = 1.0f;
	meltingSpeed = 1.0f;
	mapMin = DEFAULT_MAP_MIN;
	mapMax = DEFAULT_MAP_MAX;
	UpdateMapTransform();
	mapTexture.clear();
	mainTexture.clear();
	alternateTexture.clear();
	mapTextureInput[0] = '\0';
	mainTextureInput[0] = '\0';
	alternateTextureInput[0] = '\0';
}

void SnowCover::UpdateMapTransform()
{
	if (!std::isfinite(mapMin.x) || !std::isfinite(mapMin.y))
		mapMin = DEFAULT_MAP_MIN;
	if (!std::isfinite(mapMax.x) || !std::isfinite(mapMax.y))
		mapMax = DEFAULT_MAP_MAX;
	float2 extent = mapMax - mapMin;
	if (extent.x == 0.0f)
		extent.x = DEFAULT_MAP_MAX.x - DEFAULT_MAP_MIN.x;
	if (extent.y == 0.0f)
		extent.y = DEFAULT_MAP_MAX.y - DEFAULT_MAP_MIN.y;
	wsettings.mapScale = float2(1.0) / extent;
	wsettings.mapOffset = -mapMin * wsettings.mapScale;
}

void SnowCover::SaveConfig()
{
	ValidateSettings();
	if (lastWorldspace.empty()) {
		status = T(TKEY("no_worldspace"), "No named worldspace or cell is active.");
		return;
	}
	mapTexture = std::filesystem::path(mapTextureInput);
	mainTexture = std::filesystem::path(mainTextureInput);
	alternateTexture = std::filesystem::path(alternateTextureInput);
	json config = {
		{ "EnableSnowCover", wsettings.EnableSnowCover != 0 },
		{ "AffectGrassTint", wsettings.AffectGrassTint },
		{ "AffectTreeTint", wsettings.AffectTreeTint },
		{ "FoliageHeightOffset", wsettings.FoliageHeightOffset },
		{ "UVScale", wsettings.UVScale },
		{ "MaxSummerMonth", MaxSummerMonth },
		{ "MaxWinterMonth", MaxWinterMonth },
		{ "SummerHeightOffset", SummerHeightOffset },
		{ "WinterHeightOffset", WinterHeightOffset },
		{ "MapTexture", mapTexture.generic_string() },
		{ "MapZscale", wsettings.mapZscale },
		{ "BlendSmoothness", wsettings.BlendSmoothness },
		{ "WeatherFadeStart", wsettings.WeatherFadeStart },
		{ "WeatherFadeEnd", wsettings.WeatherFadeEnd },
		{ "ObjectFadeStart", wsettings.ObjectFadeStart },
		{ "ObjectFadeEnd", wsettings.ObjectFadeEnd },
		{ "ObjectFadeAmount", wsettings.ObjectFadeAmount },
		{ "ScreenSpaceScale", wsettings.ScreenSpaceScale },
		{ "LogMicrofacetDensity", wsettings.LogMicrofacetDensity },
		{ "MicrofacetRoughness", wsettings.MicrofacetRoughness },
		{ "DensityRandomization", wsettings.DensityRandomization },
		{ "MapMin", mapMin },
		{ "MapMax", mapMax },
		{ "MainTexture", mainTexture.generic_string() },
		{ "AltTexture", alternateTexture.generic_string() },
		{ "MainTint", wsettings.MainTint },
		{ "AltTint", wsettings.AltTint },
		{ "SnowingSpeed", snowingSpeed },
		{ "MeltingSpeed", meltingSpeed },
		{ "PeakMainAngle", wsettings.PeakMainAngle },
		{ "PeakAltAngle", wsettings.PeakAltAngle },
		{ "MinAngle", wsettings.MinAngle },
		{ "MaxAngle", wsettings.MaxAngle },
		{ "MainSpec", wsettings.MainSpec },
		{ "AltSpec", wsettings.AltSpec },
	};

	std::filesystem::path path;
	try {
		path = GetConfigPath(lastWorldspace);
		std::filesystem::create_directories(path.parent_path());
		if (!Util::FileHelpers::WriteJsonAtomically(path, config, 4, "Snow Cover")) {
			status = std::string(T(TKEY("save_failed"), "Could not save the worldspace config. See the log for details."));
			logger::error("[Snow Cover] Failed to write {}", path.generic_string());
			return;
		}
	} catch (const std::exception& e) {
		status = std::string(T(TKEY("save_failed"), "Could not save the worldspace config. See the log for details."));
		logger::error("[Snow Cover] Error saving {}: {}", path.generic_string(), e.what());
		return;
	}
	status = std::string(T(TKEY("saved"), "Worldspace config saved."));
}

void SnowCover::LoadWorldConfig(const json& config)
{
	wsettings.EnableSnowCover = config.value("EnableSnowCover", true);
	wsettings.AffectGrassTint = config.at("AffectGrassTint");
	wsettings.AffectTreeTint = config.at("AffectTreeTint");
	wsettings.FoliageHeightOffset = config.at("FoliageHeightOffset");
	wsettings.UVScale = config.at("UVScale");
	MaxSummerMonth = config.at("MaxSummerMonth");
	MaxWinterMonth = config.at("MaxWinterMonth");
	SummerHeightOffset = config.at("SummerHeightOffset");
	WinterHeightOffset = config.at("WinterHeightOffset");
	config.at("MapMin").get_to(mapMin);
	config.at("MapMax").get_to(mapMax);
	UpdateMapTransform();
	wsettings.mapZscale = config.at("MapZscale");
	wsettings.BlendSmoothness = config.at("BlendSmoothness");
	wsettings.WeatherFadeStart = config.value("WeatherFadeStart", DEFAULT_WEATHER_FADE_START);
	wsettings.WeatherFadeEnd = config.value("WeatherFadeEnd", DEFAULT_WEATHER_FADE_END);
	wsettings.ObjectFadeStart = config.value("ObjectFadeStart", DEFAULT_OBJECT_FADE_START);
	wsettings.ObjectFadeEnd = config.value("ObjectFadeEnd", DEFAULT_OBJECT_FADE_END);
	wsettings.ObjectFadeAmount = config.value("ObjectFadeAmount", DEFAULT_OBJECT_FADE_AMOUNT);
	wsettings.ScreenSpaceScale = config.at("ScreenSpaceScale");
	wsettings.LogMicrofacetDensity = config.at("LogMicrofacetDensity");
	wsettings.MicrofacetRoughness = config.at("MicrofacetRoughness");
	wsettings.DensityRandomization = config.at("DensityRandomization");
	config.at("MainTint").get_to(wsettings.MainTint);
	config.at("AltTint").get_to(wsettings.AltTint);
	snowingSpeed = config.at("SnowingSpeed");
	meltingSpeed = config.at("MeltingSpeed");
	wsettings.PeakMainAngle = config.at("PeakMainAngle");
	wsettings.PeakAltAngle = config.at("PeakAltAngle");
	wsettings.MinAngle = config.at("MinAngle");
	wsettings.MaxAngle = config.at("MaxAngle");
	wsettings.MainSpec = config.at("MainSpec");
	wsettings.AltSpec = config.at("AltSpec");
	mainTexture = config.value("MainTexture", std::string{});
	alternateTexture = config.value("AltTexture", std::string{});
	mapTexture = config.value("MapTexture", std::string{});
	strncpy_s(mainTextureInput, mainTexture.generic_string().c_str(), _TRUNCATE);
	strncpy_s(alternateTextureInput, alternateTexture.generic_string().c_str(), _TRUNCATE);
	strncpy_s(mapTextureInput, mapTexture.generic_string().c_str(), _TRUNCATE);
	ValidateSettings();
}

bool SnowCover::LoadTextures()
{
	const auto basePath = Util::PathHelpers::GetShadersPath() / "SnowCover";
	const auto dataPath = Util::PathHelpers::GetDataPath();
	auto load = [&](const std::filesystem::path& path, std::string_view name) {
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		ImVec2 size;
		const auto resourcePath = path.lexically_relative(dataPath).make_preferred().string();
		const auto resourceName = std::format("SnowCover::{}", name);
		if (!Util::LoadDDSTextureFromFile(globals::d3d::device, resourcePath.c_str(), view.put(), size, resourceName.c_str())) {
			logger::warn("Snow Cover: could not load {}", path.generic_string());
			return view;
		}
		return view;
	};
	constexpr std::array suffixes{ ".dds", "_n.dds", "_rmaos.dds" };
	constexpr std::array names{ "SnowAlbedo", "SnowNormal", "SnowRmaos", "IceAlbedo", "IceNormal", "IceRmaos" };
	for (size_t i = 0; i < suffixes.size(); ++i) {
		if (!mainTexture.empty())
			views[i] = load(Util::JoinRelativePath(dataPath, mainTexture).concat(suffixes[i]), names[i]);
		if (!views[i])
			views[i] = load(basePath / "default" / (std::string("main") + suffixes[i]), names[i]);
		if (!alternateTexture.empty())
			views[i + suffixes.size()] = load(Util::JoinRelativePath(dataPath, alternateTexture).concat(suffixes[i]), names[i + suffixes.size()]);
		if (!views[i + suffixes.size()])
			views[i + suffixes.size()] = views[i];
	}
	if (!mapTexture.empty()) {
		const auto path = Util::JoinRelativePath(dataPath, mapTexture).concat(".dds");
		views.back() = load(path, "SnowMap");
		if (views.back())
			Util::LoadR8HeightMapFromDDS(path, snowMap);
	}
	if (!views.back()) {
		constexpr float neutralHeight = 0.5f;
		views.back() = Util::CreateConstantFloatTexture(globals::d3d::device, neutralHeight, "SnowCover::SnowMap");
	}
	return std::ranges::all_of(views, [](const auto& view) { return view != nullptr; });
}

void SnowCover::ReleaseTextures()
{
	texturesLoaded = false;
	ID3D11ShaderResourceView* empty[std::tuple_size_v<decltype(views)>]{};
	if (globals::d3d::context)
		globals::d3d::context->PSSetShaderResources(FIRST_SRV_SLOT, static_cast<uint32_t>(views.size()), empty);
	for (auto& view : views)
		view = nullptr;
	snowMap = {};
}

bool SnowCover::TryLoadTextures()
{
	try {
		texturesLoaded = LoadTextures();
	} catch (const std::exception& e) {
		logger::error("Snow Cover: {}: {}", lastWorldspace, e.what());
	}
	if (!texturesLoaded) {
		ReleaseTextures();
		status = T(TKEY("texture_failed"), "Required snow textures could not be loaded. See the log for details.");
	}
	return texturesLoaded;
}

void SnowCover::Reload()
{
	const auto current = Util::GetCurrentWorldspaceOrCellEditorID();
	if (current == lastWorldspace)
		return;
	lastWorldspace = current;
	ResetWorldConfig();
	ReleaseTextures();
	trackedFires.clear();
	fireBaseCache.clear();
	buildingBaseCache.clear();
	fireScanTimer = FIRE_MELT_SCAN_INTERVAL;
	fireMeltSnap = true;
	if (current.empty()) {
		status = T(TKEY("no_worldspace"), "No named worldspace or cell is active.");
		return;
	}
	try {
		const auto path = GetConfigPath(current);
		std::ifstream file(path);
		if (!file) {
			status = T(TKEY("no_config"), "No snow config exists for this worldspace or cell.");
			return;
		}
		LoadWorldConfig(json::parse(file));
		if (!TryLoadTextures())
			return;
		status = T(TKEY("loaded"), "Worldspace config loaded.");
	} catch (const std::exception& e) {
		wsettings.EnableSnowCover = false;
		status = T(TKEY("config_failed"), "Could not load the snow config. See the log for details.");
		logger::error("Snow Cover: {}: {}", current, e.what());
	}
}

void SnowCover::ValidateSettings()
{
	settings.Enable = settings.Enable != 0;
	settings.EnableExpensiveFoliage = settings.EnableExpensiveFoliage != 0;
	settings.AffectHavok = settings.AffectHavok != 0;
	settings.AffectFloraTint = settings.AffectFloraTint != 0;
	settings.SnowHeightOffset = Util::ClampFinite(settings.SnowHeightOffset, -MaxHeightOffset, MaxHeightOffset, 0.0f);
	settings.RooftopSnow = Util::ClampFinite(settings.RooftopSnow, 0.0f, MaxHeightOffset, DEFAULT_ROOFTOP_SNOW);
	settings.RooftopHeight = Util::ClampFinite(settings.RooftopHeight, 32.0f, 1024.0f, DEFAULT_ROOFTOP_HEIGHT);
	settings.RooftopExposure = Util::ClampFinite(settings.RooftopExposure, 0.0f, 0.95f, DEFAULT_ROOFTOP_EXPOSURE);
	fireMeltSettings.RadiusScale = Util::ClampFinite(fireMeltSettings.RadiusScale, 1.0f, 16.0f, 4.0f);
	fireMeltSettings.Strength = Util::ClampFinite(fireMeltSettings.Strength, 0.0f, 1.0f, 1.0f);
	MaxSummerMonth = std::min(MaxSummerMonth, 11u);
	MaxWinterMonth = std::min(MaxWinterMonth, 11u);
	SummerHeightOffset = Util::ClampFinite(SummerHeightOffset, -MaxHeightOffset, MaxHeightOffset, DEFAULT_SUMMER_HEIGHT_OFFSET);
	WinterHeightOffset = Util::ClampFinite(WinterHeightOffset, -MaxHeightOffset, MaxHeightOffset, DEFAULT_WINTER_HEIGHT_OFFSET);
	snowingSpeed = Util::ClampFinite(snowingSpeed, 0.01f, 10.0f, 1.0f);
	meltingSpeed = Util::ClampFinite(meltingSpeed, 0.01f, 10.0f, 1.0f);
	wsettings.BlendSmoothness = Util::ClampFinite(wsettings.BlendSmoothness, 1.0f, 10000.0f, DEFAULT_BLEND_SMOOTHNESS);
	wsettings.MinAngle = Util::ClampFinite(wsettings.MinAngle, 0.0f, 1.0f - MinAngleRange, DEFAULT_MIN_ANGLE);
	wsettings.MaxAngle = Util::ClampFinite(wsettings.MaxAngle, wsettings.MinAngle + MinAngleRange, 1.0f, std::clamp(DEFAULT_MAX_ANGLE, wsettings.MinAngle + MinAngleRange, 1.0f));
	wsettings.PeakMainAngle = Util::ClampFinite(wsettings.PeakMainAngle, 0.0f, 1.0f, DEFAULT_PEAK_MAIN_ANGLE);
	wsettings.PeakAltAngle = Util::ClampFinite(wsettings.PeakAltAngle, 0.0f, 1.0f, DEFAULT_PEAK_ALT_ANGLE);
	wsettings.UVScale = Util::ClampFinite(wsettings.UVScale, 0.01f, 10.0f, DEFAULT_UV_SCALE);
	wsettings.WeatherFadeStart = Util::ClampFinite(wsettings.WeatherFadeStart, 0.0f, 200000.0f, DEFAULT_WEATHER_FADE_START);
	wsettings.WeatherFadeEnd = Util::ClampFinite(wsettings.WeatherFadeEnd, wsettings.WeatherFadeStart + 1.0f, 300000.0f, std::clamp(DEFAULT_WEATHER_FADE_END, wsettings.WeatherFadeStart + 1.0f, 300000.0f));
	wsettings.ObjectFadeStart = Util::ClampFinite(wsettings.ObjectFadeStart, 0.0f, 40000.0f, DEFAULT_OBJECT_FADE_START);
	wsettings.ObjectFadeEnd = Util::ClampFinite(wsettings.ObjectFadeEnd, wsettings.ObjectFadeStart + 1.0f, 40001.0f, std::clamp(DEFAULT_OBJECT_FADE_END, wsettings.ObjectFadeStart + 1.0f, 40001.0f));
	wsettings.ObjectFadeAmount = Util::ClampFinite(wsettings.ObjectFadeAmount, 0.0f, 1.0f, DEFAULT_OBJECT_FADE_AMOUNT);
	wsettings.MainSpec = Util::ClampFinite(wsettings.MainSpec, 0.0f, 1.0f, DEFAULT_MAIN_SPEC);
	wsettings.AltSpec = Util::ClampFinite(wsettings.AltSpec, 0.0f, 1.0f, DEFAULT_ALT_SPEC);
	wsettings.FoliageHeightOffset = Util::ClampFinite(wsettings.FoliageHeightOffset, -MaxHeightOffset, MaxHeightOffset, DEFAULT_FOLIAGE_EFFECT_OFFSET);
	wsettings.mapZscale = Util::ClampFinite(wsettings.mapZscale, 0.1f, 1000000.0f, DEFAULT_MAP_ZSCALE);
	wsettings.ScreenSpaceScale = Util::ClampFinite(wsettings.ScreenSpaceScale, 1.0f, 3.0f, DEFAULT_GLINT_1);
	wsettings.LogMicrofacetDensity = Util::ClampFinite(wsettings.LogMicrofacetDensity, 0.0f, 40.0f, DEFAULT_GLINT_2);
	wsettings.MicrofacetRoughness = Util::ClampFinite(wsettings.MicrofacetRoughness, 0.001f, 1.0f, DEFAULT_GLINT_3);
	wsettings.DensityRandomization = Util::ClampFinite(wsettings.DensityRandomization, 0.001f, 5.0f, DEFAULT_GLINT_4);
	for (auto* tint : { &wsettings.MainTint, &wsettings.AltTint }) {
		tint->x = Util::ClampFinite(tint->x, 0.0f, 1.0f, 1.0f);
		tint->y = Util::ClampFinite(tint->y, 0.0f, 1.0f, 1.0f);
		tint->z = Util::ClampFinite(tint->z, 0.0f, 1.0f, 1.0f);
		tint->w = Util::ClampFinite(tint->w, 0.0f, 1.0f, 1.0f);
	}
}

void SnowCover::Prepass()
{
	if (loaded && active.load(std::memory_order_relaxed) && wsettings.EnableSnowCover && texturesLoaded) {
		ID3D11ShaderResourceView* srvs[std::tuple_size_v<decltype(views)>]{};
		for (size_t i = 0; i < views.size(); ++i)
			srvs[i] = views[i].get();
		globals::d3d::context->PSSetShaderResources(FIRST_SRV_SLOT, (uint)views.size(), srvs);
	}
}

void SnowCover::LoadSettings(json& o_json)
{
	settings = o_json;
	const FireMeltSettings defaults{};
	fireMeltSettings.Enabled = o_json.value("FireMeltEnabled", defaults.Enabled);
	fireMeltSettings.RadiusScale = o_json.value("FireMeltRadiusScale", defaults.RadiusScale);
	fireMeltSettings.Strength = o_json.value("FireMeltStrength", defaults.Strength);
	survivalEnabled = o_json.value("SurvivalEnabled", true);
	ValidateSettings();
	if (!resourcesReady)
		active.store(settings.Enable != 0, std::memory_order_relaxed);
}

void SnowCover::SaveSettings(json& o_json)
{
	o_json = settings;
	o_json["FireMeltEnabled"] = fireMeltSettings.Enabled;
	o_json["FireMeltRadiusScale"] = fireMeltSettings.RadiusScale;
	o_json["FireMeltStrength"] = fireMeltSettings.Strength;
	o_json["SurvivalEnabled"] = survivalEnabled;
}

void SnowCover::RestoreDefaultSettings()
{
	settings = {};
	fireMeltSettings = {};
	survivalEnabled = true;
	if (!resourcesReady)
		active.store(false, std::memory_order_relaxed);
}

bool SnowCover::ScanFireSources()
{
	ZoneScopedN("SnowCover::ScanFireSources");
	CS_PROFILE_CPU_SCOPE(globals::profiler, "SnowCover::ScanFireSourcesCPU");
	for (auto& fire : trackedFires)
		++fire.missedScans;

	auto* tes = globals::game::tes;
	auto* player = globals::game::player;
	if (!tes || !player)
		return false;

	const auto eye = player->GetPosition();
	uint32_t classified = 0;
	fireClusters.clear();

	tes->ForEachReferenceInRange(player, FIRE_MELT_MAX_DISTANCE, [&](RE::TESObjectREFR* a_ref) {
		if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted())
			return RE::BSContainer::ForEachResult::kContinue;
		auto* base = a_ref->GetBaseObject();
		if (!Util::IsPlacedFireCandidate(base))
			return RE::BSContainer::ForEachResult::kContinue;
		const auto cached = fireBaseCache.find(base->GetFormID());
		const bool known = cached != fireBaseCache.end();
		if ((known && !cached->second) || (!known && classified >= MAX_FIRE_BASE_CLASSIFICATIONS_PER_SCAN))
			return RE::BSContainer::ForEachResult::kContinue;
		auto* root = a_ref->Get3D();
		if (!root)
			return RE::BSContainer::ForEachResult::kContinue;

		bool hasFire = false;
		fireSamples.clear();
		Util::CollectFireGeometry(root, false, hasFire, fireSamples);
		if (!known) {
			fireBaseCache.emplace(base->GetFormID(), hasFire);
			++classified;
		}

		const auto refPosition = a_ref->GetPosition();
		const size_t first = fireClusters.size();
		for (const auto& sample : fireSamples) {
			if (sample.center.GetSquaredDistance(refPosition) > FIRE_MELT_MAX_REF_OFFSET * FIRE_MELT_MAX_REF_OFFSET)
				continue;
			auto cluster = std::find_if(fireClusters.begin() + first, fireClusters.end(), [&](const FireCluster& a_cluster) {
				return a_cluster.bound.center.GetSquaredDistance(sample.center) <= FIRE_MELT_CLUSTER_DISTANCE * FIRE_MELT_CLUSTER_DISTANCE;
			});
			if (cluster != fireClusters.end())
				cluster->bound = Util::MergeBounds(cluster->bound, sample);
			else
				fireClusters.push_back({ a_ref->GetFormID(), static_cast<uint32_t>(fireClusters.size() - first), sample, 0.0f });
		}
		return RE::BSContainer::ForEachResult::kContinue;
	});

	for (auto& cluster : fireClusters)
		cluster.distanceSq = eye.GetSquaredDistance(cluster.bound.center);
	const size_t keep = std::min<size_t>(fireClusters.size(), MAX_TRACKED_FIRES);
	std::partial_sort(fireClusters.begin(), fireClusters.begin() + keep, fireClusters.end(), [](const FireCluster& a, const FireCluster& b) { return a.distanceSq < b.distanceSq; });

	for (size_t i = 0; i < keep; ++i) {
		const auto& cluster = fireClusters[i];
		const float radius = std::clamp(cluster.bound.radius, FIRE_MELT_MIN_RADIUS, FIRE_MELT_MAX_RADIUS);
		auto fire = std::find_if(trackedFires.begin(), trackedFires.end(), [&](const TrackedFire& a_fire) {
			return a_fire.refID == cluster.refID && a_fire.cluster == cluster.cluster;
		});
		if (fire == trackedFires.end()) {
			trackedFires.push_back({ cluster.refID, cluster.cluster, cluster.bound.center, radius, cluster.bound.center, radius, fireMeltSnap ? 1.0f : 0.0f, 0, 1 });
			continue;
		}
		fire->samples = std::min(fire->samples + 1, FIRE_MELT_FOOTPRINT_SAMPLES);
		const float sampleBlend = 1.0f / static_cast<float>(fire->samples);
		fire->sampleCenter += (cluster.bound.center - fire->sampleCenter) * sampleBlend;
		fire->sampleRadius += (radius - fire->sampleRadius) * sampleBlend;
		fire->missedScans = 0;
	}
	return classified < MAX_FIRE_BASE_CLASSIFICATIONS_PER_SCAN;
}

void SnowCover::UpdateFireMelt()
{
	if (!perFrame.wsettings.EnableSnowCover || !fireMeltSettings.Enabled || fireMeltSettings.Strength <= 0.0f || globals::state->isLoadingMenuOpen) {
		trackedFires.clear();
		fireScanTimer = FIRE_MELT_SCAN_INTERVAL;
		fireMeltSnap = true;
		UploadFireMelt();
		return;
	}

	const float dt = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, MaxFrameSeconds);
	fireScanTimer += dt;
	if (fireScanTimer >= FIRE_MELT_SCAN_INTERVAL) {
		fireScanTimer = 0.0f;
		if (ScanFireSources())
			fireMeltSnap = false;
	}

	const float footprintBlend = 1.0f - std::exp(-dt / FIRE_MELT_FOOTPRINT_SMOOTHING);
	const float fadeStep = dt / FIRE_MELT_FADE_TIME;
	for (auto& fire : trackedFires) {
		fire.center += (fire.sampleCenter - fire.center) * footprintBlend;
		fire.radius += (fire.sampleRadius - fire.radius) * footprintBlend;
		fire.strength = std::clamp(fire.strength + (fire.missedScans < FIRE_MELT_GRACE_SCANS ? fadeStep : -fadeStep), 0.0f, 1.0f);
	}
	std::erase_if(trackedFires, [](const TrackedFire& a_fire) { return a_fire.missedScans >= FIRE_MELT_GRACE_SCANS && a_fire.strength <= 0.0f; });

	UploadFireMelt();
}

void SnowCover::UploadFireMelt()
{
	auto& fireMelt = perFrame.fireMelt;
	fireMelt.Count = 0;
	fireMelt.Strength = fireMeltSettings.Strength;
	fireMelt.RadiusScale = fireMeltSettings.RadiusScale;
	auto* player = globals::game::player;
	if (trackedFires.empty() || !player)
		return;

	const auto eye = player->GetPosition();
	fireOrder.clear();
	for (uint32_t i = 0; i < trackedFires.size(); ++i) {
		if (trackedFires[i].strength > 0.0f)
			fireOrder.emplace_back(eye.GetDistance(trackedFires[i].center), i);
	}
	const size_t ranked = std::min<size_t>(fireOrder.size(), MAX_FIRE_MELT_SOURCES + 1);
	std::partial_sort(fireOrder.begin(), fireOrder.begin() + ranked, fireOrder.end());

	const float slotCutoff = fireOrder.size() > MAX_FIRE_MELT_SOURCES ? fireOrder[MAX_FIRE_MELT_SOURCES].first : std::numeric_limits<float>::max();
	for (size_t i = 0; i < std::min<size_t>(ranked, MAX_FIRE_MELT_SOURCES); ++i) {
		const auto [distance, index] = fireOrder[i];
		const auto& fire = trackedFires[index];
		const float weight = fire.strength *
		                     std::clamp((FIRE_MELT_MAX_DISTANCE - distance) / FIRE_MELT_DISTANCE_FADE, 0.0f, 1.0f) *
		                     std::clamp((slotCutoff - distance) / FIRE_MELT_SLOT_FADE, 0.0f, 1.0f);
		const float radius = fire.radius * weight;
		if (radius >= 1.0f)
			fireMelt.Spheres[fireMelt.Count++] = float4{ fire.center.x, fire.center.y, fire.center.z, radius };
	}
}

std::function<void()> SnowCover::OnRenderPassBegin(const RE::BSRenderPass* pass)
{
	if (!active.load(std::memory_order_relaxed) || !wsettings.EnableSnowCover || !texturesLoaded || !pass || !pass->shader || pass->shader->shaderType.get() != RE::BSShader::Type::Lighting || !pass->geometry)
		return nullptr;
	auto* state = globals::state;
	constexpr uint32_t mask = static_cast<uint32_t>(State::ExtraShaderDescriptors::NoSnow) | static_cast<uint32_t>(State::ExtraShaderDescriptors::NoFoliageTint) |
	                          static_cast<uint32_t>(State::ExtraShaderDescriptors::SnowRooftop) | static_cast<uint32_t>(State::ExtraShaderDescriptors::SnowLODTree) |
	                          static_cast<uint32_t>(State::ExtraShaderDescriptors::SnowInactive);
	const uint32_t previous = state->permutationData.ExtraShaderDescriptor & mask;
	uint32_t flags = 0;
	auto* reference = pass->geometry->GetUserData();
	const char* name = pass->geometry->name.c_str();
	const std::string_view geometryName = name ? name : "";
	const bool actor = reference && reference->IsActor();
	const bool movable = reference && reference->CanBeMoved();
	const bool animated = pass->geometry->HasAnimation() || (reference && reference->GetBaseObject() && reference->GetBaseObject()->IsBoundAnimObject());
	if (actor || blacklist.contains(geometryName) || ((animated || movable) && !whitelist.contains(geometryName) && !(settings.AffectHavok && movable)))
		flags |= static_cast<uint32_t>(State::ExtraShaderDescriptors::NoSnow);
	if (!settings.AffectFloraTint && Util::IsHarvestableFlora(reference))
		flags |= static_cast<uint32_t>(State::ExtraShaderDescriptors::NoFoliageTint);
	if (settings.RooftopSnow > 0.0f && IsBuilding(reference))
		flags |= static_cast<uint32_t>(State::ExtraShaderDescriptors::SnowRooftop);
	if (!reference && Util::IsObjectLODTree(name, pass->shaderProperty))
		flags |= static_cast<uint32_t>(State::ExtraShaderDescriptors::SnowLODTree);
	if (IsSnowFree(pass, flags))
		flags |= static_cast<uint32_t>(State::ExtraShaderDescriptors::SnowInactive);
	if (flags == previous)
		return nullptr;
	state->permutationData.ExtraShaderDescriptor = (state->permutationData.ExtraShaderDescriptor & ~mask) | flags;
	return [state, previous]() {
		state->permutationData.ExtraShaderDescriptor = (state->permutationData.ExtraShaderDescriptor & ~mask) | previous;
	};
}

bool SnowCover::IsBuilding(RE::TESObjectREFR* reference)
{
	auto* base = reference ? reference->GetBaseObject() : nullptr;
	if (!base || (base->GetFormType() != RE::FormType::Static && base->GetFormType() != RE::FormType::MovableStatic))
		return false;
	if (const auto found = buildingBaseCache.find(base->GetFormID()); found != buildingBaseCache.end())
		return found->second;
	const bool building = Util::IsArchitecturalModel(base->As<RE::TESModel>());
	buildingBaseCache.emplace(base->GetFormID(), building);
	return building;
}

bool SnowCover::IsSnowFree(const RE::BSRenderPass* pass, uint32_t flags) const
{
	using Material = RE::BSShaderMaterial::Feature;
	const auto* property = pass->shaderProperty;
	const auto* material = property ? property->material : nullptr;
	if (!material || perFrame.TimeSnowing > 0.0f)
		return false;
	const auto feature = material->GetFeature();
	if (feature == Material::kMultiTexLand || feature == Material::kMultiTexLandLODBlend || feature == Material::kLODLand || feature == Material::kLODLandNoise)
		return false;
	const bool tree = feature == Material::kTreeAnim || (flags & static_cast<uint32_t>(State::ExtraShaderDescriptors::SnowLODTree)) != 0;
	if (!tree && (flags & static_cast<uint32_t>(State::ExtraShaderDescriptors::NoSnow)) != 0)
		return true;
	float displacement = 0.5f;
	if (feature == Material::kDefault && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexLighting))
		displacement = 0.5f * std::max(0.0f, static_cast<const BSLightingShaderMaterialPBR*>(material)->GetDisplacementScale());
	const auto& bound = pass->geometry->worldBound;
	const float radius = std::max(bound.radius, 0.0f);
	const auto mapMaximum = snowMap.Maximum((bound.center.x - radius) * wsettings.mapScale.x + wsettings.mapOffset.x,
		(bound.center.y - radius) * wsettings.mapScale.y + wsettings.mapOffset.y,
		(bound.center.x + radius) * wsettings.mapScale.x + wsettings.mapOffset.x,
		(bound.center.y + radius) * wsettings.mapScale.y + wsettings.mapOffset.y);
	if (!mapMaximum)
		return false;
	const float rooftop = (flags & static_cast<uint32_t>(State::ExtraShaderDescriptors::SnowRooftop)) != 0 ? settings.RooftopSnow : 0.0f;
	const float top = bound.center.z + radius + SnowBoundsPadding + rooftop;
	const float environment = (top - settings.SnowHeightOffset + (*mapMaximum + MapFilterPadding - 0.5f) * wsettings.mapZscale + perFrame.SeasonalAltitude) / wsettings.BlendSmoothness;
	if (!(environment + DisplacementInfluence * displacement < 0.0f))
		return false;
	return !tree || !wsettings.AffectTreeTint || environment < wsettings.FoliageHeightOffset / FoliageTransitionWidth;
}

void SnowCover::DataLoaded()
{
	if (survivalGlobal)
		return;
	auto* handler = RE::TESDataHandler::GetSingleton();
	auto* spell = handler ? handler->LookupForm<RE::SpellItem>(SurvivalRegionSpell, "ccqdrsse001-survivalmode.esl") : nullptr;
	if (!spell || spell->effects.size() != 3 || std::ranges::any_of(spell->effects, [](const auto* effect) { return !effect || !effect->conditions.head; }))
		return;
	survivalGlobal = RE::IFormFactory::Create<RE::TESGlobal>();
	if (!survivalGlobal)
		return;
	survivalGlobal->type = RE::TESGlobal::Type::kFloat;
	survivalGlobal->value = 0.0f;
	for (uint32_t i = 0; i < 3; ++i) {
		auto* condition = new RE::TESConditionItem();
		condition->data.comparisonValue.f = 1.0f;
		condition->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetGlobalValue;
		condition->data.functionData.params[0] = survivalGlobal;
		condition->data.flags.isOR = i == 2;
		condition->data.flags.opCode = i == 2 ? RE::CONDITION_ITEM_DATA::OpCode::kGreaterThanOrEqualTo : RE::CONDITION_ITEM_DATA::OpCode::kLessThan;
		condition->next = spell->effects[i]->conditions.head;
		spell->effects[i]->conditions.head = condition;
	}
}

void SnowCover::UpdateSurvival()
{
	float amount = 0.0f;
	const auto* player = globals::game::player;
	if (survivalEnabled && survivalGlobal && perFrame.wsettings.EnableSnowCover && player && !Util::IsInterior()) {
		const auto position = player->GetPosition();
		const float map = snowMap.Sample(position.x * wsettings.mapScale.x + wsettings.mapOffset.x, position.y * wsettings.mapScale.y + wsettings.mapOffset.y);
		const float environment = (position.z - settings.SnowHeightOffset + (map - 0.5f) * wsettings.mapZscale + perFrame.SeasonalAltitude) / wsettings.BlendSmoothness;
		const float weather = timeSnowing * timeSnowing * timeSnowing * std::max(500.0f, snowingDensity) / 500.0f;
		amount = std::clamp(std::max(environment, weather), 0.0f, 1.0f);
	}
	survivalSnowAmount = amount;
	if (amount <= SurvivalSnowOff)
		survivalSnowy.store(false, std::memory_order_relaxed);
	else if (amount >= SurvivalSnowOn)
		survivalSnowy.store(true, std::memory_order_relaxed);
}

void SnowCover::OnWeatherColorsUpdated(RE::Sky*)
{
	if (survivalGlobal)
		survivalGlobal->value = active.load(std::memory_order_relaxed) && survivalSnowy.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
}

void SnowCover::OnRuntimeDisabled()
{
	survivalSnowy.store(false, std::memory_order_relaxed);
	if (survivalGlobal)
		survivalGlobal->value = 0.0f;
}

void SnowCover::RegisterUxActions()
{
	FEATURE_COMMAND("Reload", "Reload the current Snow Cover worldspace config, textures and geometry lists without saving.", [](Feature* self, const json&) {
		auto* feature = static_cast<SnowCover*>(self);
		feature->lastWorldspace.clear();
		feature->LoadGeometryLists();
		feature->UpdateFrame();
	});
	FEATURE_COMMAND("SaveConfig", "Save the current Snow Cover worldspace config to its JSON file.", [](Feature* self, const json&) {
		static_cast<SnowCover*>(self)->SaveConfig();
	});
	FEATURE_COMMAND("WorldDefaults", "Restore the current Snow Cover worldspace controls to defaults without saving.", [](Feature* self, const json&) {
		auto* feature = static_cast<SnowCover*>(self);
		const auto enabled = feature->wsettings.EnableSnowCover;
		feature->ResetWorldConfig();
		feature->wsettings.EnableSnowCover = enabled;
	});
}

json SnowCover::GetDiagnostics()
{
	return {
		{ "Worldspace", lastWorldspace },
		{ "MasterEnabled", settings.Enable != 0 },
		{ "Enabled", perFrame.wsettings.EnableSnowCover != 0 },
		{ "CpuSnowBoundsAvailable", snowMap.IsLoaded() },
		{ "Month", perFrame.Month },
		{ "SeasonalAltitude", perFrame.SeasonalAltitude },
		{ "TimeSnowing", perFrame.TimeSnowing },
		{ "SnowingDensity", perFrame.SnowingDensity },
		{ "TrackedFires", trackedFires.size() },
		{ "FireMeltCount", perFrame.fireMelt.Count },
		{ "SurvivalAvailable", survivalGlobal != nullptr },
		{ "SurvivalSnowAmount", survivalSnowAmount },
		{ "SurvivalFreezing", survivalSnowy.load(std::memory_order_relaxed) }
	};
}
