#include "WeaponThrowVRWind.h"

#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/Wind.h"
#include "Features/Wind/WindMath.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "ProjectileHookDispatcher.h"
#include "Utils/MathUtils.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <vector>

namespace
{
	constexpr std::string_view kPluginName = "WeaponThrowVR.esp";
	constexpr float kMinimumStrength = 0.0f;
	constexpr float kMaximumStrength = 5.0f;
	constexpr float kMinimumSpeed = 0.0f;
	constexpr float kMaximumSpeed = 6000.0f;
	constexpr float kMinimumDistance = 32.0f;
	constexpr float kMaximumDistance = 3000.0f;
	constexpr float kProjectileRetentionTime = 0.5f;
	constexpr std::size_t kMaximumObservedProjectiles = WindField::kTransientImpulseCapacity;
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WeaponThrowVRWind::Settings,
	enabled,
	strength,
	minimumSpeed,
	fullStrengthSpeed,
	radius,
	bowLength,
	wakeLength,
	launchStrength,
	impactStrength,
	decayTime)

WeaponThrowVRWind::~WeaponThrowVRWind()
{
	if (subscribed)
		ProjectileHookDispatcher::GetSingleton().RemoveObservers(this);
}

std::string WeaponThrowVRWind::GetDisplayName() const
{
	return T("feature.wind.wind_effect.weapon_throw_vr.name", "Weapon Throw VR");
}

void WeaponThrowVRWind::DrawSettings()
{
	if (!pluginDetected)
		ImGui::TextDisabled("%s", T("feature.wind.wind_effect.weapon_throw_vr.not_detected",
									  "WeaponThrowVR.esp not detected"));
	if (ImGui::Checkbox(T("feature.wind.wind_effect.weapon_throw_vr.enabled", "Enable Thrown Weapon Wind"),
			&settings.enabled) &&
		!settings.enabled) {
		Reset();
	}

	ImGui::BeginDisabled(!settings.enabled || !pluginDetected);
	ImGui::SliderFloat(T("feature.wind.wind_effect.weapon_throw_vr.strength", "Master Strength"),
		&settings.strength, kMinimumStrength, kMaximumStrength, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (ImGui::SliderFloat(T("feature.wind.wind_effect.weapon_throw_vr.minimum_speed", "Minimum Speed"),
			&settings.minimumSpeed, kMinimumSpeed, kMaximumSpeed, "%.0f units/s", ImGuiSliderFlags_AlwaysClamp))
		settings.fullStrengthSpeed = std::max(settings.fullStrengthSpeed, settings.minimumSpeed);
	ImGui::SliderFloat(T("feature.wind.wind_effect.weapon_throw_vr.full_speed", "Full-Strength Speed"),
		&settings.fullStrengthSpeed, settings.minimumSpeed, kMaximumSpeed, "%.0f units/s",
		ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.weapon_throw_vr.radius", "Crosswind Radius"),
		&settings.radius, kMinimumDistance, kMaximumDistance, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.weapon_throw_vr.bow", "Bow Length"),
		&settings.bowLength, kMinimumDistance, kMaximumDistance, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.weapon_throw_vr.wake", "Wake Length"),
		&settings.wakeLength, kMinimumDistance, kMaximumDistance, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.weapon_throw_vr.launch", "Launch Pulse"),
		&settings.launchStrength, kMinimumStrength, kMaximumStrength, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.weapon_throw_vr.impact", "Impact Pulse"),
		&settings.impactStrength, kMinimumStrength, kMaximumStrength, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.weapon_throw_vr.decay", "Pulse Falloff"),
		&settings.decayTime, 0.0f, WindField::kTransientImpulseMaximumDecayTime, "%.2f s",
		ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
}

void WeaponThrowVRWind::LoadSettings(const nlohmann::json& a_json)
{
	settings = a_json;
	SanitizeSettings();
}

void WeaponThrowVRWind::SaveSettings(nlohmann::json& a_json) const
{
	a_json = settings;
}

void WeaponThrowVRWind::RestoreDefaultSettings()
{
	settings = {};
}

void WeaponThrowVRWind::DataLoaded()
{
	if (recordsLoaded)
		return;
	recordsLoaded = true;
	if (!globals::game::isVR)
		return;

	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler)
		return;
	const RE::TESFile* plugin = dataHandler->LookupLoadedModByName(kPluginName);
	if (!plugin)
		plugin = dataHandler->LookupLoadedLightModByName(kPluginName);
	if (!plugin) {
		logger::info("Weapon Throw VR wind disabled: {} is not loaded", kPluginName);
		return;
	}

	std::vector<const RE::BGSProjectile*> projectiles;
	for (const auto* projectile : dataHandler->GetFormArray<RE::BGSProjectile>()) {
		if (!projectile || projectile->GetFile(0) != plugin)
			continue;
		projectileForms.emplace(projectile->GetFormID());
		projectiles.push_back(projectile);
	}
	if (projectileForms.empty()) {
		logger::warn("Weapon Throw VR wind found {} but no owned projectile records", kPluginName);
		return;
	}

	auto& dispatcher = ProjectileHookDispatcher::GetSingleton();
	dispatcher.AddImpactObserver(
		this, ProjectileHookDispatcher::ImpactObserverTrampoline<WeaponThrowVRWind, &WeaponThrowVRWind::ObserveImpact>);
	dispatcher.AddMotionObserver(this, ObserveMotionCallback);
	subscribed = true;
	for (const auto* projectile : projectiles) {
		dispatcher.ObserveProjectileType(*projectile, ProjectileHookDispatcher::Event::Impact);
		dispatcher.ObserveProjectileType(*projectile, ProjectileHookDispatcher::Event::Motion);
	}
	pluginDetected = true;
	logger::info("Registered Weapon Throw VR wind for {} projectile records", projectileForms.size());
}

void WeaponThrowVRWind::Update(float a_frameTime)
{
	std::unordered_map<std::uintptr_t, MotionObservation> motion;
	std::unordered_map<std::uintptr_t, ImpactObservation> impacts;
	{
		std::lock_guard lock(pendingMutex);
		motion.swap(pendingMotion);
		impacts.swap(pendingImpacts);
	}

	const float frameTime = std::isfinite(a_frameTime) ? std::max(a_frameTime, 0.0f) : 0.0f;
	for (auto& entry : projectileAges)
		entry.second += frameTime;
	for (auto& entry : impactAges)
		entry.second += frameTime;
	std::erase_if(projectileAges, [](const auto& a_entry) {
		return a_entry.second > kProjectileRetentionTime;
	});
	std::erase_if(impactAges, [](const auto& a_entry) {
		return a_entry.second > kProjectileRetentionTime;
	});

	if (!settings.enabled || settings.strength <= 0.0f) {
		globals::features::wind.SetAttachedTransientWindSources(
			Wind::TransientWindSourceOwner::WeaponThrowVR, {});
		return;
	}

	std::vector<Wind::TransientWindSourceSubmission> attachedSources;
	attachedSources.reserve(motion.size());
	for (const auto& [identity, observation] : motion) {
		const float speed = observation.velocity.Length();
		if (!std::isfinite(speed) || speed < settings.minimumSpeed)
			continue;
		const float3 direction = Util::SafeNormalize(
			{ observation.velocity.x, observation.velocity.y, observation.velocity.z });
		if (direction.x == 0.0f && direction.y == 0.0f && direction.z == 0.0f)
			continue;

		const bool firstObservation = !projectileAges.contains(identity);
		projectileAges.insert_or_assign(identity, 0.0f);
		const float speedRange = std::max(settings.fullStrengthSpeed - settings.minimumSpeed, 1.0f);
		const float speedScale = std::clamp((speed - settings.minimumSpeed) / speedRange, 0.0f, 1.0f);
		const float strength = settings.strength * speedScale;
		const float3 position{ observation.position.x, observation.position.y, observation.position.z };
		attachedSources.push_back({ WindField::MakeOrientedFlow(position, direction, strength,
										settings.radius, settings.bowLength, settings.wakeLength),
			Wind::TransientWindSourcePriority::Flight });

		if (firstObservation && settings.launchStrength > 0.0f) {
			const auto launch = WindField::MakeDirectionalWave(position, direction,
				strength * settings.launchStrength, settings.wakeLength,
				std::clamp(settings.radius * 0.6f, 48.0f, 500.0f),
				std::clamp(speed, 800.0f, 5000.0f), 0.8f, settings.decayTime);
			globals::features::wind.QueueTransientWindSource(launch,
				Wind::TransientWindSourceOwner::WeaponThrowVR,
				Wind::TransientWindSourcePriority::Flight);
		}
	}
	globals::features::wind.SetAttachedTransientWindSources(
		Wind::TransientWindSourceOwner::WeaponThrowVR, attachedSources);

	for (const auto& [identity, impact] : impacts) {
		if (settings.impactStrength <= 0.0f)
			break;
		if (impactAges.contains(identity))
			continue;
		impactAges.emplace(identity, 0.0f);
		float3 fallbackDirection = Util::SafeNormalize(
			{ impact.velocity.x, impact.velocity.y, impact.velocity.z });
		if (fallbackDirection.x == 0.0f && fallbackDirection.y == 0.0f && fallbackDirection.z == 0.0f)
			fallbackDirection = float3(1.0f, 0.0f, 0.0f);
		const auto source = WindField::MakeRadialWave(
			{ impact.position.x, impact.position.y, impact.position.z }, fallbackDirection,
			settings.strength * settings.impactStrength, settings.radius * 2.0f,
			std::clamp(settings.radius * 0.5f, 48.0f, 500.0f), 1800.0f, settings.decayTime);
		globals::features::wind.QueueTransientWindSource(source,
			Wind::TransientWindSourceOwner::WeaponThrowVR,
			Wind::TransientWindSourcePriority::Impact,
			Wind::TransientWindPhysics::Native);
	}
}

void WeaponThrowVRWind::Reset()
{
	{
		std::lock_guard lock(pendingMutex);
		pendingMotion.clear();
		pendingImpacts.clear();
	}
	projectileAges.clear();
	impactAges.clear();
	globals::features::wind.ClearTransientWindSources(Wind::TransientWindSourceOwner::WeaponThrowVR);
}

void WeaponThrowVRWind::ObserveMotionCallback(void* a_owner, RE::Projectile& a_projectile, float)
{
	static_cast<WeaponThrowVRWind*>(a_owner)->ObserveMotion(a_projectile);
}

void WeaponThrowVRWind::ObserveMotion(RE::Projectile& a_projectile)
{
	if (!IsWeaponThrowProjectile(a_projectile))
		return;
	const auto position = a_projectile.GetPosition();
	const auto& runtimeData = a_projectile.GetProjectileRuntimeData();
	const auto velocity = runtimeData.linearVelocity.Length() > 1e-4f ?
	                          runtimeData.linearVelocity :
	                          runtimeData.velocity;
	if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
		!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z))
		return;

	const auto identity = reinterpret_cast<std::uintptr_t>(std::addressof(a_projectile));
	std::lock_guard lock(pendingMutex);
	if (pendingMotion.size() < kMaximumObservedProjectiles || pendingMotion.contains(identity))
		pendingMotion.insert_or_assign(identity, MotionObservation{ position, velocity });
}

void WeaponThrowVRWind::ObserveImpact(RE::Projectile& a_projectile,
	const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity)
{
	if (!IsWeaponThrowProjectile(a_projectile) ||
		!std::isfinite(a_position.x) || !std::isfinite(a_position.y) || !std::isfinite(a_position.z))
		return;
	const auto identity = reinterpret_cast<std::uintptr_t>(std::addressof(a_projectile));
	std::lock_guard lock(pendingMutex);
	if (pendingImpacts.size() < kMaximumObservedProjectiles || pendingImpacts.contains(identity))
		pendingImpacts.insert_or_assign(identity, ImpactObservation{ a_position, a_velocity });
}

bool WeaponThrowVRWind::IsWeaponThrowProjectile(const RE::Projectile& a_projectile) const
{
	const auto* projectile = a_projectile.GetProjectileBase();
	return projectile && projectileForms.contains(projectile->GetFormID());
}

void WeaponThrowVRWind::SanitizeSettings()
{
	const Settings defaults{};
	settings.strength = Util::ClampFinite(
		settings.strength, kMinimumStrength, kMaximumStrength, defaults.strength);
	settings.minimumSpeed = Util::ClampFinite(
		settings.minimumSpeed, kMinimumSpeed, kMaximumSpeed, defaults.minimumSpeed);
	settings.fullStrengthSpeed = Util::ClampFinite(
		settings.fullStrengthSpeed, settings.minimumSpeed, kMaximumSpeed, defaults.fullStrengthSpeed);
	settings.radius = Util::ClampFinite(
		settings.radius, kMinimumDistance, kMaximumDistance, defaults.radius);
	settings.bowLength = Util::ClampFinite(
		settings.bowLength, kMinimumDistance, kMaximumDistance, defaults.bowLength);
	settings.wakeLength = Util::ClampFinite(
		settings.wakeLength, kMinimumDistance, kMaximumDistance, defaults.wakeLength);
	settings.launchStrength = Util::ClampFinite(
		settings.launchStrength, kMinimumStrength, kMaximumStrength, defaults.launchStrength);
	settings.impactStrength = Util::ClampFinite(
		settings.impactStrength, kMinimumStrength, kMaximumStrength, defaults.impactStrength);
	settings.decayTime = Util::ClampFinite(settings.decayTime,
		0.0f, WindField::kTransientImpulseMaximumDecayTime, defaults.decayTime);
}
