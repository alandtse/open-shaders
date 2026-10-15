#include "SkySync.h"
#include "../I18n/I18n.h"
#include "RE/B/BSVolumetricLightingRenderData.h"

#include "Utils/DevBenchUx.h"
#include "Utils/Game.h"

#define I18N_KEY_PREFIX "feature.sky_sync."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SkySync::Settings,
	Enabled,
	UseAlternateSunPath,
	MoonLightSource,
	SunPath,
	CustomAngle,
	MinShadowElevation,
	ShadowTransitionDuration,
	DimSunlightUnderHorizon,
	DimVolumetricLighting,
	HorizonFadeHours,
	NewMoonIntensity,
	CrescentMoonIntensity,
	FullMoonIntensity)

void SkySync::DrawSettings()
{
	const char* sunPathNames[] = {
		T(TKEY("sun_path_southern"), "Southern Sky"),
		T(TKEY("sun_path_northern"), "Northern Sky"),
		T(TKEY("sun_path_vanilla"), "Vanilla"),
		T(TKEY("sun_path_custom"), "Custom")
	};
	const char* moonLightSourceNames[] = {
		T(TKEY("moon_light_source_brightest"), "Brightest"),
		T(TKEY("moon_light_source_masser"), "Masser"),
		T(TKEY("moon_light_source_secunda"), "Secunda")
	};

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("enabled_tooltip"), "Enable or disable Sky Sync features."));
	}

	ImGui::Checkbox(T(TKEY("use_alternate_sun_path"), "Use alternate sun path"), &settings.UseAlternateSunPath);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("use_alternate_sun_path_tooltip"), "Calculate sun position based on time of day and season instead of vanilla movement."));
	}

	if (settings.UseAlternateSunPath) {
		if (ImGui::SliderInt(T(TKEY("sun_path"), "Sun path"), &settings.SunPath, 0, static_cast<uint8_t>(SunPath::Count) - 1, sunPathNames[settings.SunPath], ImGuiSliderFlags_AlwaysClamp))
			SetSunAngle();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("sun_path_tooltip"), "Choose the trajectory the sun takes across the sky."));
		}

		if (settings.SunPath == static_cast<int32_t>(SunPath::Custom)) {
			if (ImGui::SliderFloat(T(TKEY("custom_angle"), "Custom angle"), &settings.CustomAngle, -90.0f, 90.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
				SetSunAngle();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("custom_angle_tooltip"), "Set a custom angle for the sun's trajectory."));
			}
		}
	}

	ImGui::SliderInt(T(TKEY("moon_light_source"), "Moon light source"), &settings.MoonLightSource, 0, static_cast<uint8_t>(MoonLightSource::Count) - 1, moonLightSourceNames[settings.MoonLightSource], ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("moon_light_source_tooltip"), "Select which moon casts shadows during the night."));
	}

	ImGui::SliderFloat(T(TKEY("min_shadow_elevation"), "Min Shadow Elevation"), &settings.MinShadowElevation, 0.0f, 45.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_shadow_elevation_tooltip"), "Zero follows the sun and moons without limiting elevation. Higher values shorten horizon shadows but separate lighting from the celestial position."));
	}

	ImGui::SliderFloat(T(TKEY("shadow_transition_duration"), "Shadow Transition Duration"), &settings.ShadowTransitionDuration, 0.0f, 500.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("shadow_transition_duration_tooltip"), "How long (in game-time units) the shadow direction takes to fade between sources. 100 = ~5 seconds at timescale 20."));
	}

	ImGui::Checkbox(T(TKEY("dim_sunlight_under_horizon"), "Dim Sunlight Under Horizon"), &settings.DimSunlightUnderHorizon);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("dim_sunlight_under_horizon_tooltip"), "Fade directional light to zero as the sun goes below the horizon."));
	}

	ImGui::Checkbox(T(TKEY("fade_volumetric_lighting"), "Fade Volumetric Lighting"), &settings.DimVolumetricLighting);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("fade_volumetric_lighting_tooltip"), "Also fade volumetric lighting with the directional dim around dawn and dusk."));
	}
	if (settings.DimSunlightUnderHorizon || settings.DimVolumetricLighting) {
		ImGui::SliderFloat(T(TKEY("horizon_fade_duration"), "Horizon Fade Duration"), &settings.HorizonFadeHours, 0.0f, MaxHorizonFadeHours, "%.1f h", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("horizon_fade_duration_tooltip"), "How long (in game hours) the dim eases out after sunset and back in before sunrise."));
		}
	}

	ImGui::SliderFloat(T(TKEY("new_moon_intensity"), "New Moon Intensity"), &settings.NewMoonIntensity, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("crescent_intensity"), "Crescent Intensity"), &settings.CrescentMoonIntensity, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("full_moon_intensity"), "Full Moon Intensity"), &settings.FullMoonIntensity, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);

	if (ImGui::TreeNodeEx(T(TKEY("debug"), "Debug"), ImGuiTreeNodeFlags_None)) {
		const char* CasterNames[] = { T(TKEY("caster_sun"), "Sun"), T(TKEY("caster_masser"), "Masser"), T(TKEY("caster_secunda"), "Secunda"), T(TKEY("caster_none"), "None") };
		const char* PhaseNames[] = { T(TKEY("phase_full"), "Full"), T(TKEY("phase_waning_gibbous"), "Waning Gibbous"), T(TKEY("phase_waning_quarter"), "Waning Quarter"), T(TKEY("phase_waning_crescent"), "Waning Crescent"), T(TKEY("phase_new"), "New"), T(TKEY("phase_waxing_crescent"), "Waxing Crescent"), T(TKEY("phase_waxing_quarter"), "Waxing Quarter"), T(TKEY("phase_waxing_gibbous"), "Waxing Gibbous") };

		auto getPhase = [&](const RE::Moon* moon) -> const char* {
			if (!moon || !moon->moonMesh)
				return T(TKEY("phase_unknown"), "Unknown");
			if (const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().shaderProperty.get())) {
				if (auto tex = prop->GetBaseTexture())
					return PhaseNames[static_cast<int>(Util::Moon::GetPhaseFromTexture(tex->name.c_str()))];
			}
			return T(TKEY("phase_unknown"), "Unknown");
		};

		auto drawMoonEntry = [&](const char* label, Caster caster, const char* phase) {
			auto& color = colors[static_cast<int>(caster)];
			ImVec4 swatch = { color.x, color.y, color.z, 1.0f };
			ImGui::ColorButton(label, swatch, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, { ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight() });
			ImGui::SameLine();
			ImGui::Text("%s  [%s]  color (%.3f, %.3f, %.3f, %.3f)", label, phase, color.x, color.y, color.z, color.w);
		};

		const auto sky = globals::game::sky;
		drawMoonEntry(T(TKEY("caster_masser"), "Masser"), Caster::Masser, sky ? getPhase(sky->masser) : T(TKEY("phase_unknown"), "Unknown"));
		drawMoonEntry(T(TKEY("caster_secunda"), "Secunda"), Caster::Secunda, sky ? getPhase(sky->secunda) : T(TKEY("phase_unknown"), "Unknown"));

		ImGui::Text(T(TKEY("debug_dim"), "Dim: %.3f"), currentDim);

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::Text(T(TKEY("debug_shadow_target"), "Shadow target: %s"), CasterNames[static_cast<int>(shadowFader.target)]);
		ImGui::Text(T(TKEY("debug_shadow_dir"), "Shadow dir:    (%.2f, %.2f, %.2f)"), shadowFader.currentDir.x, shadowFader.currentDir.y, shadowFader.currentDir.z);
		ImGui::Text(T(TKEY("debug_vl_intensity"), "VL intensity factor: %.3f"), settings.DimVolumetricLighting ? currentDim : 1.0f);
		if (shadowFader.transitioning) {
			const float t = settings.ShadowTransitionDuration > 0.0f ? shadowFader.fadeTimer / settings.ShadowTransitionDuration : 1.0f;
			ImGui::ProgressBar(t, { -1.0f, 0.0f }, "");
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::Text(T(TKEY("debug_transitioning"), "Transitioning %.0f%%"), t * 100.0f);
		} else {
			ImGui::TextDisabled("%s", T(TKEY("debug_no_transition"), "No transition"));
		}

		ImGui::TreePop();
	}
}

void SkySync::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.MoonLightSource = std::clamp(settings.MoonLightSource, static_cast<int32_t>(MoonLightSource::Brightest), static_cast<int32_t>(MoonLightSource::Secunda));
	settings.SunPath = std::clamp(settings.SunPath, static_cast<int32_t>(SunPath::Southern), static_cast<int32_t>(SunPath::Custom));
	settings.CustomAngle = std::clamp(settings.CustomAngle, -90.0f, 90.0f);
	settings.MinShadowElevation = std::clamp(settings.MinShadowElevation, 0.0f, 45.0f);
	settings.HorizonFadeHours = std::clamp(settings.HorizonFadeHours, 0.0f, MaxHorizonFadeHours);
	SetSunAngle();
}

void SkySync::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SkySync::RestoreDefaultSettings()
{
	settings = {};
	SetSunAngle();
}

void SkySync::PostPostLoad()
{
	if (Util::Moon::IsMoonAndStarsLoaded())
		logger::info("[Sky Sync] Moon and Stars detected, compatibility enabled");

	if (GetModuleHandle(L"EVLaS.dll")) {
		DisableOnConflict("EVLaS");
		return;
	}

	stl::detour_thunk<Sky_Update>(REL::RelocationID(25682, 26229));
	Util::SetCelestialTransitionHandlerAvailable(true);

	gSunPosition = reinterpret_cast<RE::NiPoint3*>(REL::RelocationID(527924, 414871).address());

	gVolumetricLighting = reinterpret_cast<RE::BSVolumetricLightingRenderData*>(
		REL::RelocationID(527719, 414629).address() - offsetof(RE::BSVolumetricLightingRenderData, color));

	logger::info("[Sky Sync] Installed hooks");
}

void SkySync::DataLoaded()
{
	const auto data = RE::TESDataHandler::GetSingleton();
	if (data && (data->LookupLoadedModByName("DVLaSS.esp"sv) || data->LookupLoadedLightModByName("DVLaSS.esp"sv)))
		DisableOnConflict("DVLaSS");
	if (const auto collection = globals::game::gameSettingCollection)
		gSunAlphaTransTime = collection->GetSetting("fSunAlphaTransTime");
}

void SkySync::GameLoaded()
{
	Util::RequestGameLoadTransition();
}

void SkySync::DisableOnConflict(std::string_view conflictName)
{
	// A conflicting mod is an expected, benign disable, not a load failure --
	// leave failedLoadedMessage unset so the cache classifier treats it as such.
	loaded = false;
	settings.Enabled = false;
	Util::SetCelestialTransitionHandlerAvailable(false);
	logger::warn("[Sky Sync] Disabled as {} has been detected, both cannot be used together", conflictName);
}

void SkySync::RegisterUxActions()
{
	FEATURE_QUERY("lightingState", "Return live celestial transition state: source indices (sun=0, Masser=1, Secunda=2, none=3), weights, raw and clamped world directions, moon node visibility and shader alpha, horizon dim, applied volumetric multiplier (1 when fading is disabled), sunlight color, climate boundaries and game hour. Read-only; no settings are saved.", ([](const Feature* a_self, const json&) -> json {
		const auto& self = *static_cast<const SkySync*>(a_self);
		const auto sky = globals::game::sky;
		const auto point = [](const RE::NiPoint3& value) { return json::array({ value.x, value.y, value.z }); };
		const auto& fader = self.shadowFader;
		json result{ { "enabled", self.loaded && self.settings.Enabled }, { "valid", self.celestialLightingValid },
			{ "frame", globals::game::graphicsState->GetFrameCount() }, { "hour", sky ? sky->currentGameHour : 0.0f },
			{ "source", static_cast<uint32_t>(fader.target) }, { "previousSource", static_cast<uint32_t>(fader.previousTarget) },
			{ "transitioning", fader.transitioning }, { "fadeTimer", fader.fadeTimer },
			{ "weights", { fader.lightWeights.x, fader.lightWeights.y, fader.lightWeights.z } },
			{ "moonAlpha", { self.colors[1].w, self.colors[2].w } },
			{ "horizonDim", self.currentDim }, { "volumetricFactor", self.settings.DimVolumetricLighting ? self.currentDim : 1.0f },
			{ "sunSetting", self.sunSetting }, { "sunRising", self.sunRising }, { "sunBelowHorizon", self.sunBelowHorizon } };
		if (sky && sky->root) {
			result["moons"] = json::array();
			for (const auto* moon : { sky->masser, sky->secunda }) {
				const auto* property = moon && moon->moonMesh ? skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().shaderProperty.get()) : nullptr;
				result["moons"].push_back({ { "rootHidden", !moon || !moon->root || moon->root->GetAppCulled() },
					{ "moonNodeHidden", !moon || !moon->moonNode || moon->moonNode->GetAppCulled() },
					{ "shaderAlpha", property ? json(property->kBlendColor.alpha) : json(nullptr) } });
			}
			result["celestialDirection"] = point(sky->root->world.rotate * fader.celestialDir);
			result["rawDirections"] = json::array();
			for (const auto& direction : self.rawDirections)
				result["rawDirections"].push_back(point(sky->root->world.rotate * direction));
			if (sky->sun && sky->sun->light)
				result["shadowDirection"] = point(sky->sun->light->GetWorldDirection());
			const auto& color = sky->skyColor[static_cast<uint32_t>(RE::TESWeather::ColorTypes::kSunlight)];
			result["sunlightColor"] = { color.red, color.green, color.blue };
			if (sky->currentClimate) {
				const auto& timing = sky->currentClimate->timing;
				result["sunriseHours"] = { timing.sunrise.begin / 6.0f, timing.sunrise.end / 6.0f };
				result["sunsetHours"] = { timing.sunset.begin / 6.0f, timing.sunset.end / 6.0f };
				const auto fade = GetSunFadeTimes(sky);
				result["sunFadeHours"] = { fade.sunriseBegin, fade.sunriseMiddle, fade.sunsetMiddle, fade.sunsetEnd };
			}
		}
		return result;
	}));
}

void SkySync::OnWeatherColorsComputed(RE::Sky* sky)
{
	if (!settings.Enabled || !celestialLightingValid || !sky || !sky->currentWeather)
		return;

	const float moonWeight = std::clamp(shadowFader.lightWeights.y + shadowFader.lightWeights.z, 0.0f, 1.0f);
	if (moonWeight <= 0.0f)
		return;

	const auto nightColor = [](const RE::TESWeather* weather) {
		return RE::NiColor(weather->colorData[RE::TESWeather::ColorTypes::kSunlight][RE::TESWeather::ColorTime::kNight]);
	};
	const float weatherBlend = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
	const auto current = nightColor(sky->currentWeather);
	const auto previous = sky->lastWeather ? nightColor(sky->lastWeather) : current;
	const auto moonColor = previous * (1.0f - weatherBlend) + current * weatherBlend;
	auto& sunlight = sky->skyColor[RE::TESWeather::ColorTypes::kSunlight];
	sunlight = sunlight * (1.0f - moonWeight) + moonColor * moonWeight;
}

void SkySync::OnSkyUpdateColors(RE::Sky* sky)
{
	if (!settings.Enabled || !sky)
		return;

	if (settings.DimSunlightUnderHorizon && currentDim < 1.0f) {
		auto& dirLight = sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kSunlight)];
		dirLight.red *= currentDim;
		dirLight.green *= currentDim;
		dirLight.blue *= currentDim;
	}

	if (gVolumetricLighting && settings.DimVolumetricLighting)
		gVolumetricLighting->intensity *= currentDim;
}

std::optional<float3> SkySync::GetCelestialLightWeights() const
{
	if (!loaded || !settings.Enabled || !celestialLightingValid)
		return std::nullopt;

	return shadowFader.lightWeights;
}

std::optional<RE::NiPoint3> SkySync::GetCelestialLightDirection() const
{
	const auto sky = globals::game::sky;
	if (!loaded || !settings.Enabled || !celestialLightingValid || !sky || !sky->root)
		return std::nullopt;

	auto direction = sky->root->world.rotate * shadowFader.celestialDir;
	if (direction.Unitize() <= FLT_EPSILON)
		return std::nullopt;
	return direction;
}

void SkySync::Sky_Update::thunk(RE::Sky* sky)
{
	func(sky);
	auto& skySync = globals::features::skySync;
	skySync.PreparePendingTransitions();
	if (skySync.Update(sky))
		Util::CompleteCelestialTransition();
}

void SkySync::PreparePendingTransitions()
{
	const auto request = Util::ConsumeCelestialTransitionRequest();
	if (!request.timeJump && !request.gameLoad)
		return;

	if (request.gameLoad) {
		shadowFader.Reset();
		celestialLightingValid = false;
		currentCell = nullptr;
		currentCellInterior = false;
		currentCellWorldspace = nullptr;
		currentSkyRotation = D3D11_FLOAT32_MAX;
	}

	immediateTransitionReady = true;
}

bool SkySync::Update(const RE::Sky* sky)
{
	celestialLightingValid = false;
	if (!settings.Enabled) {
		currentDim = 1.0f;
		const bool transitionCompleted = immediateTransitionReady;
		immediateTransitionReady = false;
		return transitionCompleted;
	}

	if (!sky)
		return false;

	const auto sun = sky->sun;
	const auto climate = sky->currentClimate;
	const auto player = RE::PlayerCharacter::GetSingleton();
	if (!sun || !climate || !player) {
		currentDim = 1.0f;
		return false;
	}

	const auto cell = player->GetParentCell();
	bool resetTransition = false;

	if (cell != currentCell) {
		resetTransition = true;
		const auto prevCell = currentCell;
		const bool resetFaderForCellChange = cell && prevCell &&
		                                     (cell->IsInteriorCell() != currentCellInterior ||
												 cell->GetRuntimeData().worldSpace != currentCellWorldspace);
		if (cell)
			SetSkyRotation(sky, cell);
		else
			currentCell = nullptr;  // keep the cache in sync so a cell-less frame doesn't reset every frame
		if (resetFaderForCellChange)
			shadowFader.Reset();
	}

	// Exterior worldspaces always run; interior cells require the sunlight-shadows flag.
	if (cell && cell->IsInteriorCell() && !cell->cellFlags.all(static_cast<RE::TESObjectCELL::Flag>(CellFlagExt::kSunlightShadows))) {
		currentDim = 1.0f;
		return false;
	}

	// Compute dim once per frame - used by OnSkyUpdateColors (if option on) and ShadowFader (always)
	if (sky->currentClimate) {
		const auto timing = GetSunFadeTimes(sky);
		const float hour = sky->currentGameHour;
		const float sunriseBegin = timing.sunriseBegin;
		const float sunriseMiddle = timing.sunriseMiddle;
		const float sunsetMiddle = timing.sunsetMiddle;
		const float sunsetEnd = timing.sunsetEnd;
		const float fadeHours = settings.HorizonFadeHours;

		// Hours elapsed from a to b, wrapping across midnight so gap windows survive past 24h.
		auto hoursBetween = [](float from, float to) {
			float d = to - from;
			return d < 0.0f ? d + 24.0f : d;
		};

		sunSetting = hour >= sunsetMiddle && hour < sunsetEnd;
		sunRising = hour >= sunriseBegin && hour < sunriseMiddle;
		sunBelowHorizon = hour >= sunsetEnd || hour < sunriseBegin;

		if (hour >= sunsetMiddle && hour < sunsetEnd) {
			// Dusk: sun dipping under the horizon, fade the directional light out.
			float range = sunsetEnd - sunsetMiddle;
			float t = range > 0.0f ? (hour - sunsetMiddle) / range : 1.0f;
			currentDim = std::sqrt(1.0f - t);
		} else if (fadeHours > 0.0f && hoursBetween(sunsetEnd, hour) < fadeHours) {
			// Caster has swapped to the moon but the colour is still dusk-bright; ease the dim back out.
			currentDim = hoursBetween(sunsetEnd, hour) / fadeHours;
		} else if (fadeHours > 0.0f && hoursBetween(hour, sunriseBegin) > 0.0f && hoursBetween(hour, sunriseBegin) <= fadeHours) {
			// Still on the moon but the colour is brightening toward dawn; ease the dim back in.
			currentDim = hoursBetween(hour, sunriseBegin) / fadeHours;
		} else if (hour >= sunriseBegin && hour < sunriseMiddle) {
			// Dawn: sun rising above the horizon, fade the directional light in.
			float range = sunriseMiddle - sunriseBegin;
			float t = range > 0.0f ? (hour - sunriseBegin) / range : 1.0f;
			currentDim = std::sqrt(t);
		} else {
			currentDim = 1.0f;
		}
	} else {
		currentDim = 1.0f;
		sunSetting = false;
		sunRising = false;
		sunBelowHorizon = false;
	}

	RE::NiPoint3 directions[3] = {};
	float intensities[3] = {};

	ProcessSun(sky, directions, intensities);
	ProcessMoon(sky, Caster::Masser, directions, intensities);
	ProcessMoon(sky, Caster::Secunda, directions, intensities);
	std::copy(std::begin(directions), std::end(directions), std::begin(rawDirections));

	const auto calendar = globals::game::calendar;
	const auto deltaTime = globals::game::deltaTime;
	float fadeAdvance = calendar && deltaTime ? std::max(*deltaTime * calendar->GetTimescale(), 0.0f) : 0.0f;

	// The clock can outrun real time (waiting, fast travel) or move while frames are paused
	// (console, scrubbing), so advance by whichever of the two elapsed more.
	const float gameHour = sky->currentGameHour;
	if (lastGameHour >= 0.0f) {
		float hourDelta = gameHour - lastGameHour;
		if (hourDelta > 12.0f)
			hourDelta -= 24.0f;
		else if (hourDelta < -12.0f)
			hourDelta += 24.0f;
		fadeAdvance = std::max(fadeAdvance, std::abs(hourDelta) * SecondsPerGameHour);
	}
	lastGameHour = gameHour;

	const bool transitionCompleted = immediateTransitionReady;
	shadowFader.Update(sky, directions, intensities, settings.ShadowTransitionDuration, fadeAdvance, transitionCompleted || resetTransition);
	const auto& weights = shadowFader.lightWeights;
	currentDim *= sunBelowHorizon ?
	                  weights.y * std::clamp(intensities[1], 0.0f, 1.0f) + weights.z * std::clamp(intensities[2], 0.0f, 1.0f) :
	                  weights.x * std::clamp(intensities[0], 0.0f, 1.0f);
	celestialLightingValid = true;
	immediateTransitionReady = false;
	return transitionCompleted;
}
void SkySync::SetSunAngle()
{
	switch (static_cast<SunPath>(settings.SunPath)) {
	case SunPath::Southern:
		sunAngle = SouthernSunAngle;
		break;
	case SunPath::Northern:
		sunAngle = NorthernSunAngle;
		break;
	case SunPath::Vanilla:
		sunAngle = VanillaSunAngle;
		break;
	case SunPath::Custom:
		sunAngle = 90.0f + settings.CustomAngle;
		break;
	default:;
	}
}

void SkySync::SetSkyRotation(const RE::Sky* sky, RE::TESObjectCELL* cell)
{
	// If the interior cell isn't initialised it won't have the north rotation extra data ready, skip for a frame
	if (cell->IsInteriorCell() && cell->cellState == static_cast<RE::TESObjectCELL::CellState>(0))
		return;

	currentCell = cell;
	currentCellInterior = cell->IsInteriorCell();
	currentCellWorldspace = cell->GetRuntimeData().worldSpace;
	const float rotation = cell->GetNorthRotation();
	if (rotation == currentSkyRotation)
		return;

	currentSkyRotation = rotation;
	sky->root->local.rotate = RE::NiMatrix3{ RE::NiPoint3{ 0.0f, 0.0f, -rotation } };
	RE::NiUpdateData updateData;
	sky->root->Update(updateData);
}

// --- Open Shaders fork: horizon-dip altitude correction, restored from the pre-#2408 SkySync.
// Rotates a celestial direction down by the apparent horizon dip for the player's elevation, so
// the sun/moons (and the shadow direction derived from them) don't sit too high when the player
// gains altitude. Upstream #2408 deleted this but still advertises the fix. Kept as additive
// helpers applied inside ProcessSun/ProcessMoon so upstream #2408 merges stay clean.
float SkySync::GetPlayerAltitude()
{
	const auto player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return 0.0f;
	const auto worldSpace = player->GetWorldspace();
	return worldSpace ? player->GetPositionZ() - worldSpace->GetDefaultWaterHeight() : 0.0f;
}

RE::NiPoint3 SkySync::GetApparentDirection(const RE::NiPoint3& dir, const float altitude)
{
	const float dipAngle = -std::atan(altitude / RenderDistance);
	float sinPhi, cosPhi;
	DirectX::XMScalarSinCosEst(&sinPhi, &cosPhi, dipAngle);

	const auto rotationAxis = dir.UnitCross({ 0.0f, 0.0f, 1.0f });
	const float axisDotDir = rotationAxis.Dot(dir);
	const auto axisCrossDir = rotationAxis.Cross(dir);
	const float oneMinusCosPhi = 1.0f - cosPhi;

	const float x = dir.x * cosPhi + axisCrossDir.x * sinPhi + rotationAxis.x * (axisDotDir * oneMinusCosPhi);
	const float y = dir.y * cosPhi + axisCrossDir.y * sinPhi + rotationAxis.y * (axisDotDir * oneMinusCosPhi);
	const float z = dir.z * cosPhi + axisCrossDir.z * sinPhi + rotationAxis.z * (axisDotDir * oneMinusCosPhi);

	RE::NiPoint3 rotated = { x, y, z };
	rotated.Unitize();
	return rotated;
}

void SkySync::ProcessSun(const RE::Sky* sky, RE::NiPoint3 dirs[], float intensities[])
{
	const auto sun = sky->sun;
	RE::NiPoint3 dir;
	float dist;

	if (settings.UseAlternateSunPath) {
		const auto timing = GetSunFadeTimes(sky);
		CalculateAlternateSunDirectionAndDistance(dir, dist, sky->currentGameHour, timing.sunriseBegin, timing.sunsetEnd, sunAngle);
	} else {
		CalculateSunDirectionAndDistance(sun, dir, dist);
		// Vanilla reuses the upper sun arc at night, when its disc is hidden.
		if (sunBelowHorizon)
			dir.z = -dir.z;
	}

	dir = GetApparentDirection(dir, GetPlayerAltitude());  // fork: altitude correction

	SetSunPosition(sun, dir, dist);
	HideSunOutsideFadeWindow(sky);

	dirs[static_cast<int>(Caster::Sun)] = dir;

	if (const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(sun->sunBase->GetGeometryRuntimeData().shaderProperty.get()))
		intensities[static_cast<int>(Caster::Sun)] = prop->kBlendColor.alpha;
}

SkySync::SunFadeTimes SkySync::GetSunFadeTimes(const RE::Sky* sky)
{
	// Match vanilla's float operations so exact fade boundaries agree.
	constexpr float HoursPerTimingUnit = 1.0f / 6.0f;
	auto middleHour = [](const RE::TESClimate::Timing::Interval& interval) {
		return (interval.end * HoursPerTimingUnit + interval.begin * HoursPerTimingUnit) * 0.5f;
	};
	const auto& timing = sky->currentClimate->timing;
	const float sunriseMiddle = middleHour(timing.sunrise);
	const float sunsetMiddle = middleHour(timing.sunset);
	if (!gSunAlphaTransTime)
		return { timing.sunrise.begin * HoursPerTimingUnit, sunriseMiddle, sunsetMiddle, timing.sunset.end * HoursPerTimingUnit };
	const float halfTransition = gSunAlphaTransTime->GetFloat() * 0.5f;
	return { sunriseMiddle - halfTransition, sunriseMiddle, sunsetMiddle, sunsetMiddle + halfTransition };
}

void SkySync::HideSunOutsideFadeWindow(const RE::Sky* sky)
{
	if (!gSunAlphaTransTime)
		return;
	const auto timing = GetSunFadeTimes(sky);
	const float hour = sky->currentGameHour;
	if (hour > timing.sunriseBegin && hour < timing.sunsetEnd)
		return;

	for (const auto& geometry : { sky->sun->sunBase, sky->sun->sunGlare }) {
		if (const auto prop = geometry ? skyrim_cast<RE::BSSkyShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get()) : nullptr)
			prop->kBlendColor.alpha = 0.0f;
	}
}

void SkySync::ProcessMoon(const RE::Sky* sky, const Caster type, RE::NiPoint3 dirs[], float intensities[])
{
	const int idx = static_cast<int>(type);
	colors[idx] = float4{};

	const auto moon = type == Caster::Masser ? sky->masser : sky->secunda;
	if (!Util::Moon::IsVisible(moon))
		return;

	auto dir = Util::Moon::GetFacingAxis(moon->root->local.rotate);

	dir = GetApparentDirection(dir, GetPlayerAltitude());  // fork: altitude correction

	dirs[idx] = dir;

	const float4& baseColor = type == Caster::Masser ? Util::Moon::MasserBaseColor : Util::Moon::SecundaBaseColor;
	float4 color = Util::Moon::GetBlendColor(moon, baseColor, settings.NewMoonIntensity, settings.CrescentMoonIntensity, settings.FullMoonIntensity);
	colors[idx] = color;

	const auto src = static_cast<MoonLightSource>(settings.MoonLightSource);
	const bool isValidSource = src == MoonLightSource::Brightest || (src == MoonLightSource::Masser && type == Caster::Masser) || (src == MoonLightSource::Secunda && type == Caster::Secunda);
	if (!isValidSource)
		return;

	intensities[idx] = color.w;
}

std::optional<RE::NiPoint3> SkySync::GetCelestialDirection(Caster caster) const
{
	if (caster == Caster::None)
		return std::nullopt;

	const auto index = static_cast<size_t>(caster);
	assert(index < std::size(rawDirections));
	const auto sky = globals::game::sky;
	if (!loaded || !settings.Enabled || !celestialLightingValid || !sky || !sky->root || index >= std::size(rawDirections))
		return std::nullopt;

	auto direction = sky->root->world.rotate * rawDirections[index];
	if (direction.Unitize() <= FLT_EPSILON)
		return std::nullopt;
	return direction;
}

inline void SkySync::CalculateSunDirectionAndDistance(const RE::Sun* sun, RE::NiPoint3& outDir, float& outDistance)
{
	outDir = sun->root->local.translate;
	if (outDistance = outDir.Unitize(); outDistance < FLT_EPSILON) {
		outDir = { 0.0f, 0.0f, 1.0f };
		outDistance = SunPeakDistance;
	}
}

inline void SkySync::CalculateAlternateSunDirectionAndDistance(RE::NiPoint3& outDir, float& outDist, const float time, const float sunrise, const float sunset, const float sunAngle)
{
	constexpr float HoursPerDay = 24.0f;
	const float daylightHours = sunset - sunrise;
	const float elapsedHours = time >= sunrise ? time - sunrise : time - sunrise + HoursPerDay;
	const float phase = elapsedHours < daylightHours ? elapsedHours / daylightHours :
	                                                   1.0f + (elapsedHours - daylightHours) / (HoursPerDay - daylightHours);
	const float phi = DirectX::XM_PI * phase;
	float sinPhi, cosPhi;
	DirectX::XMScalarSinCosEst(&sinPhi, &cosPhi, phi);

	float tiltRadians = DirectX::XMConvertToRadians(sunAngle);
	float cosTilt, sinTilt;
	DirectX::XMScalarSinCosEst(&sinTilt, &cosTilt, tiltRadians);

	outDir = { cosPhi, -sinPhi * cosTilt, sinPhi * sinTilt };

	if (const float length = outDir.Unitize(); length < FLT_EPSILON)
		outDir = { 0.0f, 0.0f, 1.0f };

	const float elevationRatio = std::max(sinPhi, 0.0f);
	outDist = std::lerp(SunHorizonDistance, SunPeakDistance, elevationRatio);
}

inline void SkySync::SetSunPosition(const RE::Sun* sun, const RE::NiPoint3& dir, const float distance)
{
	const auto position = dir * distance;
	sun->root->local.translate = position;
	sun->sunGlareNode->local.translate = position;
	*gSunPosition = position;
}

void SkySync::ShadowFader::Reset()
{
	lightWeights = float3{ 1.0f, 0.0f, 0.0f };
	startLightWeights = lightWeights;
	target = Caster::Sun;
	previousTarget = Caster::Sun;
	fadeTimer = 0.0f;
	immediateTransitionRemaining = 0.0f;
	transitioning = false;
}

void SkySync::ShadowFader::Update(const RE::Sky* sky, RE::NiPoint3 dirs[], float intensities[], float fadeDuration, float fadeAdvance, bool a_immediateTransition)
{
	auto isValidDir = [](const RE::NiPoint3& d) { return d.x != 0.0f || d.y != 0.0f || d.z != 0.0f; };

	if (fadeDuration > 0.0f && a_immediateTransition)
		immediateTransitionRemaining = fadeDuration;
	else
		immediateTransitionRemaining = std::max(immediateTransitionRemaining - fadeAdvance, 0.0f);

	Caster best;

	if (globals::features::skySync.sunBelowHorizon) {
		bool masserValid = isValidDir(dirs[static_cast<int>(Caster::Masser)]) && intensities[static_cast<int>(Caster::Masser)] > 0.0f;
		bool secundaValid = isValidDir(dirs[static_cast<int>(Caster::Secunda)]) && intensities[static_cast<int>(Caster::Secunda)] > 0.0f;

		best = SelectMoonCaster(masserValid ? intensities[static_cast<int>(Caster::Masser)] : 0.0f,
			secundaValid ? intensities[static_cast<int>(Caster::Secunda)] : 0.0f, target);
	} else {
		best = Caster::Sun;
	}

	auto casterDir = [&](Caster c) {
		return dirs[static_cast<int>(c == Caster::None ? Caster::Sun : c)];
	};
	const RE::NiPoint3 celestialTargetDir = casterDir(best);

	// If best source changed, begin a new transition
	if (best != target) {
		previousTarget = target;
		target = best;
		startDir = currentDir;
		startCelestialDir = celestialDir;
		startLightWeights = lightWeights;
		fadeTimer = 0.0f;
		transitioning = true;
	}

	const RE::NiPoint3 targetDir = casterDir(target);
	const float3 targetLightWeights = {
		target == Caster::Sun ? 1.0f : 0.0f,
		target == Caster::Masser ? 1.0f : 0.0f,
		target == Caster::Secunda ? 1.0f : 0.0f
	};

	if (!transitioning) {
		currentDir = targetDir;
		celestialDir = celestialTargetDir;
		lightWeights = targetLightWeights;
		if (target != Caster::None)
			immediateTransitionRemaining = 0.0f;
		SetLighting(sky, currentDir);
		return;
	}

	const float effectiveFadeAdvance = immediateTransitionRemaining > 0.0f ? fadeDuration : fadeAdvance;
	fadeTimer = std::min(fadeTimer + effectiveFadeAdvance, fadeDuration);
	const float t = fadeDuration > 0.0f ? fadeTimer / fadeDuration : 1.0f;
	lightWeights = float3{
		std::lerp(startLightWeights.x, targetLightWeights.x, t),
		std::lerp(startLightWeights.y, targetLightWeights.y, t),
		std::lerp(startLightWeights.z, targetLightWeights.z, t)
	};

	currentDir = {
		std::lerp(startDir.x, targetDir.x, t),
		std::lerp(startDir.y, targetDir.y, t),
		std::lerp(startDir.z, targetDir.z, t)
	};
	if (currentDir.Unitize() <= FLT_EPSILON)
		currentDir = { 0.0f, 0.0f, 1.0f };
	celestialDir = {
		std::lerp(startCelestialDir.x, celestialTargetDir.x, t),
		std::lerp(startCelestialDir.y, celestialTargetDir.y, t),
		std::lerp(startCelestialDir.z, celestialTargetDir.z, t)
	};
	if (celestialDir.Unitize() <= FLT_EPSILON)
		celestialDir = { 0.0f, 0.0f, 1.0f };

	if (t >= 1.0f) {
		currentDir = targetDir;
		celestialDir = celestialTargetDir;
		transitioning = false;
	}

	if (target != Caster::None && !transitioning)
		immediateTransitionRemaining = 0.0f;
	SetLighting(sky, currentDir);
}

void SkySync::ShadowFader::SetLighting(const RE::Sky* sky, RE::NiPoint3 dir)
{
	ClampDirection(dir);

	RE::NiMatrix3& m = sky->sun->light->local.rotate;
	m.entry[0][0] = -dir.x;
	m.entry[1][0] = -dir.y;
	m.entry[2][0] = -dir.z;

	RE::NiUpdateData updateData;
	sky->sun->light->Update(updateData);

	const auto* scene = globals::game::smState->shadowSceneNode[0];
	auto* shadowLight = scene ? scene->GetRuntimeData().sunShadowDirLight : nullptr;
	if (shadowLight && shadowLight->light.get() == sky->sun->light.get()) {
		auto& shadowData = shadowLight->GetShadowDirectionalLightRuntimeData();
		// The native direction cache would otherwise delay the already-smoothed celestial light.
		shadowData.sunVector = sky->sun->light->GetWorldDirection();
		shadowData.lastSunVector = shadowData.sunVector;
	}
}

inline void SkySync::ShadowFader::SetDirection(RE::NiPoint3& dir, float headingRadians, float elevRadians)
{
	float sinElev, cosElev, sinHeading, cosHeading;
	DirectX::XMScalarSinCosEst(&sinElev, &cosElev, elevRadians);
	DirectX::XMScalarSinCosEst(&sinHeading, &cosHeading, headingRadians);

	dir.x = cosElev * cosHeading;
	dir.y = cosElev * sinHeading;
	dir.z = sinElev;
}

inline void SkySync::ShadowFader::SetElevation(RE::NiPoint3& dir, float elevRadians)
{
	SetDirection(dir, std::atan2(dir.y, dir.x), elevRadians);
}

inline void SkySync::ShadowFader::ClampDirection(RE::NiPoint3& dir)
{
	const float minDegrees = globals::features::skySync.settings.MinShadowElevation;
	if (minDegrees <= 0.0f)
		return;
	const float minElev = DirectX::XMConvertToRadians(minDegrees);
	const float elev = DirectX::XMScalarASinEst(dir.z);
	if (elev >= minElev)
		return;

	SetElevation(dir, minElev);
}

#undef I18N_KEY_PREFIX

SkySync::Caster SkySync::SelectMoonCaster(float masser, float secunda, Caster previous)
{
	if (masser <= 0.0f && secunda <= 0.0f)
		return Caster::None;
	if (masser == secunda && (previous == Caster::Masser || previous == Caster::Secunda))
		return previous;
	return secunda > masser ? Caster::Secunda : Caster::Masser;
}

RE::Moon* SkySync::GetVisibleMoonLightSource(const RE::Sky* sky) const
{
	if (!sky)
		return nullptr;

	auto visibility = [](const RE::Moon* moon) {
		if (!Util::Moon::IsVisible(moon) || !moon->moonMesh)
			return 0.0f;
		const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().shaderProperty.get());
		return prop ? prop->kBlendColor.alpha : 0.0f;
	};

	const float masser = visibility(sky->masser);
	const float secunda = visibility(sky->secunda);

	switch (static_cast<MoonLightSource>(settings.MoonLightSource)) {
	case MoonLightSource::Masser:
		return masser > 0.0f ? sky->masser : nullptr;
	case MoonLightSource::Secunda:
		return secunda > 0.0f ? sky->secunda : nullptr;
	default:
		const auto caster = SelectMoonCaster(masser, secunda, shadowFader.target);
		return caster == Caster::None ? nullptr : caster == Caster::Secunda ? sky->secunda :
		                                                                      sky->masser;
	}
}
