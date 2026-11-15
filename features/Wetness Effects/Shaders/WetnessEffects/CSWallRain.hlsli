#include "Common/Game.hlsli"
#ifndef CS_WALL_RAIN_HLSLI
#define CS_WALL_RAIN_HLSLI

namespace CSWallRain
{
	uint HashWallRainBits(uint value)
	{
		value ^= value >> 16u;
		value *= 0x7FEB352Du;
		value ^= value >> 15u;
		value *= 0x846CA68Bu;
		return value ^ (value >> 16u);
	}

	float4 UnpackWallRainRandom(uint value)
	{
		return (float4(value & 255u, (value >> 8u) & 255u, (value >> 16u) & 255u, value >> 24u) + 0.5) * (1.0 / 256.0);
	}

	float4 GetWallRainNoise(float position, uint seed)
	{
		int node = int(floor(position));
		float t = frac(position);
		float2 a = UnpackWallRainRandom(HashWallRainBits(asuint(node) * 747796405u ^ seed)).xy * 2.0 - 1.0;
		float2 b = UnpackWallRainRandom(HashWallRainBits(asuint(node + 1) * 747796405u ^ seed)).xy * 2.0 - 1.0;
		return float4(lerp(a, b, t * t * (3.0 - 2.0 * t)), (b - a) * (6.0 * t * (1.0 - t)));
	}

	float2 WallRainProfile(float distance, float radius, float footprint)
	{
		float filteredRadius = radius + footprint * 0.5;
		float x = distance / filteredRadius;
		float profile = saturate(1.0 - x * x);
		float coverage = radius / filteredRadius;
		return float2(profile * profile, -4.0 * x * profile / filteredRadius) * coverage;
	}

	float2 WallRainRamp(float position, float start, float end)
	{
		float t = saturate((position - start) / (end - start));
		return float2(t * t * (3.0 - 2.0 * t), 6.0 * t * (1.0 - t) / (end - start));
	}

	float2 GetWallRainBeadUV(float2 position, float2 asymmetry, float taper, float neck)
	{
		float2 square = position * position;
		float2 bend = square / (1.0 + square);
		float vertical = position.y * rsqrt(1.0 + square.y);
		return float2(position.x * (1.0 - taper * vertical + neck * (rcp(1.0 + 4.0 * square.y) - 0.2)) + asymmetry.x * bend.y,
			position.y + asymmetry.y * bend.x);
	}

	float3 GetWallRainBead(float2 position, float2 footprint, float width, float headLength, float heightRatio, float angle, float2 asymmetry, float taper, float neck)
	{
		float sinAngle, cosAngle;
		sincos(angle, sinAngle, cosAngle);
		float2 axisX = float2(cosAngle, sinAngle);
		float2 axisY = float2(-sinAngle, cosAngle);
		float2 local = float2(dot(position, axisX) / width, dot(position, axisY) / headLength);
		float2 uv = GetWallRainBeadUV(local, asymmetry, taper, neck);
		float radiusSquared = dot(uv, uv);
		float radius = sqrt(radiusSquared);
		float2 surfacePosition = local / max(radius, 1.0);
		float2 surfaceUV = GetWallRainBeadUV(surfacePosition, asymmetry, taper, neck);
		surfaceUV /= max(length(surfaceUV), 1.0);
		float2 invSquare = rcp(1.0 + surfacePosition * surfacePosition);
		float2 bendDerivative = 2.0 * surfacePosition * invSquare * invSquare;
		float vertical = surfacePosition.y * sqrt(invSquare.y);
		float verticalDerivative = invSquare.y * sqrt(invSquare.y);
		float neckProfile = rcp(1.0 + 4.0 * surfacePosition.y * surfacePosition.y);
		float2 localGradient = float2(
			(1.0 - taper * vertical + neck * (neckProfile - 0.2)) * surfaceUV.x + asymmetry.y * bendDerivative.x * surfaceUV.y,
			(asymmetry.x * bendDerivative.y - surfacePosition.x * (taper * verticalDerivative + 8.0 * neck * surfacePosition.y * neckProfile * neckProfile)) * surfaceUV.x + surfaceUV.y);
		float2 radialGradient = axisX * (localGradient.x / width) + axisY * (localGradient.y / headLength);
		float edgeWidth = max(dot(abs(radialGradient), footprint) / max(min(radius, 1.0), 1e-4), 0.002);
		float coverage = 1.0 - smoothstep(1.0 - edgeWidth * 0.5, 1.0 + edgeWidth * 0.5, radius);
		float capRadius = (1.0 + heightRatio * heightRatio) / (2.0 * heightRatio);
		float capDepth = sqrt(capRadius * capRadius - min(radiusSquared, 1.0));
		float2 slope = -width * radialGradient / capDepth;
		return float3(slope * coverage, coverage);
	}

	struct WallRainProjection
	{
		float2 position;
		float2 footprint;
		float2x2 metric;
		float2 down;
		float slope;
	};

	float GetWallRainTravelLimit(float2 margin, float2 direction)
	{
		float2 limit = max(margin, 0.0) / max(abs(direction), 1e-4);
		return min(limit.x, limit.y);
	}

	float4 GetWallRainDrop(float2 position, WallRainProjection projection, float2 cellSize, float4 random, float age, float lifetime, float layerScale)
	{
		float2 footprint = projection.footprint;
		bool stormLayer = layerScale < 1.0;
		float width = (stormLayer ? 0.0024 : 0.003) * lerp(0.55, 1.2, random.y) * SharedData::wetnessEffectsSettings.WallRainDropSize;
		position = mul(projection.metric, position - (random.xz - 0.5) * cellSize);
		float bendAmplitude = cellSize.x * 0.02;
		float bendFrequency = 5.0 / layerScale;
		if (abs(position.x) > width * 1.6 + bendAmplitude + footprint.x + bendAmplitude * bendFrequency * footprint.y)
			return 0.0;
		float phase = saturate(age / lifetime);
		float birth = saturate(age / 0.006);
		float waterLife = birth * (1.0 - smoothstep(0.45, 0.9, phase));
		float dampLife = birth * (1.0 - smoothstep(0.65, 1.0, phase));
		float4 shape = frac(random.zwxy * float4(13.17, 7.31, 19.73, 11.91));
		float bondNumber = width * width / (0.0027 * 0.0027);
		float deformation = bondNumber / (1.0 + bondNumber);
		float mobility = smoothstep(0.25 + shape.z * 0.15, 0.9 + shape.z * 0.3, bondNumber * projection.slope * (stormLayer ? 1.6 : 1.0));
		float holdTime = lerp(1.8, 0.25, mobility) * lerp(0.8, 1.2, shape.w) * (stormLayer ? 0.4 : 1.0);
		float releaseTime = stormLayer ? lerp(0.15, 0.35, shape.x) : lerp(0.35, 0.65, shape.x);
		float flowAge = max(age - holdTime, 0.0);
		float releaseAge = min(flowAge, releaseTime);
		float flowSpeed = (stormLayer ? 0.024 : 0.012) * mobility * projection.slope * SharedData::wetnessEffectsSettings.WallRainSpeed * SharedData::wetnessEffectsSettings.WallRainDropSize;
		float motion = mobility * projection.slope * saturate(flowAge / releaseTime) * saturate(SharedData::wetnessEffectsSettings.WallRainSpeed);
		float headLength = width * (lerp(0.9, 1.15, shape.y) + 0.4 * deformation * projection.slope + 0.25 * motion);
		float angle = (shape.x - 0.5) * 1.2 * (1.0 - motion * 0.7);
		float2 asymmetry = (shape.zw - 0.5) * (0.12 + 1.35 * deformation);
		float2 beadExtent = width * sqrt(6.25 + 3.99 * projection.down * projection.down) + cellSize.x * 0.02;
		float travelLimit = min(width * (stormLayer ? 7.0 : 5.0), GetWallRainTravelLimit(cellSize * 0.44 - beadExtent, projection.down));
		float travel = min(travelLimit, flowSpeed * (flowAge - releaseAge + releaseAge * releaseAge / (2.0 * releaseTime)));
		float travelTime = travelLimit / max(flowSpeed, 1e-5);
		float arrivalTime = travelTime < releaseTime * 0.5 ? sqrt(2.0 * releaseTime * travelTime) : travelTime + releaseTime * 0.5;
		float headWater = flowSpeed > 1e-5 ? 1.0 - smoothstep(arrivalTime, arrivalTime + (stormLayer ? 0.6 : 1.2), flowAge) : 1.0;
		float spawnPosition = -travelLimit * 0.4;
		float headPosition = spawnPosition + travel;
		float along = position.y - headPosition;

		float bendPhase = position.y * bendFrequency + random.z * 6.2831853;
		float bend = sin(bendPhase) * bendAmplitude;
		float bendDerivative = cos(bendPhase) * bendAmplitude * bendFrequency;
		float across = position.x - bend;
		float acrossFootprint = footprint.x + abs(bendDerivative) * footprint.y;

		float trailLength = min(width * (stormLayer ? 5.0 : 3.5) * SharedData::wetnessEffectsSettings.WallRainTrailLength, travel);
		if (abs(across) > width * 1.6 + acrossFootprint || along < -max(headLength * 1.6, trailLength) - footprint.y || along > headLength * 1.6 + footprint.y)
			return 0.0;
		float trailStrength = smoothstep(width * 0.25, width, trailLength);
		float heightRatio = lerp(0.42, 0.52, shape.x) * rsqrt(1.0 + bondNumber) * sqrt(width / headLength);
		float taper = 0.18 * deformation * projection.slope + 0.1 * motion;
		float neck = (shape.y - 0.3) * 1.4 * deformation;
		float3 bead = GetWallRainBead(float2(across, along), float2(acrossFootprint, footprint.y), width, headLength, heightRatio, angle, asymmetry, taper, neck) * headWater;
		float2 trailAcross = WallRainProfile(across, width * 0.28, acrossFootprint);
		float2 tailFade = WallRainRamp(along, -max(trailLength, width * 0.25) - footprint.y * 0.5, 0.0);
		float2 headFade = WallRainRamp(along, -headLength * 0.25 - footprint.y * 0.5, headLength * 0.5 + footprint.y * 0.5);
		float2 trailAlong = float2(tailFade.x * (1.0 - headFade.x), tailFade.y * (1.0 - headFade.x) - tailFade.x * headFade.y) * trailStrength;
		float2 dampAcross = WallRainProfile(across, width * 0.9, acrossFootprint);

		float trail = trailAcross.x * trailAlong.x;
		float water = saturate(bead.z + trail) * waterLife;
		float damp = max(bead.z * 0.2, dampAcross.x * trailAlong.x * 0.35) * dampLife;
		float2 slope = bead.xy;
		slope += float2(trailAcross.y * trailAlong.x, trailAcross.x * trailAlong.y) * width * 0.06 * (1.0 - bead.z);
		slope *= waterLife;
		slope.y -= slope.x * bendDerivative;
		float normalFade = 1.0 - smoothstep(width, width * 4.0, max(acrossFootprint, footprint.y));
		return float4(slope * normalFade, water, damp);
	}

	float4 GetWallRainStreakEvent(float2 position, WallRainProjection projection, float2 cellSize, float4 random, uint seed, float age, float lifetime, bool stormLayer)
	{
		float2 footprint = projection.footprint;
		position = mul(projection.metric, position - (random.xz - 0.5) * cellSize * float2(0.5, 1.0));
		float width = 0.0014 * lerp(0.8, 1.2, random.y) * SharedData::wetnessEffectsSettings.WallRainDropSize;
		float headLength = width * 3.6;
		float2 downSquared = projection.down * projection.down;
		float2 extent = width * (2.2 * sqrt(saturate(1.0 - downSquared)) + sqrt(6.25 + 6.71 * downSquared));
		float halfLength = max(1e-4, min(0.11 * lerp(0.7, 1.3, random.x) * SharedData::wetnessEffectsSettings.WallRainTrailLength,
										 GetWallRainTravelLimit(cellSize * 0.4 - extent, projection.down)));
		if (abs(position.x) > width * 4.7 + footprint.x || abs(position.y) > halfLength + headLength + footprint.y)
			return 0.0;

		static const float waterKinematicViscosity = 1e-6;
		float filmThickness = lerp(0.00008, 0.000125, random.y) * (stormLayer ? 1.4 : 0.8);
		float flowSpeed = (9.81 / (3.0 * waterKinematicViscosity)) * filmThickness * filmThickness * projection.slope;
		flowSpeed *= SharedData::wetnessEffectsSettings.WallRainSpeed * SharedData::wetnessEffectsSettings.WallRainDropSize;
		float releaseTime = stormLayer ? lerp(0.2, 0.4, random.z) : lerp(0.7, 1.2, random.z);
		float holdTime = stormLayer ? lerp(0.05, 0.18, random.x) : lerp(0.5, 1.2, random.x);
		float flowAge = max(age - holdTime, 0.0);
		float releaseAge = min(flowAge, releaseTime);
		float travel = flowSpeed * (flowAge - releaseAge + releaseAge * releaseAge / (2.0 * releaseTime));
		float headPosition = -halfLength + min(travel, 2.0 * halfLength);
		float travelTime = (2.0 * halfLength) / max(flowSpeed, 1e-5);
		float arrivalTime = travelTime < releaseTime * 0.5 ? sqrt(2.0 * releaseTime * travelTime) : travelTime + releaseTime * 0.5;
		float headDrainTime = stormLayer ? 2.5 : 3.5;
		float headWater = smoothstep(0.0, 0.15, flowAge) * (1.0 - smoothstep(arrivalTime, arrivalTime + headDrainTime, flowAge));
		float eventLife = saturate(age / 0.02) * (1.0 - smoothstep(0.65, 1.0, age / lifetime));
		float trailAge = max(flowAge - releaseTime * 0.5 - max(position.y + halfLength, 0.0) / max(flowSpeed, 1e-5), 0.0);
		float drainageTime = stormLayer ? lerp(4.0, 6.0, random.x) : lerp(7.0, 11.0, random.x);
		float filmLevel = rsqrt(1.0 + trailAge / drainageTime);
		float filmDerivative = trailAge > 0.0 && position.y > -halfLength && flowSpeed > 1e-5 ?
		                           0.5 * filmLevel * filmLevel * filmLevel / (drainageTime * flowSpeed) :
		                           0.0;
		float frequency = lerp(18.0, 35.0, random.w);
		float4 noise = GetWallRainNoise(position.y * frequency + random.z * 13.0, seed);
		float bend = width * noise.x;
		float bendDerivative = width * noise.z * frequency;
		float2 headProfile = WallRainProfile(position.y - headPosition - headLength * 0.15, headLength * 1.5, 0.0) * headWater;
		float streamWidth = width * (0.95 + 0.45 * noise.y + 1.1 * headProfile.x);
		float widthDerivative = width * (0.45 * noise.w * frequency + 1.1 * headProfile.y);
		float endDistance = position.y - clamp(position.y, -halfLength, headPosition);
		float2 uv = float2((position.x - bend) / streamWidth, endDistance / headLength);
		float radiusSquared = dot(uv, uv);
		float radius = sqrt(radiusSquared);
		float2 edgeUV = uv / max(radius, 1.0);
		float2 radialGradient = float2(edgeUV.x / streamWidth,
			edgeUV.y / headLength - edgeUV.x * (bendDerivative + edgeUV.x * widthDerivative) / streamWidth);
		float edgeWidth = clamp(dot(abs(radialGradient), footprint) / max(min(radius, 1.0), 1e-4), 0.002, 0.5);
		float coverage = 1.0 - smoothstep(1.0 - edgeWidth * 0.5, 1.0 + edgeWidth * 0.5, radius);
		static const float capRadius = (1.0 + 0.32 * 0.32) / (2.0 * 0.32);
		float capDepth = sqrt(capRadius * capRadius - min(radiusSquared, 1.0));
		float2 slope = float2(0.0, widthDerivative * (capDepth - (capRadius - 0.32))) - streamWidth * radialGradient / capDepth;
		float height = streamWidth * (capDepth - (capRadius - 0.32));
		slope = (slope * filmLevel + float2(0.0, height * filmDerivative)) * coverage;
		float normalFade = 1.0 - smoothstep(width, width * 4.0, max(footprint.x + abs(bendDerivative) * footprint.y, footprint.y));
		float detailFade = 1.0 - smoothstep(width * 4.0, width * 12.0, max(footprint.x, footprint.y));
		return float4(slope * normalFade, coverage, coverage * 0.5) * (eventLife * detailFade);
	}

	float4 GetWallRainLayer(WallRainProjection projection, float2 cellSize, float rainAmount, float layerScale, uint seed, bool streaks, bool stormLayer)
	{
		float detailLimit = streaks ? min(0.00168 * SharedData::wetnessEffectsSettings.WallRainDropSize * 12.0, min(cellSize.x, cellSize.y) * 0.16) : cellSize.x * 0.24;
		if (max(projection.footprint.x, projection.footprint.y) >= detailLimit)
			return 0.0;
		float cellWidth = streaks ? min(cellSize.x, cellSize.y) : cellSize.x;
		float detailFade = 1.0 - smoothstep(cellWidth * 0.08, cellWidth * (streaks ? 0.16 : 0.24), max(projection.footprint.x, projection.footprint.y));
		float2 position = projection.position;
		float columnPosition = position.x / cellSize.x;
		int baseColumn = int(floor(columnPosition));
		int columnStep = frac(columnPosition) < 0.5 ? -1 : 1;
		float rainCoverage = stormLayer ? smoothstep(0.15, 0.85, rainAmount) * rainAmount * rainAmount : sqrt(rainAmount);
		float eventDensity = rainCoverage * SharedData::wetnessEffectsSettings.WallRainDensity * (streaks ? 0.95 : (stormLayer ? 1.5 : 0.6));
		if (streaks)
			eventDensity /= 1.0 + eventDensity;
		int sampleCount = 2 * int(ceil(eventDensity));
		float activeFade = streaks ? 0.08 : 0.04;
		float4 flow = 0.0;
		float normalCoverage = 0.0;
		[loop] for (int neighbour = 0; neighbour < 4; neighbour++)
		{
			int column = baseColumn + (neighbour >> 1) * columnStep;
			float rowOffset = float(column & 1) * 0.5;
			float rowPosition = position.y / cellSize.y + rowOffset;
			int rowStep = frac(rowPosition) < 0.5 ? -1 : 1;
			int2 cell = int2(column, int(floor(rowPosition)) + (neighbour & 1) * rowStep);
			float2 localPosition = position - (float2(cell) + float2(0.5, 0.5 - rowOffset)) * cellSize;
			uint cellSeed = HashWallRainBits(asuint(cell.x) * 1597334677u ^ asuint(cell.y) * 3812015801u ^ seed);
			[loop] for (int sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++)
			{
				uint stream = uint(sampleIndex >> 1);
				uint streamSeed = stream == 0u ? cellSeed : HashWallRainBits(cellSeed ^ (stream * 277803737u));
				float4 timing = UnpackWallRainRandom(streamSeed);
				float dropInterval = stormLayer ? lerp(0.8, 1.3, timing.z) : lerp(4.0, 6.0, timing.z);
				float streamInterval = stormLayer ? lerp(4.0, 6.0, timing.z) : lerp(8.0, 12.0, timing.z);
				float interval = streaks ? streamInterval : dropInterval;
				float clock = SharedData::wetnessEffectsSettings.Time / interval + timing.x;
				int cycle = int(floor(clock));
				float lifetime = interval * 1.85;
				int previous = sampleIndex & 1;
				float age = (frac(clock) + previous) * interval;
				if (age < lifetime) {
					uint eventSeed = HashWallRainBits(streamSeed ^ (asuint(cycle - previous) * 747796405u + 0xA511E9B3u));
					float4 random = UnpackWallRainRandom(eventSeed);
					float activity = saturate(eventDensity - float(stream));
					float active = smoothstep(random.w * (1.0 - activeFade), random.w * (1.0 - activeFade) + activeFade, activity);
					if (active > 0.0) {
						float4 waterSample;
						[branch] if (streaks)
						{
							waterSample = GetWallRainStreakEvent(localPosition, projection, cellSize, random, eventSeed, age, lifetime, stormLayer);
						}
						else
						{
							waterSample = GetWallRainDrop(localPosition, projection, cellSize, random, age, lifetime, layerScale);
						}
						waterSample *= detailFade;
						waterSample *= active;
						flow.xy += waterSample.xy;
						normalCoverage += waterSample.z;
						flow.zw = 1.0 - (1.0 - flow.zw) * (1.0 - waterSample.zw);
					}
				}
			}
		}
		flow.xy /= max(normalCoverage, 1.0);
		return flow;
	}

	float4 GetWallRainProjection(WallRainProjection projection, float2 streakCellSize, float rainAmount, uint seed)
	{
		float4 flow = 0.0;
		float normalCoverage = 0.0;
		float stormWeight = smoothstep(0.15, 0.85, rainAmount);
		float streakWeight = SharedData::wetnessEffectsSettings.WallRainLayering * 0.5 * projection.slope;
		[loop] for (int layer = 0; layer < 4; layer++)
		{
			bool stormLayer = (layer & 1) != 0;
			bool streaks = layer >= 2;
			float weight = streaks ? streakWeight : 1.0;
			weight *= stormLayer ? stormWeight : 1.0;
			if (weight <= 0.0)
				continue;
			WallRainProjection layerProjection = projection;
			if (layer == 1)
				layerProjection.position += float2(0.731, 1.173);
			float scale = layer == 1 ? 0.4 : 1.0;
			float2 cellSize = streaks ? streakCellSize : (stormLayer ? float2(0.024, 0.048) : float2(0.045, 0.09));
			if (!streaks)
				cellSize *= max(SharedData::wetnessEffectsSettings.WallRainDropSize, 1.0);
			if (streaks && stormLayer)
				cellSize.x *= 0.8;
			uint layerSeed = seed + (layer == 0 ? 0u : (layer == 1 ? 19u : (layer == 2 ? 37u : 59u)));
			float4 waterSample = GetWallRainLayer(layerProjection, cellSize, rainAmount, scale, layerSeed, streaks, stormLayer) * weight;
			normalCoverage += waterSample.z;
			if (layer == 0) {
				flow = waterSample;
			} else {
				flow.xy += waterSample.xy;
				flow.zw = 1.0 - (1.0 - flow.zw) * (1.0 - waterSample.zw);
			}
		}
		flow.xy /= max(normalCoverage, 1.0);
		return flow;
	}

	struct WallRainState
	{
		float3 gradient;
		float water;
		float damp;
	};

	WallRainProjection GetWallRainCoordinates(float3 position, float3 normal, float3 across, float3 down, float2 footprint, float slope, float3 axisU, float3 axisV, float3 axisN)
	{
		float normalComponent = dot(normal, axisN);
		float3 tangentU = axisU - axisN * (dot(normal, axisU) / normalComponent);
		float3 tangentV = axisV - axisN * (dot(normal, axisV) / normalComponent);
		WallRainProjection projection;
		projection.position = float2(dot(position, axisU), dot(position, axisV));
		projection.footprint = footprint;
		projection.metric = float2x2(dot(tangentU, across), dot(tangentV, across), dot(tangentU, down), dot(tangentV, down));
		projection.down = float2(dot(down, axisU), dot(down, axisV));
		projection.slope = slope;
		return projection;
	}

	WallRainState GetWallRainState(float3 rainPosition, float3 vertexNormal, float3 positionDx, float3 positionDy, float3 rainPositionDx, float3 rainPositionDy, bool attachToModel)
	{
		WallRainState state = (WallRainState)0;
		bool validProjection = true;
		float3 position = rainPosition * GAME_UNIT_TO_M;
		float slope = length(vertexNormal.xy);
		float3 down = slope > 1e-4 ? (vertexNormal * vertexNormal.z - float3(0, 0, 1)) / slope : float3(0, -1, 0);
		float3 rainNormal = vertexNormal;
		float3 reciprocalX = 0.0;
		float3 reciprocalY = 0.0;
		[branch] if (attachToModel)
		{
			float3 worldArea = cross(positionDx, positionDy);
			float3 modelArea = cross(rainPositionDx, rainPositionDy);
			float worldAreaSquared = dot(worldArea, worldArea);
			float modelAreaSquared = dot(modelArea, modelArea);
			validProjection = !(worldAreaSquared <= max(1e-20, dot(positionDx, positionDx) * dot(positionDy, positionDy) * 1e-8) ||
								modelAreaSquared <= max(1e-20, dot(rainPositionDx, rainPositionDx) * dot(rainPositionDy, rainPositionDy) * 1e-8));
			if (validProjection) {
				reciprocalX = cross(positionDy, worldArea) / worldAreaSquared;
				reciprocalY = cross(worldArea, positionDx) / worldAreaSquared;
				rainNormal = modelArea * rsqrt(modelAreaSquared) * (dot(worldArea, vertexNormal) < 0.0 ? -1.0 : 1.0);
				if (slope > 1e-4) {
					down = rainPositionDx * dot(reciprocalX, down) + rainPositionDy * dot(reciprocalY, down);
				}
				if (slope <= 1e-4 || dot(down, down) <= 1e-12) {
					float3 axis = abs(rainNormal.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0);
					down = rainNormal * dot(rainNormal, axis) - axis;
				}
				down *= rsqrt(max(dot(down, down), 1e-20));
			}
		}
		if (validProjection) {
			float3 across = normalize(cross(down, rainNormal));
			float2 footprint = (abs(float2(dot(rainPositionDx, across), dot(rainPositionDx, down))) + abs(float2(dot(rainPositionDy, across), dot(rainPositionDy, down)))) * GAME_UNIT_TO_M;
			float3 worldAcross = across;
			float3 worldDown = down;
			[branch] if (attachToModel)
			{
				worldAcross = reciprocalX * dot(rainPositionDx, across) + reciprocalY * dot(rainPositionDy, across);
				worldDown = reciprocalX * dot(rainPositionDx, down) + reciprocalY * dot(rainPositionDy, down);
			}
			float3 weights = smoothstep(0.35, 0.75, abs(rainNormal));
			weights *= weights;
			weights /= weights.x + weights.y + weights.z;
			float rainAmount = max(SharedData::wetnessEffectsSettings.Raining, 0.0) * 1.5;
			float4 flow = 0.0;
			[loop] for (int axis = 0; axis < 3; axis++)
			{
				float weight = axis == 0 ? weights.x : (axis == 1 ? weights.y : weights.z);
				if (weight > 0.0) {
					float3 axisU = axis == 0 ? float3(0, 1, 0) : float3(1, 0, 0);
					float3 axisV = axis == 2 ? float3(0, 1, 0) : float3(0, 0, -1);
					float3 axisN = axis == 0 ? float3(1, 0, 0) : (axis == 1 ? float3(0, 1, 0) : float3(0, 0, 1));
					uint seed = axis == 0 ? 3u : (axis == 1 ? 11u : 23u);
					float2 streakCellSize = axis == 2 ? float2(0.16, 0.16) : float2(0.045, 0.42);
					WallRainProjection projection = GetWallRainCoordinates(position, rainNormal, across, down, footprint, slope, axisU, axisV, axisN);
					flow += GetWallRainProjection(projection, streakCellSize, rainAmount, seed) * weight;
				}
			}

			state.gradient = (worldAcross * flow.x + worldDown * flow.y) / length(weights);
			state.gradient -= vertexNormal * dot(state.gradient, vertexNormal);
			state.gradient *= SharedData::wetnessEffectsSettings.WallRainNormalStrength;
			state.water = flow.z;
			state.damp = flow.w;
		}
		return state;
	}

	float GetWallRainFlowBlend(float wetnessOcclusion, float nearFactor)
	{
		if (!SharedData::wetnessEffectsSettings.EnableWetnessEffects ||
			!SharedData::wetnessEffectsSettings.EnableRaindropFx ||
			!SharedData::wetnessEffectsSettings.EnableWallRain ||
			SharedData::wetnessEffectsSettings.WallRainDensity <= 0.0)
	return 0.0;

		float rainAmount = smoothstep(0.0, 0.15, SharedData::wetnessEffectsSettings.Raining);
		return rainAmount * saturate(wetnessOcclusion * 2.0) * nearFactor;
	}
}

#endif
