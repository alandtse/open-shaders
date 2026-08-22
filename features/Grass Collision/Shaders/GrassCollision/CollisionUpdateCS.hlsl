
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

cbuffer PerFrameCB : register(b0)
{
	float2 PosOffset;   // cell origin in camera space
	uint2 ArrayOrigin;  // xy: array origin (clipmap wrapping)

	int2 ValidMargin;
	float TimeDelta;
	uint BoundingBoxCount;

	float CameraHeightDelta;
	float GrassInteractionRadius;
}

struct BoundingBoxPacked
{
	float2 MinExtent;
	float2 MaxExtent;
	uint IndexStart;
	uint IndexEnd;
	float2 pad0;
};

StructuredBuffer<BoundingBoxPacked> CollisionBoundingBoxes : register(t0);

struct CollisionShapePacked
{
	float4 PointAAndRadius;
	float4 PointB;
};

StructuredBuffer<CollisionShapePacked> CollisionInstances : register(t1);

RWTexture2D<float4> Collision : register(u0);

groupshared BoundingBoxPacked SharedBoundingBoxes[64];

[numthreads(8, 8, 1)] void main(
	uint3 groupId : SV_GroupID, uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
	if (groupIndex < BoundingBoxCount)
		SharedBoundingBoxes[groupIndex] = CollisionBoundingBoxes[groupIndex];

	GroupMemoryBarrierWithGroupSync();

	const uint TEXTURE_SIZE = 512;
	const float WORLD_SIZE = 4096;
	const float2 ZRANGE = float2(2048.0, -2048.0);
	const float CONTACT_STIFFNESS = 16.0;
	const float CONTACT_DAMPING = 8.0;
	const float RECOVERY_STIFFNESS = 0.35;
	const float RECOVERY_DAMPING = 1.2;

	const int2 textureSize = int2(TEXTURE_SIZE, TEXTURE_SIZE);
	int2 cellID = int2(dispatchThreadId.xy) - int2(ArrayOrigin);
	cellID = cellID % textureSize;
	cellID += int2(cellID < 0) * textureSize;

	float2 cellCentreMS = float2(cellID) + 0.5 - TEXTURE_SIZE / 2;
	cellCentreMS = cellCentreMS / TEXTURE_SIZE * WORLD_SIZE + PosOffset.xy;

	// Check if the cell is newly added
	int2 validMin = max(int2(0, 0), ValidMargin);
	int2 validMax = int2(TEXTURE_SIZE - 1, TEXTURE_SIZE - 1) + min(int2(0, 0), ValidMargin);
	bool isValid = all(cellID >= validMin) && all(cellID <= validMax);

	float targetHeight = ZRANGE.x;

	for (uint i = 0; i < BoundingBoxCount; i++) {
		BoundingBoxPacked boundingBox = SharedBoundingBoxes[i];
		// Test high level collision
		if (all(cellCentreMS >= boundingBox.MinExtent && cellCentreMS <= boundingBox.MaxExtent)) {
			// Process collision data
			for (uint j = boundingBox.IndexStart; j < boundingBox.IndexEnd; j++) {
				CollisionShapePacked collisionInstance = CollisionInstances[j];
				float3 pointA = collisionInstance.PointAAndRadius.xyz;
				float3 pointB = collisionInstance.PointB.xyz;
				float radius = collisionInstance.PointAAndRadius.w;
				float footprintRadius = radius + max(GrassInteractionRadius, 0.0);
				float profileScale = radius / footprintRadius;
				float2 capsuleAxis = pointB.xy - pointA.xy;
				float capsuleAxisLengthSquared = dot(capsuleAxis, capsuleAxis);
				float capsulePosition = capsuleAxisLengthSquared > 1e-5 ?
				                            saturate(dot(cellCentreMS - pointA.xy, capsuleAxis) / capsuleAxisLengthSquared) :
				                            0.0;
				float3 closestPoint = lerp(pointA, pointB, capsulePosition);
				float lowestHeight = ZRANGE.x;
				bool intersectsShape = false;

				float lowerCapDistance = distance(pointA.xy, cellCentreMS);
				if (lowerCapDistance < footprintRadius) {
					float profileDistance = lowerCapDistance * profileScale;
					lowestHeight = pointA.z - sqrt(max(radius * radius - profileDistance * profileDistance, 0.0));
					intersectsShape = true;
				}

				float segmentDistance = distance(closestPoint.xy, cellCentreMS);
				if (segmentDistance < footprintRadius) {
					float profileDistance = segmentDistance * profileScale;
					float segmentHeight = closestPoint.z - sqrt(max(radius * radius - profileDistance * profileDistance, 0.0));
					lowestHeight = min(lowestHeight, segmentHeight);
					intersectsShape = true;
				}

				if (intersectsShape)
					targetHeight = min(targetHeight, lowestHeight);
			}
		}
	}

	targetHeight = clamp(targetHeight, ZRANGE.y, ZRANGE.x);

	float collisionHeight = ZRANGE.x;
	float recoveryVelocity = 0.0;
	if (isValid) {
		float4 previousState = Collision[dispatchThreadId.xy];
		collisionHeight = lerp(ZRANGE.x, ZRANGE.y, previousState.x) + CameraHeightDelta;
		recoveryVelocity = previousState.y;
	}

	float previousCollisionHeight = collisionHeight;
	float deltaTime = max(TimeDelta, 0.0);
	float targetDelta = targetHeight - collisionHeight;
	float stiffness = targetDelta < 0.0 ? CONTACT_STIFFNESS : RECOVERY_STIFFNESS;
	float damping = targetDelta < 0.0 ? CONTACT_DAMPING : RECOVERY_DAMPING;
	float denominator = 1.0 + damping * deltaTime + stiffness * deltaTime * deltaTime;
	recoveryVelocity = (recoveryVelocity + stiffness * targetDelta * deltaTime) / denominator;
	collisionHeight += recoveryVelocity * deltaTime;

	float remainingDelta = targetHeight - collisionHeight;
	if (targetDelta * remainingDelta <= 0.0) {
		collisionHeight = targetHeight;
		recoveryVelocity = 0.0;
	}

	collisionHeight = clamp(collisionHeight, ZRANGE.y, ZRANGE.x);
	previousCollisionHeight = clamp(previousCollisionHeight, ZRANGE.y, ZRANGE.x);
	float encodedCollisionHeight = (collisionHeight - ZRANGE.x) / (ZRANGE.y - ZRANGE.x);
	float encodedPreviousCollisionHeight = (previousCollisionHeight - ZRANGE.x) / (ZRANGE.y - ZRANGE.x);

	Collision[dispatchThreadId.xy] =
		float4(encodedCollisionHeight, recoveryVelocity, encodedPreviousCollisionHeight, 0.0);
}
