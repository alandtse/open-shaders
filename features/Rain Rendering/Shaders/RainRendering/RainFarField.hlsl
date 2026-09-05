#include "Common/FrameBuffer.hlsli"
#include "RainRendering/RainConstants.hlsli"

Texture2D<float> SceneDepth : register(t0);

uint DistantRainHash(uint value)
{
	value ^= value >> 16u;
	value *= 0x7FEB352Du;
	value ^= value >> 15u;
	value *= 0x846CA68Bu;
	return value ^ (value >> 16u);
}

float DistantRainHash01(uint value)
{
	return float(DistantRainHash(value) >> 8u) * (1.0f / 16777216.0f);
}

uint DistantRainHashLattice(int2 lattice, uint seed)
{
	return DistantRainHash(asuint(lattice.x) ^ DistantRainHash(asuint(lattice.y) ^ seed));
}

float DistantRainNoise(float2 position, uint seed)
{
	int2 lattice = int2(floor(position));
	float2 offset = frac(position);
	float2 blend = offset * offset * (3.0f - 2.0f * offset);
	float value00 = DistantRainHash01(DistantRainHashLattice(lattice, seed));
	float value10 = DistantRainHash01(DistantRainHashLattice(lattice + int2(1, 0), seed));
	float value01 = DistantRainHash01(DistantRainHashLattice(lattice + int2(0, 1), seed));
	float value11 = DistantRainHash01(DistantRainHashLattice(lattice + int2(1, 1), seed));
	return lerp(lerp(value00, value10, blend.x), lerp(value01, value11, blend.x), blend.y);
}

int DistantRainPositiveModulo(int value, int divisor)
{
	int result = value % divisor;
	return result < 0 ? result + divisor : result;
}

int3 DistantRainWorldCell(uint3 slot, int3 baseCell, uint3 dimensions)
{
	int3 dimension = int3(dimensions);
	int3 baseSlot = int3(
		DistantRainPositiveModulo(baseCell.x, dimension.x),
		DistantRainPositiveModulo(baseCell.y, dimension.y),
		DistantRainPositiveModulo(baseCell.z, dimension.z));
	int3 offset = int3(
		DistantRainPositiveModulo(int(slot.x) - baseSlot.x, dimension.x),
		DistantRainPositiveModulo(int(slot.y) - baseSlot.y, dimension.y),
		DistantRainPositiveModulo(int(slot.z) - baseSlot.z, dimension.z));
	return baseCell + offset;
}

float3 DistantRainSafeNormalize(float3 value, float3 fallback)
{
	float lengthSquared = dot(value, value);
	return lengthSquared > 1e-6f ? value * rsqrt(lengthSquared) : fallback;
}

struct DistantRainVertexOutput
{
	float4 Position: SV_Position;
	float2 StreakCoordinate: TEXCOORD0;
	float4 ColorOpacity: TEXCOORD1;
	float ViewDepth: TEXCOORD2;
	float2 EyeClip: SV_ClipDistance0;
};

DistantRainVertexOutput DistantRainVS(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	DistantRainVertexOutput output = (DistantRainVertexOutput)0;
#ifdef VR
	uint eyeIndex = instanceID & 1u;
	uint dropIndex = instanceID >> 1u;
#else
	uint eyeIndex = 0u;
	uint dropIndex = instanceID;
#endif

	static const float2 corners[6] = {
		float2(-1.0f, -1.0f), float2(-1.0f, 1.0f), float2(1.0f, 1.0f),
		float2(-1.0f, -1.0f), float2(1.0f, 1.0f), float2(1.0f, -1.0f)
	};
	float2 corner = corners[vertexID];
	const uint3 gridDimensions = uint3(96u, 96u, 4u);
	const uint totalCellCount = gridDimensions.x * gridDimensions.y * gridDimensions.z;
	uint slotIndex = (dropIndex * 12347u + 5317u) % totalCellCount;
	uint3 slot = uint3(
		slotIndex % gridDimensions.x,
		(slotIndex / gridDimensions.x) % gridDimensions.y,
		slotIndex / (gridDimensions.x * gridDimensions.y));

	float3 volumeSize = max(VolumeSizeAndDensity.xyz, 1.0f.xxx);
	float3 cellSize = volumeSize / max(float3(gridDimensions) - 1.0f, 1.0f.xxx);
	float3 volumeMinimum = HeadPositionAndTime.xyz - volumeSize * 0.5f;
	int3 baseCell = int3(floor(volumeMinimum / cellSize));
	int3 worldCell = DistantRainWorldCell(slot, baseCell, gridDimensions);
	uint cellSeed = DistantRainHash(dropIndex ^ DistantRainHash(asuint(worldCell.x)) ^
									DistantRainHash(asuint(worldCell.y) + 0x9E3779B9u) ^ DistantRainHash(asuint(worldCell.z) + 0x85EBCA6Bu));

	float fallSpeed = max(WeatherFallDepth.y, 1.0f);
	float fallCycleHeight = max(cellSize.z, 1.0f);
	float fallDistance = max(HeadPositionAndTime.w, 0.0f) * fallSpeed +
	                     DistantRainHash01(cellSeed ^ 0xA511E9B3u) * fallCycleHeight;
	uint lifeCycle = uint(floor(fallDistance / fallCycleHeight));
	float lifeFraction = frac(fallDistance / fallCycleHeight);
	uint lifeSeed = DistantRainHash(cellSeed ^ DistantRainHash(lifeCycle + 0x63D83595u));
	float3 jitter = float3(
		DistantRainHash01(lifeSeed ^ 0xB5297A4Du),
		DistantRainHash01(lifeSeed ^ 0x68E31DA4u),
		0.0f);
	float3 dropPosition = (float3(worldCell) + jitter) * cellSize;
	dropPosition.z = (float(worldCell.z) + 1.0f - lifeFraction) * cellSize.z;

	float distanceFromHead = length(dropPosition - HeadPositionAndTime.xyz);
	float startDistance = max(LayerRadii.x, LayerRadii.y * 0.82f);
	float endDistance = max(LayerRadii.z, startDistance + 1.0f);
	float distanceFade = smoothstep(startDistance * 0.88f, startDistance, distanceFromHead) *
	                     (1.0f - smoothstep(endDistance * 0.90f, endDistance, distanceFromHead));

	float spatialNoise = DistantRainNoise(dropPosition.xy / max(DistanceNoise.y, 1.0f), 0xD1B54A35u);
	float spatialDensity = lerp(1.0f, lerp(0.32f, 1.68f, spatialNoise), saturate(DistanceNoise.z));
	const float2 curtainDirection = float2(0.8192319f, 0.5734624f);
	float2 curtainPerpendicular = float2(-curtainDirection.y, curtainDirection.x);
	float curtainScale = max(Curtain.x, 1.0f);
	float2 curtainCoordinate = float2(
		dot(dropPosition.xy, curtainDirection) / (curtainScale * 0.28f),
		dot(dropPosition.xy, curtainPerpendicular) / curtainScale);
	float curtainNoise = DistantRainNoise(curtainCoordinate, 0x94D049BBu);
	float curtainShape = pow(saturate(curtainNoise), max(Curtain.z, 0.1f));
	float curtainDensity = lerp(min(CurtainDensity.x, CurtainDensity.y), max(CurtainDensity.x, CurtainDensity.y), curtainShape);
	float density = DistantRain.x * VolumeSizeAndDensity.w * WeatherFallDepth.x * spatialDensity *
	                lerp(1.0f, curtainDensity, saturate(Curtain.y));
	if (distanceFade <= 0.0f || DistantRainHash01(lifeSeed ^ 0xC2B2AE35u) > saturate(density)) {
		output.Position = float4(2.0f, 2.0f, 0.0f, 1.0f);
		return output;
	}

	float3 streakAxis = float3(0.0f, 0.0f, -1.0f);
	float3 headDirection = DistantRainSafeNormalize(HeadPositionAndTime.xyz - dropPosition, float3(1.0f, 0.0f, 0.0f));
	float3 sideAxis = DistantRainSafeNormalize(cross(streakAxis, headDirection), float3(1.0f, 0.0f, 0.0f));
	float streakVariation = lerp(0.72f, 1.28f, DistantRainHash01(lifeSeed ^ 0x917AC53Du));
	float3 worldPosition = dropPosition +
	                       streakAxis * (corner.x * DistantRain.z * streakVariation * 0.5f) +
	                       sideAxis * (corner.y * DistantRain.w * 0.5f);
	float4 clipPosition = mul(
		FrameBuffer::CameraViewProj[eyeIndex],
		float4(worldPosition - FrameBuffer::CameraPosAdjust[eyeIndex].xyz, 1.0f));
	output.EyeClip = float2(clipPosition.w + clipPosition.x, clipPosition.w - clipPosition.x);
#ifdef VR
	clipPosition.x = clipPosition.x * 0.5f + (eyeIndex == 0u ? -0.5f : 0.5f) * clipPosition.w;
#endif

	float lightLuminance = dot(max(LightColor.rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
	float lighting = lerp(1.0f, 0.35f + sqrt(saturate(lightLuminance)), saturate(Appearance.y));
	lighting = max(lighting, Appearance.z);
	output.Position = clipPosition;
	output.StreakCoordinate = float2(corner.x * 0.5f + 0.5f, corner.y);
	output.ColorOpacity = float4(
		float3(0.62f, 0.72f, 0.82f) * Appearance.x * lighting,
		Streak.w * DistantRain.y * WeatherFallDepth.x * distanceFade);
	output.ViewDepth = max(abs(clipPosition.w), 0.0f);
	return output;
}

float DistantRainLinearDepth(float2 pixel)
{
	float rawSceneDepth = SceneDepth.Load(int3(pixel, 0));
	return RainCameraData.w / max(-rawSceneDepth * RainCameraData.z + RainCameraData.x, 1e-4f);
}

float4 DistantRainPS(DistantRainVertexOutput input) : SV_Target0
{
	float intersectionFade = saturate(
		(DistantRainLinearDepth(input.Position.xy) - input.ViewDepth) / max(WeatherFallDepth.w, 1.0f));
	float widthFade = saturate(1.0f - abs(input.StreakCoordinate.y));
	float endFade = smoothstep(0.0f, 0.12f, input.StreakCoordinate.x) *
	                smoothstep(0.0f, 0.12f, 1.0f - input.StreakCoordinate.x);
	float alpha = input.ColorOpacity.a * widthFade * endFade * intersectionFade * 0.55f;
	if (alpha <= 1e-4f)
		discard;
	return float4(input.ColorOpacity.rgb, alpha);
}
