#include "Common/FrameBuffer.hlsli"
#include "LightLimitFix/Common.hlsli"

cbuffer PerFrame : register(b0)
{
	uint LightCount;
	uint3 pad0;  // Padding for 16-byte alignment: 4 -> 16 bytes
	uint4 ClusterSize;
}

//references
//https://github.com/pezcode/Cluster

StructuredBuffer<ClusterAABB> clusters : register(t0);
StructuredBuffer<Light> lights : register(t1);

RWStructuredBuffer<uint> lightIndexList : register(u1);
RWStructuredBuffer<LightGrid> lightGrid : register(u2);

groupshared float4 sharedLightsVS[GROUP_SIZE];
#if defined(VR)
groupshared float4 sharedLightsVSRight[GROUP_SIZE];
#endif

bool LightIntersectsCluster(float3 position, float radiusSquared, ClusterAABB cluster)
{
	float3 closest = max(cluster.minPoint.xyz, min(position, cluster.maxPoint.xyz));

	float3 dist = closest - position;
	return dot(dist, dist) <= radiusSquared;
}

[numthreads(NUMTHREAD_X, NUMTHREAD_Y, NUMTHREAD_Z)] void main(
	uint3 groupId : SV_GroupID, uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
	if (groupIndex < LightCount) {
		Light light = lights[groupIndex];
		sharedLightsVS[groupIndex] = float4(FrameBuffer::WorldToView(light.positionWS[0].xyz, true, 0), light.radius * light.radius);
#if defined(VR)
		sharedLightsVSRight[groupIndex] = float4(FrameBuffer::WorldToView(light.positionWS[1].xyz, true, 1), light.radius * light.radius);
#endif
	}

	GroupMemoryBarrierWithGroupSync();

	if (any(dispatchThreadId >= uint3(ClusterSize.x, ClusterSize.y, ClusterSize.z)))
		return;

	uint visibleLightCount = 0;

	uint clusterIndex = dispatchThreadId.x +
	                    dispatchThreadId.y * ClusterSize.x +
	                    dispatchThreadId.z * (ClusterSize.x * ClusterSize.y);

	uint offset = clusterIndex * MAX_CLUSTER_LIGHTS;
	ClusterAABB cluster = clusters[clusterIndex];

	// Pack only view-space bounds; staging full Light structs exceeds the 32 KB LDS limit.
	for (uint i = 0; i < LightCount; i++) {
		float4 lightVS = sharedLightsVS[i];

#if defined(VR)
		float4 lightVSRight = sharedLightsVSRight[i];
		[branch] if (LightIntersectsCluster(lightVS.xyz, lightVS.w, cluster) || LightIntersectsCluster(lightVSRight.xyz, lightVSRight.w, cluster))
#else
		[branch] if (LightIntersectsCluster(lightVS.xyz, lightVS.w, cluster))
#endif
		{
			lightIndexList[offset + visibleLightCount] = i;
			visibleLightCount++;
			if (visibleLightCount >= MAX_CLUSTER_LIGHTS)
				break;
		}
	}

	LightGrid output = {
		offset, visibleLightCount, 0, 0
	};

	lightGrid[clusterIndex] = output;
}

//https://www.3dgep.com/forward-plus/#Grid_Frustums_Compute_Shader
/*
function CullLights( L, G, I )
    Input: A set L of n lights.
    Input: A 2D grid G of index offset and count in the global light index list.
    Input: A list I of global light index list.
    Output: A 2D grid G with the current tiles offset and light count.
    Output: A list I with the current tiles overlapping light indices appended to it.

1.  let t be the index of the current tile  ; t is the 2D index of the tile.
2.  let i be I(t * MAX_CLUSTER_LIGHTS)      ; The tile's output light list.
3.  let f <- Frustum(t)                     ; f is the frustum for the current tile.

4.  for l in L                      ; Iterate the lights in the light list.
5.      if Cull( l, f )             ; Cull the light against the tile frustum.
6.          AppendLight( l, i )     ; Append the light to the tile's output list.

7.  o <- t * MAX_CLUSTER_LIGHTS    ; The tile's fixed output offset.

8.  G(t) <- ( o, i.count )          ; Store the offset and light count in the light grid.

*/
