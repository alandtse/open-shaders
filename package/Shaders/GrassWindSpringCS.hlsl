#define GRASS_WIND_SPRING_COMPUTE
#include "Common/GrassWindSpring.hlsli"
#include "Common/SharedData.hlsli"

RWTexture2D<float4> Response : register(u0);
RWTexture2D<float4> Velocity : register(u1);

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
	GrassWindSpring::FieldData field = GrassWindSpring::Fields[GrassWindSpring::ActiveField];
	uint2 fieldDimensions = uint2(field.TextureSize, field.TextureSize);
	if (any(dispatchThreadId.xy >= fieldDimensions))
		return;
	float cellSize = field.FieldSize / field.TextureSize;
	float2 worldPosition = field.FieldMinimum +
	                       (float2(dispatchThreadId.xy) + 0.5f) * cellSize;
	float3 target = GrassWindSpring::CalculateTarget(
		SharedData::SampleAmbientWind(float3(worldPosition, field.FieldHeight)).velocity, field);

	float3 response = target;
	float3 velocity = 0.0f.xxx;
	float2 previousCoordinate =
		(worldPosition - field.PreviousFieldMinimum) / cellSize;
	int2 previousCell = int2(floor(previousCoordinate));
	bool historyValid = field.Initialize == 0u &&
	                    all(previousCell >= 0) && all(previousCell < int2(field.TextureSize, field.TextureSize));
	if (historyValid) {
		response = PreviousResponse.Load(int3(previousCell, 0)).xyz;
		velocity = PreviousVelocity.Load(int3(previousCell, 0)).xyz;
		float3 nextResponse;
		float3 nextVelocity;
		GrassWindSpring::Advance(response, velocity, target, field.FrameTime,
			field.SpringFrequency, field.SpringDamping,
			nextResponse, nextVelocity);
		response = nextResponse;
		velocity = nextVelocity;
	}

	float bendMagnitude = length(response.xy);
	if (bendMagnitude > field.MaximumTiltRadians && bendMagnitude > 1e-5f) {
		float2 bendDirection = response.xy / bendMagnitude;
		response.xy = bendDirection * field.MaximumTiltRadians;
		velocity.xy -= bendDirection * max(dot(velocity.xy, bendDirection), 0.0f);
	}
	if (response.z <= 0.0f) {
		response.z = 0.0f;
		velocity.z = max(velocity.z, 0.0f);
	} else if (response.z >= 1.0f) {
		response.z = 1.0f;
		velocity.z = min(velocity.z, 0.0f);
	}

	Response[dispatchThreadId.xy] = float4(response, 0.0f);
	Velocity[dispatchThreadId.xy] = float4(velocity, 0.0f);
}
