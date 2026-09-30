#ifndef __TREE_WIND_DEPENDENCY_HLSL__
#define __TREE_WIND_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/TreeWindSpring.hlsli"
#include "Common/WindField.hlsli"

namespace TreeWind
{
	namespace Detail
	{
		static const float TRANSIENT_LEAF_FLUTTER_GAIN = 3.0;
		static const float TRANSIENT_LEAF_STRUCTURAL_COUPLING = 0.75;

		float GetLeafFlutterGain()
		{
			return max(Permutation::TreeLeafBaseWindFlutterGain, 0.0) *
			       max(Permutation::TreeLeafModelSensitivity, 0.0);
		}
	}

	namespace Detail
	{
		WindField::TransientImpulseSample UnpackTransientSample(float4 packedSample, bool immediate)
		{
			WindField::TransientImpulseSample sample;
			float2 velocity = immediate ? packedSample.zw : packedSample.xy;
			sample.velocity = float3(velocity, 0.0f);
			sample.intensity = length(velocity);
			return sample;
		}

		WindField::TransientImpulseSample SampleCurrentTransientAt(float3 worldPosition)
		{
			float4 packedSample = 0.0f.xxxx;
			TreeWindSpring::TrySampleCurrentTransient(worldPosition, packedSample);
			return UnpackTransientSample(packedSample, false);
		}

		WindField::TransientImpulseSample SamplePreviousTransientAt(float3 worldPosition)
		{
			float4 packedSample = 0.0f.xxxx;
			TreeWindSpring::TrySamplePreviousTransient(worldPosition, packedSample);
			return UnpackTransientSample(packedSample, false);
		}

		WindField::TransientImpulseSample SampleCurrentImmediateTransientAt(float3 worldPosition)
		{
			float4 packedSample = 0.0f.xxxx;
			TreeWindSpring::TrySampleCurrentTransient(worldPosition, packedSample);
			return UnpackTransientSample(packedSample, true);
		}

		WindField::TransientImpulseSample SamplePreviousImmediateTransientAt(float3 worldPosition)
		{
			float4 packedSample = 0.0f.xxxx;
			TreeWindSpring::TrySamplePreviousTransient(worldPosition, packedSample);
			return UnpackTransientSample(packedSample, true);
		}

		WindField::TransientImpulseSample ResolveTransientSample(
			float treeHeight,
			WindField::TransientImpulseSample baseSample,
			WindField::TransientImpulseSample middleSample,
			WindField::TransientImpulseSample topSample)
		{
			WindField::TransientImpulseSample sample;
			sample.velocity = 0.0.xxx;
			sample.intensity = max(baseSample.intensity,
				max(middleSample.intensity, topSample.intensity));
			if (treeHeight <= EPSILON_WIND_HEIGHT || sample.intensity <= EPSILON_WIND_GEOMETRY)
				return sample;

			float2 combinedVelocity = baseSample.velocity.xy + middleSample.velocity.xy + topSample.velocity.xy;
			float combinedSpeed = length(combinedVelocity);
			float strongestSpeed = max(length(baseSample.velocity.xy),
				max(length(middleSample.velocity.xy), length(topSample.velocity.xy)));
			sample.velocity.xy =
				combinedSpeed > EPSILON_WIND_RESPONSE ? combinedVelocity * (strongestSpeed / combinedSpeed) : 0.0.xx;
			return sample;
		}
	}

	/** @brief Bends the common tree axis at constant arc length and carries detail motion with it. */
	float3 GetWorldDisplacement(float3 restPosition, float3 animatedPosition,
		row_major float3x4 worldMatrix, float2 windVelocity, out float3x3 normalTransform)
	{
		normalTransform = (float3x3)Math::IdentityMatrix;
		float3 root = mul(worldMatrix, float4(Permutation::TreeWindProbeBase.xyz, 1.0)).xyz;
		float3 treeAxis = mul(worldMatrix, float4(
											   Permutation::TreeWindProbeTop.xyz - Permutation::TreeWindProbeBase.xyz, 0.0))
		                      .xyz;
		float treeHeight = length(treeAxis);
		if (Permutation::TreeWindBoundsHeight <= EPSILON_WIND_HEIGHT || treeHeight <= EPSILON_WIND_HEIGHT)
			return 0.0.xxx;
		treeAxis /= treeHeight;

		float upperBendRange = max(Permutation::TreeWindUpperBendRange * 0.01, 0.05);
		float bendLength = treeHeight * upperBendRange;
		float bendStart = treeHeight - bendLength;
		float vertexHeight = dot(restPosition - root, treeAxis);
		float arcLength = clamp(vertexHeight - bendStart, 0.0, bendLength);
		if (arcLength <= 0.0)
			return 0.0.xxx;

		float bendStrength = max(Permutation::TreeWindMaximumDisplacementPercent, 0.0) * 0.01 *
		                     max(Permutation::TrunkWindBendSensitivity, 0.0) *
		                     max(Permutation::TreeBendModelSensitivity, 0.0);
		float3 bendDirection = float3(windVelocity, 0.0);
		bendDirection -= treeAxis * dot(bendDirection, treeAxis);
		float windStrength = length(bendDirection);
		if (windStrength <= EPSILON_WIND_RESPONSE || bendStrength <= 0.0)
			return 0.0.xxx;
		bendDirection /= windStrength;

		float slopeRate = 2.0 * bendStrength * windStrength / (upperBendRange * bendLength);
		float slope = slopeRate * arcLength;
		float slopeSquared = slope * slope;
		float tangentLength = sqrt(1.0 + slopeSquared);
		float bendCos = rcp(tangentLength);
		float bendSin = slope * bendCos;
		float horizontalDistance = arcLength * slope / (tangentLength + 1.0);
		float verticalDistance = arcLength * (slopeSquared < EPSILON_WIND_GEOMETRY ?
													 1.0 - slopeSquared / 6.0 + 3.0 * slopeSquared * slopeSquared / 40.0 :
													 log(slope + tangentLength) / slope);
		float3 bentDirection = bendDirection * bendCos - treeAxis * bendSin;
		float3 bentAxis = treeAxis * bendCos + bendDirection * bendSin;
		float3 directionChange = bentDirection - bendDirection;
		float3 axisChange = bentAxis - treeAxis;
		float3x3 rotation = (float3x3)Math::IdentityMatrix + float3x3(
																 directionChange.x * bendDirection + axisChange.x * treeAxis,
																 directionChange.y * bendDirection + axisChange.y * treeAxis,
																 directionChange.z * bendDirection + axisChange.z * treeAxis);

		float3 radialPosition = restPosition - root - treeAxis * vertexHeight;
		float curvature = vertexHeight < treeHeight ? slopeRate / (1.0 + slopeSquared) : 0.0;
		float axialScale = 1.0 - curvature * dot(radialPosition, bendDirection);
		float inverseAxialScale = rcp((axialScale < 0.0 ? -1.0 : 1.0) *
									  max(abs(axialScale), EPSILON_WIND_GEOMETRY));
		normalTransform = mul(rotation, (float3x3)Math::IdentityMatrix +
											(inverseAxialScale - 1.0) * float3x3(
																			treeAxis.x * treeAxis, treeAxis.y * treeAxis, treeAxis.z * treeAxis));
		float3 centerDisplacement = bendDirection * horizontalDistance +
		                            treeAxis * (verticalDistance - arcLength);
		float3 sectionPosition = radialPosition + treeAxis * max(vertexHeight - treeHeight, 0.0) +
		                         (animatedPosition - restPosition);
		return centerDisplacement + mul(rotation, sectionPosition) - sectionPosition;
	}

	struct SamplePositions
	{
		float3 root;
		float3 base;
		float3 middle;
		float3 top;
	};

	SamplePositions BuildSamplePositions(row_major float3x4 worldMatrix, float3 worldOffset)
	{
		SamplePositions positions;
		positions.base =
			mul(worldMatrix, float4(Permutation::TreeWindProbeBase.xyz, 1.0)).xyz + worldOffset;
		positions.top =
			mul(worldMatrix, float4(Permutation::TreeWindProbeTop.xyz, 1.0)).xyz + worldOffset;
		positions.middle = (positions.base + positions.top) * 0.5;
		positions.root = positions.base;
		return positions;
	}

	SamplePositions BuildSamplePositions(row_major float4x4 worldMatrix, float3 worldOffset)
	{
		return BuildSamplePositions((row_major float3x4)worldMatrix, worldOffset);
	}

	struct Sample
	{
		float3 trunkVelocity;
		float leafAnimationStrength;
	};

	namespace Detail
	{
		float3 LimitTrunkTransientVelocity(float3 transientVelocity)
		{
			float bendSensitivity = max(Permutation::TrunkWindBendSensitivity, 0.0f) *
			                        max(Permutation::TreeBendModelSensitivity, 0.0f);
			float responseSpeed = length(transientVelocity) * bendSensitivity;
			float maximumResponse = max(Permutation::TreeTransientMaximumBendMultiplier, 0.0f);
			if (maximumResponse <= EPSILON_WIND_RESPONSE)
				return 0.0.xxx;
			float responseRatio = responseSpeed / maximumResponse;
			return transientVelocity * rsqrt(1.0 + responseRatio * responseRatio);
		}

		Sample ResolveSample(
			WindField::TransientImpulseSample trunkTransientSample,
			WindField::TransientImpulseSample leafTransientSample,
			float2 filteredAmbientVelocity, float2 transientInfluence)
		{
			Sample sample;
			float3 trunkAmbientVelocity = float3(filteredAmbientVelocity, 0.0f);
			float3 trunkTransientVelocity = LimitTrunkTransientVelocity(
				trunkTransientSample.velocity * max(transientInfluence.x, 0.0f));
			float leafTransientInfluence = max(transientInfluence.y, 0.0f);
			float transientLeafSpeed =
				length(leafTransientSample.velocity.xy) +
				length(trunkTransientSample.velocity.xy) * TRANSIENT_LEAF_STRUCTURAL_COUPLING;
			float transientLeafAnimationStrength = min(
				transientLeafSpeed * TRANSIENT_LEAF_FLUTTER_GAIN * leafTransientInfluence,
				max(Permutation::TreeLeafTransientFlutterMaximum, 0.0f));
			sample.trunkVelocity = trunkAmbientVelocity + trunkTransientVelocity;
			sample.leafAnimationStrength =
				length(filteredAmbientVelocity) * GetLeafFlutterGain() +
				transientLeafAnimationStrength;
			return sample;
		}

		WindField::TransientImpulseSample SampleCurrentTransient(SamplePositions positions)
		{
			return ResolveTransientSample(
				Permutation::TreeWindBoundsHeight,
				SampleCurrentTransientAt(positions.base),
				SampleCurrentTransientAt(positions.middle),
				SampleCurrentTransientAt(positions.top));
		}

		WindField::TransientImpulseSample SamplePreviousTransient(SamplePositions positions)
		{
			return ResolveTransientSample(
				Permutation::TreeWindBoundsHeight,
				SamplePreviousTransientAt(positions.base),
				SamplePreviousTransientAt(positions.middle),
				SamplePreviousTransientAt(positions.top));
		}

		WindField::TransientImpulseSample SampleCurrentImmediateTransient(SamplePositions positions)
		{
			return ResolveTransientSample(
				Permutation::TreeWindBoundsHeight,
				SampleCurrentImmediateTransientAt(positions.base),
				SampleCurrentImmediateTransientAt(positions.middle),
				SampleCurrentImmediateTransientAt(positions.top));
		}

		WindField::TransientImpulseSample SamplePreviousImmediateTransient(SamplePositions positions)
		{
			return ResolveTransientSample(
				Permutation::TreeWindBoundsHeight,
				SamplePreviousImmediateTransientAt(positions.base),
				SamplePreviousImmediateTransientAt(positions.middle),
				SamplePreviousImmediateTransientAt(positions.top));
		}
	}

	Sample SampleCurrent(SamplePositions positions, float2 transientInfluence)
	{
		return Detail::ResolveSample(
			Detail::SampleCurrentTransient(positions),
			Detail::SampleCurrentImmediateTransient(positions),
			TreeWindSpring::SampleCurrent(positions.root.xy), transientInfluence);
	}

	Sample SampleCurrent(
		SamplePositions positions, float3 leafWorldPosition,
		float2 transientInfluence)
	{
		return Detail::ResolveSample(
			Detail::SampleCurrentTransient(positions),
			Detail::SampleCurrentImmediateTransientAt(leafWorldPosition),
			TreeWindSpring::SampleCurrent(positions.root.xy), transientInfluence);
	}

	Sample SamplePrevious(SamplePositions positions, float2 transientInfluence)
	{
		return Detail::ResolveSample(
			Detail::SamplePreviousTransient(positions),
			Detail::SamplePreviousImmediateTransient(positions),
			TreeWindSpring::SamplePrevious(positions.root.xy), transientInfluence);
	}

	Sample SamplePrevious(
		SamplePositions positions, float3 leafWorldPosition,
		float2 transientInfluence)
	{
		return Detail::ResolveSample(
			Detail::SamplePreviousTransient(positions),
			Detail::SamplePreviousImmediateTransientAt(leafWorldPosition),
			TreeWindSpring::SamplePrevious(positions.root.xy), transientInfluence);
	}
}

#endif  // __TREE_WIND_DEPENDENCY_HLSL__
