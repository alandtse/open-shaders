#include "Wind.h"

#include <algorithm>
#include <cmath>

#include "Features/Wind/Trees/TreeWindPatcher.h"
#include "Globals.h"

namespace
{
	bool IsFiniteSource(const WindField::TransientWindSource& a_source)
	{
		return std::isfinite(a_source.origin.x) && std::isfinite(a_source.origin.y) &&
		       std::isfinite(a_source.origin.z) && std::isfinite(a_source.direction.x) &&
		       std::isfinite(a_source.direction.y) && std::isfinite(a_source.direction.z) &&
		       std::isfinite(a_source.wavefrontDistance) && std::isfinite(a_source.strength) &&
		       std::isfinite(a_source.maxDistance) && std::isfinite(a_source.waveHalfWidth) &&
		       std::isfinite(a_source.propagationSpeed) && std::isfinite(a_source.coneSpreadTangent) &&
		       std::isfinite(a_source.decayTime) && std::isfinite(a_source.collisionRadius) &&
		       std::isfinite(a_source.sideSpreadTangent) && std::isfinite(a_source.sideStrength) &&
		       std::isfinite(a_source.verticalScale) && std::isfinite(a_source.falloffWidth);
	}

	struct WindSelection
	{
		float3 direction;
		float speed;
	};

	float EvaluateWindResponse(float a_speed, const std::array<float, 3>& a_response)
	{
		const float speed = std::max(a_speed, 0.0f);
		if (speed <= WindSettingsLimits::kWindResponseSpeeds[0])
			return std::lerp(0.0f, a_response[0], speed / WindSettingsLimits::kWindResponseSpeeds[0]);
		if (speed <= WindSettingsLimits::kWindResponseSpeeds[1])
			return std::lerp(a_response[0], a_response[1],
				(speed - WindSettingsLimits::kWindResponseSpeeds[0]) /
					(WindSettingsLimits::kWindResponseSpeeds[1] - WindSettingsLimits::kWindResponseSpeeds[0]));
		if (speed <= WindSettingsLimits::kWindResponseSpeeds[2])
			return std::lerp(a_response[1], a_response[2],
				(speed - WindSettingsLimits::kWindResponseSpeeds[1]) /
					(WindSettingsLimits::kWindResponseSpeeds[2] - WindSettingsLimits::kWindResponseSpeeds[1]));
		return a_response[2] * speed;
	}

	WindSelection SelectWind(const Wind& a_wind, const float3& a_ambientVelocity,
		const float3& a_fallbackDirection)
	{
		const float ambientSpeed = std::sqrt(
			a_ambientVelocity.x * a_ambientVelocity.x +
			a_ambientVelocity.y * a_ambientVelocity.y +
			a_ambientVelocity.z * a_ambientVelocity.z);
		const float speed = !a_wind.loaded || a_wind.ShouldUseRealWindSpeed() ?
		                        (std::isfinite(ambientSpeed) ? std::max(ambientSpeed, 0.0f) : 0.0f) :
		                        std::max(a_wind.GetEffectiveWindOverrideSpeed(), 0.0f);
		const bool useRealDirection = !a_wind.loaded || a_wind.runtimeState.windFieldUseRealDirection;
		const float directionLength = std::hypot(a_ambientVelocity.x, a_ambientVelocity.y);
		float3 direction = a_fallbackDirection;
		if (useRealDirection && std::isfinite(directionLength) && directionLength > 0.0001f) {
			direction = float3{
				a_ambientVelocity.x / directionLength,
				a_ambientVelocity.y / directionLength,
				0.0f
			};
		} else if (!useRealDirection) {
			const float directionRadians = DirectX::XMConvertToRadians(a_wind.runtimeState.windFieldAppliedDirectionDegrees);
			direction = float3{ std::cos(directionRadians), std::sin(directionRadians), 0.0f };
		}
		return { direction, speed };
	}
}

void Wind::Reset()
{
	windFieldTuning.gustAmplitude = GetEffectiveWindGustAmplitude();
	windFieldTuning.gustScale = GetEffectiveWindGustScale();
	windFieldTuning.frontAspectRatio =
		settings.windFieldGustCrosswindScale / windFieldTuning.gustScale;
	windFieldTuning.gustAdvectionMultiplier = GetEffectiveWindGustAdvectionMultiplier();
	const bool gamePaused = globals::game::ui && globals::game::ui->GameIsPaused();
	const float frameTime = gamePaused ? 0.0f : std::max(RE::GetSecondsSinceLastFrame(), 0.0f);
	UpdateWindEffects(frameTime);
	AdvanceWindHistory(frameTime);
	UpdateWeatherWind();
	const float3 fallbackDirection =
		windFieldHasPreviousSample ? windFieldCurrent.direction : float3{ 1.0f, 0.0f, 0.0f };
	const auto selectedWind = SelectWind(*this, ambientWindVelocity, fallbackDirection);
	UpdateWindField(selectedWind.direction, selectedWind.speed, frameTime);
}

void Wind::AdvanceWindHistory(float a_frameTime)
{
	windFieldFrameTime = a_frameTime;
	UpdateTransientWindImpulses(a_frameTime);
	previousWindFieldSelectedVelocity = windFieldSelectedVelocity;
	previousWindFieldGustTravelDistance = windFieldGustTravelDistance;
	previousWindFieldCurrent = windFieldCurrent;
	previousWindFieldTransition = windFieldTransition;
	previousWindFieldTransitionBlend = windFieldTransitionBlend;
}

void Wind::UpdateWeatherWind()
{
	ambientWindVelocity = float3{};
	const auto* sky = globals::game::sky;
	const float activeWindIntensity = sky && std::isfinite(sky->windSpeed) ?
	                                      std::clamp(sky->windSpeed, 0.0f, 1.0f) :
	                                      0.0f;
	float2 weatherDirection{};
	if (sky && std::isfinite(sky->windAngle)) {
		weatherDirection = float2{ std::cos(sky->windAngle), std::sin(sky->windAngle) };
	}
	const float directionLength = std::hypot(weatherDirection.x, weatherDirection.y);
	if (std::isfinite(directionLength) && directionLength > 0.0001f && std::isfinite(activeWindIntensity)) {
		weatherDirection.x /= directionLength;
		weatherDirection.y /= directionLength;
		ambientWindVelocity = float3{ weatherDirection.x * activeWindIntensity, weatherDirection.y * activeWindIntensity, 0.0f };
	}
}

void Wind::UpdateWindField(const float3& a_direction, float a_speed, float a_frameTime)
{
	const float gustAdvectionSpeed = EvaluateWindResponse(a_speed, settings.windFieldGustAdvectionResponse) *
	                                 windFieldTuning.gustAdvectionBaseSpeed *
	                                 windFieldTuning.gustAdvectionMultiplier;
	windFieldAdvectionSpeed = std::isfinite(gustAdvectionSpeed) ? std::max(gustAdvectionSpeed, 0.0f) : 0.0f;
	windFieldTravelDelta = std::isfinite(a_frameTime) ? windFieldAdvectionSpeed * std::max(a_frameTime, 0.0f) : 0.0f;
	if (!windFieldHasPreviousSample) {
		windFieldCurrent = WindField::CreateField(a_direction, a_speed);
		previousWindFieldCurrent = windFieldCurrent;
		windFieldTransition = windFieldCurrent;
		previousWindFieldTransition = windFieldCurrent;
		windFieldTransitionBlend = 1.0f;
		previousWindFieldTransitionBlend = 1.0f;
		windFieldHasPreviousSample = true;
	} else {
		constexpr float kOneDegreeDirectionChangeCosine = 0.9998477f;
		const float directionDot = windFieldCurrent.direction.x * a_direction.x +
		                           windFieldCurrent.direction.y * a_direction.y;
		if (!windFieldTransitionActive && directionDot < kOneDegreeDirectionChangeCosine) {
			windFieldTransition = windFieldCurrent;
			previousWindFieldTransition = previousWindFieldCurrent;
			windFieldCurrent = WindField::CreateField(a_direction, a_speed);
			windFieldTransitionElapsed = 0.0f;
			windFieldTransitionBlend = 0.0f;
			windFieldTransitionActive = true;
		}
	}
	WindField::SetFieldSpeed(windFieldCurrent, a_speed);
	WindField::AdvanceField(windFieldCurrent, windFieldTravelDelta);
	if (windFieldTransitionActive) {
		WindField::SetFieldSpeed(windFieldTransition, a_speed);
		WindField::AdvanceField(windFieldTransition, windFieldTravelDelta);
		windFieldTransitionElapsed += a_frameTime;
		const float configuredTransitionDuration = settings.windFieldDirectionTransitionDuration;
		const float transitionDuration = std::isfinite(configuredTransitionDuration) ?
		                                     std::clamp(configuredTransitionDuration, 0.0f, 30.0f) :
		                                     1.0f;
		windFieldTransitionBlend = transitionDuration > 0.0f ?
		                               std::clamp(windFieldTransitionElapsed / transitionDuration, 0.0f, 1.0f) :
		                               1.0f;
		if (windFieldTransitionBlend >= 1.0f)
			windFieldTransitionActive = false;
	} else {
		windFieldTransitionBlend = 1.0f;
	}
	windFieldSelectedSpeed = windFieldCurrent.speed;
	windFieldSelectedVelocity = windFieldCurrent.direction * windFieldCurrent.speed;
	windFieldAmbientSpeed = windFieldCurrent.speed;
	windFieldGustTravelDistance = windFieldCurrent.travelDistance;
	previousWindFieldSelectedVelocity = previousWindFieldCurrent.direction * previousWindFieldCurrent.speed;
	previousWindFieldGustTravelDistance = previousWindFieldCurrent.travelDistance;
}

WindPermutationContribution Wind::GetPermutationContribution() const
{
	const TreeWindPatcher::Sensitivities treeDefaults{};
	return {
		settings.trunkWindIntensityOverride,
		loaded && settings.overrideTrunkWindIntensity,
		loaded && settings.enableGrassWindSpringBend,
		treeDefaults.transientWindInfluence,
		treeDefaults.leafTransientWindInfluence,
		treeDefaults.leafTransientFlutterMaximum,
		treeDefaults.transientMaximumBendMultiplier,
		settings.trunkWindBendSensitivity,
		settings.treeLeafBaseWindFlutterGain,
		static_cast<uint32_t>(loaded && settings.enableAmbientGrassWind),
		settings.grassWindSensitivity,
		settings.grassWindBendProfile,
		settings.grassWindCompressionToBend,
		settings.grassWindFlutterStrength,
		settings.grassWindFlutterFrequency
	};
}

WindSharedData Wind::GetSharedWindData() const
{
	WindSharedData data{};
	data.tuning = windFieldTuning;
	const float3 transitionVelocity = windFieldTransition.direction * windFieldTransition.speed;
	const float3 previousTransitionVelocity = previousWindFieldTransition.direction * previousWindFieldTransition.speed;
	const float3 blendedVelocity = transitionVelocity +
	                               (windFieldSelectedVelocity - transitionVelocity) * windFieldTransitionBlend;
	const float3 previousBlendedVelocity =
		previousTransitionVelocity +
		(previousWindFieldSelectedVelocity - previousTransitionVelocity) * previousWindFieldTransitionBlend;
	data.ambient = float4{
		blendedVelocity.x, blendedVelocity.y, blendedVelocity.z,
		windFieldGustTravelDistance
	};
	data.previousAmbient = float4{
		previousBlendedVelocity.x, previousBlendedVelocity.y, previousBlendedVelocity.z,
		previousWindFieldGustTravelDistance
	};
	data.current = windFieldCurrent;
	data.previous = previousWindFieldCurrent;
	data.transition = windFieldTransition;
	data.previousTransition = previousWindFieldTransition;
	data.transitionData = float4{
		windFieldTransitionBlend,
		previousWindFieldTransitionBlend,
		0.0f,
		0.0f
	};
	data.springDebug = float4{
		grassState.springFieldMinimum[0].x,
		grassState.springFieldMinimum[0].y,
		grassState.springFieldAvailable[0] ? grassState.springWorldSizes[0] : 0.0f,
		DirectX::XMConvertToRadians(settings.grassWindMaximumTilt)
	};
	data.activeCounts = {
		activeTransientWindImpulseCount,
		previousActiveTransientWindImpulseCount,
		0u,
		0u
	};
	data.transientImpulses = transientWindImpulses;
	data.previousTransientImpulses = previousTransientWindImpulses;
	return data;
}

WindField::WindSample Wind::SampleWind(const float3& a_worldPosition) const noexcept
{
	auto sample = WindField::SampleField(a_worldPosition, windFieldCurrent, windFieldTuning);
	if (windFieldTransitionBlend < 1.0f) {
		const auto previousSample = WindField::SampleField(a_worldPosition, windFieldTransition, windFieldTuning);
		sample.velocity = previousSample.velocity + (sample.velocity - previousSample.velocity) * windFieldTransitionBlend;
		sample.ambientGust = previousSample.ambientGust +
		                     (sample.ambientGust - previousSample.ambientGust) * windFieldTransitionBlend;
	}
	const auto transientSample = WindField::SampleTransientImpulses(a_worldPosition,
		std::span{ transientWindImpulses }.first(activeTransientWindImpulseCount));
	sample.velocity += transientSample.velocity;
	sample.transientImpulse = std::max(sample.transientImpulse, transientSample.intensity);
	return sample;
}

WindField::WindSample Wind::SampleWind(const float3& a_worldPosition,
	const float3& a_windDirection, float a_windSpeed) const noexcept
{
	const auto field = WindField::CreateField(a_windDirection, a_windSpeed, windFieldGustTravelDistance);
	auto sample = WindField::SampleField(a_worldPosition, field, windFieldTuning);
	const auto transientSample = WindField::SampleTransientImpulses(a_worldPosition,
		std::span{ transientWindImpulses }.first(activeTransientWindImpulseCount));
	sample.velocity += transientSample.velocity;
	sample.transientImpulse = std::max(sample.transientImpulse, transientSample.intensity);
	return sample;
}

void Wind::QueueTransientWindImpulse(const WindField::TransientWindSource& a_impulse)
{
	QueueTransientWindSource(a_impulse, TransientWindSourceOwner::Generic,
		TransientWindSourcePriority::Wingbeat);
}

void Wind::QueueTransientWindSource(const WindField::TransientWindSource& a_source,
	TransientWindSourceOwner a_owner, TransientWindSourcePriority a_priority)
{
	std::lock_guard lock(transientWindImpulseMutex);
	pendingTransientWindSources.push_back({ a_source, a_owner, a_priority, ++transientWindSourceSequence });
	if (pendingTransientWindSources.size() > WindField::kTransientImpulseCapacity) {
		const auto leastImportant = std::ranges::min_element(
			pendingTransientWindSources, [](const auto& left, const auto& right) {
				if (left.priority != right.priority)
					return left.priority < right.priority;
				return left.sequence < right.sequence;
			});
		pendingTransientWindSources.erase(leastImportant);
	}
}

void Wind::SetAttachedTransientWindSources(TransientWindSourceOwner a_owner,
	std::span<const TransientWindSourceSubmission> a_sources)
{
	std::lock_guard lock(transientWindImpulseMutex);
	std::erase_if(attachedTransientWindSources,
		[a_owner](const auto& source) { return source.owner == a_owner; });
	for (const auto& submission : a_sources) {
		attachedTransientWindSources.push_back(
			{ submission.source, a_owner, submission.priority, ++transientWindSourceSequence });
	}
}

void Wind::ClearTransientWindSources(TransientWindSourceOwner a_owner)
{
	std::lock_guard lock(transientWindImpulseMutex);
	auto removeOwned = [a_owner](auto& sources) {
		std::erase_if(sources, [a_owner](const auto& source) { return source.owner == a_owner; });
	};
	removeOwned(activeTransientWindSources);
	removeOwned(pendingTransientWindSources);
	removeOwned(attachedTransientWindSources);

	auto compactSnapshot = [a_owner](auto& sources, auto& owners, uint32_t& count) {
		uint32_t outputIndex = 0;
		for (uint32_t index = 0; index < count; ++index) {
			if (owners[index] == a_owner)
				continue;
			sources[outputIndex] = sources[index];
			owners[outputIndex] = owners[index];
			++outputIndex;
		}
		for (uint32_t index = outputIndex; index < count; ++index) {
			sources[index] = {};
			owners[index] = {};
		}
		count = outputIndex;
	};
	compactSnapshot(transientWindImpulses, transientWindImpulseOwners, activeTransientWindImpulseCount);
	compactSnapshot(previousTransientWindImpulses, previousTransientWindImpulseOwners,
		previousActiveTransientWindImpulseCount);
}

void Wind::ClearTransientWindImpulses()
{
	std::lock_guard lock(transientWindImpulseMutex);
	transientWindImpulses = {};
	previousTransientWindImpulses = {};
	transientWindImpulseOwners = {};
	previousTransientWindImpulseOwners = {};
	activeTransientWindImpulseCount = 0;
	previousActiveTransientWindImpulseCount = 0;
	activeTransientWindSources.clear();
	pendingTransientWindSources.clear();
	attachedTransientWindSources.clear();
}

void Wind::UpdateTransientWindImpulses(float a_frameTime)
{
	std::lock_guard lock(transientWindImpulseMutex);

	previousTransientWindImpulses = transientWindImpulses;
	previousTransientWindImpulseOwners = transientWindImpulseOwners;
	previousActiveTransientWindImpulseCount = activeTransientWindImpulseCount;

	const float frameTime = std::isfinite(a_frameTime) ? std::max(a_frameTime, 0.0f) : 0.0f;
	for (std::size_t index = 0; index < activeTransientWindSources.size();) {
		auto& impulse = activeTransientWindSources[index].source;
		const float travelDelta = std::isfinite(impulse.propagationSpeed) ?
		                              std::max(impulse.propagationSpeed, 0.0f) * frameTime :
		                              0.0f;
		impulse.wavefrontDistance += travelDelta;
		const float decayTime = std::clamp(
			std::isfinite(impulse.decayTime) ? impulse.decayTime : 0.0f, 0.0f,
			WindField::kTransientImpulseMaximumDecayTime);
		const float retentionDistance = impulse.maxDistance + std::abs(impulse.waveHalfWidth) +
		                                std::max(impulse.propagationSpeed, 0.0f) * decayTime;
		if (!std::isfinite(impulse.wavefrontDistance) || impulse.wavefrontDistance >= retentionDistance) {
			activeTransientWindSources.erase(activeTransientWindSources.begin() + index);
		} else {
			++index;
		}
	}

	std::vector<ManagedTransientWindSource> pendingSources;
	pendingSources.swap(pendingTransientWindSources);
	const std::vector<ManagedTransientWindSource>& attachedSources = attachedTransientWindSources;
	for (auto& pendingSource : pendingSources) {
		pendingSource.source.wavefrontDistance = 0.0f;
		activeTransientWindSources.push_back(pendingSource);
	}
	std::ranges::stable_sort(activeTransientWindSources, [](const auto& left, const auto& right) {
		if (left.priority != right.priority)
			return left.priority > right.priority;
		return left.sequence > right.sequence;
	});
	if (activeTransientWindSources.size() > WindField::kTransientImpulseCapacity)
		activeTransientWindSources.resize(WindField::kTransientImpulseCapacity);

	std::vector<ManagedTransientWindSource> composedSources = activeTransientWindSources;
	composedSources.insert(composedSources.end(), attachedSources.begin(), attachedSources.end());
	std::erase_if(composedSources, [](const auto& a_managed) {
		return !IsFiniteSource(a_managed.source);
	});
	std::ranges::stable_sort(composedSources, [](const auto& left, const auto& right) {
		if (left.priority != right.priority)
			return left.priority > right.priority;
		return left.sequence > right.sequence;
	});
	if (composedSources.size() > WindField::kTransientImpulseCapacity)
		composedSources.resize(WindField::kTransientImpulseCapacity);

	transientWindImpulses = {};
	transientWindImpulseOwners = {};
	activeTransientWindImpulseCount = static_cast<uint32_t>(composedSources.size());
	for (uint32_t index = 0; index < activeTransientWindImpulseCount; ++index) {
		transientWindImpulses[index] = composedSources[index].source;
		transientWindImpulseOwners[index] = composedSources[index].owner;
	}
}
