#include "Features/Skin.h"
#include "Globals.h"
#include "State.h"
#include "Utils/MathUtils.h"
#include <cmath>

// By PO3
// https://github.com/powerof3/Splashes-of-Skyrim/blob/master/src/Manager.cpp
float Skin::GetWaterHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos)
{
	float waterHeight = -RE::NI_INFINITY;

	if (const auto waterManager = RE::TESWaterSystem::GetSingleton()) {
		waterHeight = a_ref->GetWaterHeight();

		if (waterHeight != -RE::NI_INFINITY) {
			return waterHeight;
		}

		const auto get_nearest_water_object_height = [&]() {
			for (const auto& waterObject : waterManager->waterObjects) {
				if (waterObject) {
					for (const auto& bound : waterObject->multiBounds) {
						if (bound) {
							if (auto size{ bound->size }; size.z <= 10.0f) {  //avoid sloped water
								auto center{ bound->center };
								const auto boundMin = center - size;
								const auto boundMax = center + size;
								if (!(a_pos.x < boundMin.x || a_pos.x > boundMax.x || a_pos.y < boundMin.y || a_pos.y > boundMax.y)) {
									return center.z;
								}
							}
						}
					}
				}
			}

			return -RE::NI_INFINITY;
		};

		waterHeight = get_nearest_water_object_height();
	}

	return waterHeight;
}

float4 Skin::GetWetness(RE::BSGeometry* geometry)
{
	float4 wetness = float4(0.0f, 0.0f, 0.0f, 0.0f);
	auto userData = geometry->GetUserData();
	if (userData && userData->IsActor()) {
		auto actor = static_cast<RE::Character*>(userData);
		const uint32_t actorFormID = userData->formID;
		const uint currentFrame = globals::state->frameCount;

		if (actorWetnessMap.size() > 1024) {
			std::erase_if(actorWetnessMap, [currentFrame](const auto& entry) {
				return currentFrame - entry.second.frameCount > 1;
			});
		}

		auto [it, inserted] = actorWetnessMap.try_emplace(actorFormID);
		auto& cached = it->second;
		if (!inserted && cached.frameCount == currentFrame) {
			return cached.wetness;
		}
		cached.frameCount = currentFrame;

		const float positionZ = actor->GetPositionZ();
		wetness.z = positionZ;
		const float stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		const float permanentStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		const float temporaryStamina = actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kStamina);
		const float maxStamina = std::max(permanentStamina + temporaryStamina, 1.0f);
		const float staminaPercentage = actor->IsDead() ? 1.0f : (stamina / maxStamina);
		const float sweatStart = std::max(settings.StartSweat, settings.FullSweat);
		const float sweatFull = std::min(settings.StartSweat, settings.FullSweat);
		const float sweatRange = sweatStart - sweatFull;
		wetness.x = (std::abs(sweatRange) < Util::kNormalizedRangeEpsilon) ? 0.0f :
		            (staminaPercentage >= sweatStart)                      ? 0.0f :
		            (staminaPercentage <= sweatFull)                       ? 1.0f :
		                                                                     (sweatStart - staminaPercentage) / sweatRange;
		if (actor->IsInWater()) {
			wetness.y = 2.0f;
			const float waterHeight = GetWaterHeight(userData, actor->GetPosition());
			wetness.w = std::max(cached.wetness.w, waterHeight - positionZ);
		} else {
			wetness.y = 0.0f;
			wetness.w = 0.0f;
		}

		if (inserted) {
			cached.wetness = wetness;
		} else {
			const float fadeTime = std::max(settings.WetFadeTime, 0.001f);
			if (cached.wetness.x < wetness.x) {
				cached.wetness.x = wetness.x;
			} else if (cached.wetness.x > wetness.x) {
				cached.wetness.x -= *globals::game::deltaTime / fadeTime;
				cached.wetness.x = std::max(cached.wetness.x, 0.0f);
			}
			wetness.x = cached.wetness.x;

			constexpr float kWaterWetnessMagnitude = 2.0f;
			if (cached.wetness.y < wetness.y) {
				cached.wetness.y = wetness.y;
			} else if (cached.wetness.y > wetness.y) {
				cached.wetness.y -= kWaterWetnessMagnitude * *globals::game::deltaTime / fadeTime;
				cached.wetness.y = std::max(cached.wetness.y, 0.0f);
				wetness.y = cached.wetness.y;
				if (wetness.y == 0.0f) {
					wetness.w = 0.0f;
				} else {
					// Hold the last known water depth while drying instead of snapping to 0 the instant water is left.
					wetness.w = cached.wetness.w;
				}
			}
			cached.wetness = wetness;
		}
	}
	return wetness;
}
