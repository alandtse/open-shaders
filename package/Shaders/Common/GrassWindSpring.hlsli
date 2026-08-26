#ifndef __GRASS_WIND_SPRING_DEPENDENCY_HLSL__
#define __GRASS_WIND_SPRING_DEPENDENCY_HLSL__

namespace GrassWindSpring
{
	static const float TAU = 6.28318530717958647692f;

#if defined(GRASS_WIND_SPRING_COMPUTE)
	cbuffer SpringField : register(b0)
#else
	cbuffer SpringField : register(b9)
#endif
	{
		float2 FieldMinimum;
		float2 PreviousFieldMinimum;
		float FieldHeight;
		float FrameTime;
		float ResponseRadians;
		float MaximumTiltRadians;
		float Sensitivity;
		float SpringFrequency;
		float SpringDamping;
		uint Initialize;
		uint FieldAvailable;
		float FieldSize;
		uint TextureSize;
		float SpringPadding;
	};

#if !defined(GRASS_WIND_SPRING_COMPUTE)
	Texture2D<float4> ResponseField : register(t105);
	Texture2D<float4> PreviousResponseField : register(t106);
	SamplerState ResponseSampler : register(s14);
#endif

	float3 CalculateTarget(float3 windVelocity)
	{
		windVelocity *= max(Sensitivity, 0.0f);
		float lateralSpeed = length(windVelocity.xy);
		float targetAngle = MaximumTiltRadians > 1e-5f ?
		                        MaximumTiltRadians * tanh(lateralSpeed * max(ResponseRadians, 0.0f) / MaximumTiltRadians) :
		                        0.0f;
		float2 targetBend = lateralSpeed > 1e-5f ? windVelocity.xy * (targetAngle / lateralSpeed) : 0.0f.xx;
		float downwardSpeed = max(-windVelocity.z, 0.0f);
		float targetCompression = MaximumTiltRadians > 1e-5f ?
		                              saturate(tanh(downwardSpeed * max(ResponseRadians, 0.0f) / MaximumTiltRadians)) :
		                              0.0f;
		return float3(targetBend, targetCompression);
	}

	/** @brief Advances an exact damped oscillator over one constant-target interval. */
	void Advance(float3 position, float3 velocity, float3 target, float frameTime,
		float frequency, float damping, out float3 nextPosition, out float3 nextVelocity)
	{
		float deltaTime = max(frameTime, 0.0f);
		float angularFrequency = TAU * max(frequency, 1e-3f);
		float dampingRatio = max(damping, 0.0f);
		float3 displacement = position - target;

		if (dampingRatio < 0.999f) {
			float dampedFrequency = angularFrequency * sqrt(max(1.0f - dampingRatio * dampingRatio, 1e-5f));
			float phase = dampedFrequency * deltaTime;
			float phaseSin, phaseCos;
			sincos(phase, phaseSin, phaseCos);
			float decay = exp(-dampingRatio * angularFrequency * deltaTime);
			float dampingScale = dampingRatio * angularFrequency / dampedFrequency;
			float sinOverFrequency = phaseSin / dampedFrequency;
			nextPosition = target + decay *
			                            (displacement * (phaseCos + dampingScale * phaseSin) + velocity * sinOverFrequency);
			nextVelocity = decay *
			               (velocity * (phaseCos - dampingScale * phaseSin) -
							   displacement * (angularFrequency * angularFrequency * sinOverFrequency));
		} else if (dampingRatio <= 1.001f) {
			float decay = exp(-angularFrequency * deltaTime);
			nextPosition = target + decay *
			                            (displacement * (1.0f + angularFrequency * deltaTime) + velocity * deltaTime);
			nextVelocity = decay *
			               (velocity * (1.0f - angularFrequency * deltaTime) -
							   displacement * (angularFrequency * angularFrequency * deltaTime));
		} else {
			float root = sqrt(dampingRatio * dampingRatio - 1.0f);
			float rate1 = -angularFrequency * (dampingRatio - root);
			float rate2 = -angularFrequency * (dampingRatio + root);
			float inverseRateDelta = rcp(rate1 - rate2);
			float3 coefficient1 = (velocity - rate2 * displacement) * inverseRateDelta;
			float3 coefficient2 = displacement - coefficient1;
			float decay1 = exp(rate1 * deltaTime);
			float decay2 = exp(rate2 * deltaTime);
			nextPosition = target + coefficient1 * decay1 + coefficient2 * decay2;
			nextVelocity = rate1 * coefficient1 * decay1 + rate2 * coefficient2 * decay2;
		}
	}

#if !defined(GRASS_WIND_SPRING_COMPUTE)
	bool Contains(float2 worldPosition, float2 fieldMinimum)
	{
		float2 uv = (worldPosition - fieldMinimum) / FieldSize;
		return all(uv >= 0.0f) && all(uv <= 1.0f);
	}

	float4 SampleField(Texture2D<float4> field, float2 worldPosition, float2 fieldMinimum)
	{
		float2 uv = (worldPosition - fieldMinimum) / FieldSize;
		return Contains(worldPosition, fieldMinimum) ?
		           field.SampleLevel(ResponseSampler, saturate(uv), 0.0f) :
		           0.0f.xxxx;
	}

	float4 SampleCurrent(float2 worldPosition)
	{
		return FieldAvailable != 0u ? SampleField(ResponseField, worldPosition, FieldMinimum) : 0.0f.xxxx;
	}

	float4 SamplePrevious(float2 worldPosition)
	{
		return FieldAvailable != 0u ?
		           SampleField(PreviousResponseField, worldPosition, PreviousFieldMinimum) :
		           0.0f.xxxx;
	}

	bool HasTemporalCoverage(float2 worldPosition, float2 previousWorldPosition)
	{
		return FieldAvailable != 0u && Contains(worldPosition, FieldMinimum) &&
		       Contains(previousWorldPosition, PreviousFieldMinimum);
	}

	void ResolveModelBend(float4 fieldSample, float responseScale, float4x4 worldMatrix,
		out float3 bendAxis, out float bendAngle, out float compression)
	{
		float3 modelBend = mul(transpose((float3x3)worldMatrix), float3(fieldSample.xy, 0.0f));
		modelBend.z = 0.0f;
		float modelBendMagnitude = length(modelBend);
		float3 bendDirection = modelBendMagnitude > 1e-5f ?
		                           modelBend / modelBendMagnitude :
		                           float3(1.0f, 0.0f, 0.0f);
		bendAxis = cross(float3(0.0f, 0.0f, 1.0f), bendDirection);
		bendAngle = min(modelBendMagnitude * responseScale, max(MaximumTiltRadians, 0.0f));
		compression = saturate(fieldSample.z * responseScale);
	}
#endif
}

#endif  // __GRASS_WIND_SPRING_DEPENDENCY_HLSL__
