namespace GrassCollision
{
	Texture2D<float4> Collision : register(t100);

	const static uint TEXTURE_SIZE = 512;
	const static float WORLD_SIZE = 4096;
	const static float CELL_SIZE = WORLD_SIZE / TEXTURE_SIZE;
	const static float2 ZRANGE = float2(2048.0, -2048.0);
	const static float MAXIMUM_BEND_ANGLE = radians(75.0);

	void GetCollision(float3 worldPosition, float maximumDepth, out float collisionHeights, out float collisionAmount, out float previousCollisionHeights, out float previousCollisionAmount)
	{
		float2 positionMSAdjusted = worldPosition.xy - SharedData::grassCollisionData.PosOffset;
		float2 uv = positionMSAdjusted / WORLD_SIZE + .5;

		float2 cellVxCoord = uv * TEXTURE_SIZE;
		int2 cell000 = floor(cellVxCoord - 0.5);
		float2 bilinearPos = cellVxCoord - 0.5 - cell000;

		int2 cellID = cell000;

		collisionHeights = 0.0;
		collisionAmount = 0.0;

		previousCollisionHeights = 0.0;
		previousCollisionAmount = 0.0;

		float wsum = 0;

		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++) {
				int2 offset = int2(i, j);
				int2 cellID = cell000 + offset;

				if (any(cellID < 0) || any((uint2)cellID >= TEXTURE_SIZE))
					continue;

				float2 cellCentreMS = cellID + 0.5 - TEXTURE_SIZE / 2;
				cellCentreMS = cellCentreMS * CELL_SIZE;

				float2 bilinearWeights = 1 - abs(offset - bilinearPos);
				float w = bilinearWeights.x * bilinearWeights.y;

				uint2 cellTexID = ((uint2)cellID + SharedData::grassCollisionData.ArrayOrigin) % TEXTURE_SIZE;

				float4 collisionSample = Collision[cellTexID];
				float collisionHeight = lerp(ZRANGE.x, ZRANGE.y, collisionSample.x);
				float previousCollisionHeight = lerp(ZRANGE.x, ZRANGE.y, collisionSample.z);

				collisionHeights += collisionHeight * w;
				collisionAmount += max(0, min(maximumDepth, worldPosition.z - collisionHeight)) * w;

				previousCollisionHeights += previousCollisionHeight * w;
				previousCollisionAmount += max(0, min(maximumDepth, worldPosition.z - previousCollisionHeight)) * w;

				wsum += w;
			}

		if (wsum > 0.0) {
			collisionHeights /= wsum;
			collisionAmount /= wsum;
			previousCollisionHeights /= wsum;
			previousCollisionAmount /= wsum;
		} else {
			collisionHeights = ZRANGE.x;
			collisionAmount = 0.0;
			previousCollisionHeights = ZRANGE.x;
			previousCollisionAmount = 0.0;
		}
	}

	float3 ComputeNormalFromHeights(float h0, float hX, float hY, float delta)
	{
		float3 tangentX = float3(delta, 0, hX - h0);
		float3 tangentY = float3(0, delta, hY - h0);
		float3 crossProd = cross(tangentX, tangentY) * float3(1.0, 1.0, 0.1);

		float lenSq = dot(crossProd, crossProd);
		return lenSq > 1e-12 ? -crossProd * rsqrt(lenSq) : float3(0, 0, -1);
	}

	void ComputeCollision(float3 worldPosition, float maximumDepth, float delta, out float2 collisionLateralNormal, out float collisionAmount, out float2 previousCollisionLateralNormal, out float previousCollisionAmount)
	{
		// Sample collision at three points forming a small triangle
		float collisionCenter;
		float collisionX;
		float collisionY;

		float collisionCenterAmount;
		float collisionXAmount;
		float collisionYAmount;

		float previousCollisionCenter;
		float previousCollisionX;
		float previousCollisionY;

		float previousCollisionCenterAmountSample;
		float previousCollisionXAmountSample;
		float previousCollisionYAmountSample;

		GetCollision(worldPosition, maximumDepth, collisionCenter, collisionCenterAmount, previousCollisionCenter, previousCollisionCenterAmountSample);
		GetCollision(worldPosition + float3(delta, 0, 0), maximumDepth, collisionX, collisionXAmount, previousCollisionX, previousCollisionXAmountSample);
		GetCollision(worldPosition + float3(0, delta, 0), maximumDepth, collisionY, collisionYAmount, previousCollisionY, previousCollisionYAmountSample);

		float3 currentAmounts = float3(collisionCenterAmount, collisionXAmount, collisionYAmount);
		collisionAmount = dot(currentAmounts, float3(1.0, 1.0, 1.0)) / 3.0;
		collisionLateralNormal = ComputeNormalFromHeights(collisionCenter, collisionX, collisionY, delta).xy;

		float3 previousAmounts = float3(previousCollisionCenterAmountSample, previousCollisionXAmountSample, previousCollisionYAmountSample);
		previousCollisionAmount = dot(previousAmounts, float3(1.0, 1.0, 1.0)) / 3.0;
		previousCollisionLateralNormal =
			ComputeNormalFromHeights(previousCollisionCenter, previousCollisionX, previousCollisionY, delta).xy;
	}

	float3 CalculateBendDisplacement(
		float3 modelPosition, float3 instanceRoot, float2 worldLateralNormal,
		float collisionAmount, float maximumDepth, float responseScale, float4x4 worldMatrix)
	{
		float lateralStrength = length(worldLateralNormal);
		if (lateralStrength <= 1e-5 || maximumDepth <= 1e-5)
			return 0.0;

		float3 worldBendDirection = float3(worldLateralNormal / lateralStrength, 0.0);
		float3 modelBendDirection = mul(transpose((float3x3)worldMatrix), worldBendDirection);
		modelBendDirection.z = 0.0;
		float modelDirectionLength = length(modelBendDirection);
		if (modelDirectionLength <= 1e-5)
			return 0.0;

		modelBendDirection /= modelDirectionLength;
		float3 bendAxis = cross(float3(0.0, 0.0, 1.0), modelBendDirection);
		float collisionResponse = saturate(collisionAmount / maximumDepth) * saturate(lateralStrength) * responseScale;
		float bendAngle = MAXIMUM_BEND_ANGLE * collisionResponse;
		float3 relativePosition = modelPosition - instanceRoot;
		return Wind::Common::RotateVector(relativePosition, bendAxis, bendAngle) - relativePosition;
	}

	void GetDisplacedPosition(VS_INPUT input, float3 position, out float3 displacement, out float3 previousDisplacement)
	{
		float3 worldPosition = mul(World[0], float4(position.xyz, 1.0)).xyz;
		float nearFactor = smoothstep(2048.0, 0.0, length(worldPosition));

		if (input.Color.w > 0.0 && nearFactor > 0.0) {
			float3 worldPositionCentre = mul(World[0], float4(input.InstanceData1.xyz, 1.0)).xyz;

			float3 collisionSamplePosition = float3(worldPositionCentre.xy, worldPosition.z);

			float maximumDepth = worldPosition.z - worldPositionCentre.z;

			float2 collisionLateralNormal, previousCollisionLateralNormal;
			float collisionAmount, previousCollisionAmount;
			ComputeCollision(collisionSamplePosition, maximumDepth, CELL_SIZE,
				collisionLateralNormal, collisionAmount, previousCollisionLateralNormal, previousCollisionAmount);

			float alpha = saturate(input.Color.w * 10.0);
			float responseScale = alpha * nearFactor * 0.75 *
			                      max(SharedData::grassCollisionData.CollisionImpactStrength, 0.0);
			displacement = CalculateBendDisplacement(
				position, input.InstanceData1.xyz, collisionLateralNormal,
				collisionAmount, maximumDepth, responseScale, World[0]);
			previousDisplacement = CalculateBendDisplacement(
				position, input.InstanceData1.xyz, previousCollisionLateralNormal,
				previousCollisionAmount, maximumDepth, responseScale, PreviousWorld[0]);
		} else {
			displacement = 0.0;
			previousDisplacement = 0.0;
		}
	}
}
