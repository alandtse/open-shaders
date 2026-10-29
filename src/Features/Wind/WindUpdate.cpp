#include "Wind.h"

#include "Features/Wind/Trees/TreeWindPatcher.h"

WindPermutationContribution Wind::GetPermutationContribution() const
{
	const TreeWindPatcher::Sensitivities treeDefaults{};
	return {
		settings.trunkWindIntensityOverride,
		loaded && provider && settings.overrideTrunkWindIntensity,
		loaded && provider && settings.enableGrassWindSpringBend,
		treeDefaults.transientWindInfluence,
		treeDefaults.leafTransientWindInfluence,
		treeDefaults.leafTransientFlutterMaximum,
		treeDefaults.transientMaximumBendMultiplier,
		settings.trunkWindBendSensitivity,
		settings.treeLeafBaseWindFlutterGain,
		static_cast<uint32_t>(loaded && provider && settings.enableAmbientGrassWind),
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
