
namespace LightLimitFix
{

#include "LightLimitFix/Common.hlsli"

	cbuffer StrictLightData : register(b3)
	{
		uint NumStrictLights;
		int RoomIndex;
		uint ShadowBitMask;
		uint pad0;
		Light StrictLights[15];
	};

	StructuredBuffer<Light> lights : register(t35);
	StructuredBuffer<uint> lightList : register(t36);       //MAX_CLUSTER_LIGHTS * 16^3
	StructuredBuffer<LightGrid> lightGrid : register(t37);  //16^3

	bool GetClusterIndex(in float2 uv, in float z, inout uint clusterIndex)
	{
		const uint3 clusterSize = SharedData::lightLimitFixSettings.ClusterSize.xyz;

		if (!FrameBuffer::FrameParams.y)  // Fix first person lights
			uv = 0.5;

		z = max(z, SharedData::CameraData.y);

		uint clusterZ = log(z / SharedData::CameraData.y) * clusterSize.z / log(SharedData::CameraData.x / SharedData::CameraData.y);
		uint3 cluster = uint3(uint2(uv * clusterSize.xy), clusterZ);

		// Bounds validation to prevent out-of-range cluster indices
		if (any(cluster >= clusterSize))
			return false;

		clusterIndex = cluster.x + (clusterSize.x * cluster.y) + (clusterSize.x * clusterSize.y * cluster.z);
		return true;
	}

	bool IsSaturated(float value)
	{
		return value == saturate(value);
	}

	bool IsSaturated(float2 value)
	{
		return IsSaturated(value.x) && IsSaturated(value.y);
	}

	// Chooses the contact-shadow noise sample coordinate. In VR we derive it
	// from screenUV (which FrameBuffer::ViewToUV already returns per-eye via
	// CameraProj[eye]) so both eyes sample the same noise pattern at the same
	// world position — using the raw rasterized pixel position in VR makes
	// each eye hash a different value, producing per-eye jitter that reads as
	// flicker on contact-shadow recipients.
	//
	// BufferDim.x is the full packed stereo width (State::UpdateSharedData
	// reads it from the kMAIN texture, which spans both eyes side-by-side),
	// so we halve X in VR to match the per-eye pixel grid. Without the
	// halving, the per-eye sample steps by ~2 pixels in X — still stereo-
	// consistent, but at half the effective noise resolution. Flat keeps the
	// raw pixel position to match the original implementation byte-for-byte.
	float2 GetContactShadowNoiseCoord(float2 screenPosition, float2 screenUV)
	{
#if defined(VR)
		return screenUV * float2(SharedData::BufferDim.x * 0.5, SharedData::BufferDim.y);
#else
		return screenPosition;
#endif
	}

	float ContactShadows(float3 viewPosition, float noise2D, float3 lightDirectionVS, uint contactShadowSteps, uint a_eyeIndex = 0)
	{
		if (contactShadowSteps == 0)
			return 1.0;

		float2 depthDeltaMult = float2(0.20, 0.05);

		// Extend contact shadow distance
		lightDirectionVS *= 2.0;

		// Offset starting position with interleaved gradient noise
		viewPosition += lightDirectionVS * noise2D;

		// Accumulate samples
		float contactShadow = 0.0;
		for (uint i = 0; i < contactShadowSteps; i++) {
			// Step the ray
			viewPosition += lightDirectionVS;

			float2 rayUV = FrameBuffer::ViewToUV(viewPosition, true, a_eyeIndex);

			// Ensure the UV coordinates are inside the screen
			if (!IsSaturated(rayUV))
				break;

			// Compute the difference between the ray's and the camera's depth
			float rayDepth = SharedData::GetScreenDepth(rayUV, a_eyeIndex);

			// Difference between the current ray distance and the marched light
			float depthDelta = viewPosition.z - rayDepth;
			if (rayDepth > 16.5)  // First person
				contactShadow = max(contactShadow, saturate(depthDelta * depthDeltaMult.x) - saturate(depthDelta * depthDeltaMult.y));
			if (contactShadow == 1.0)
				break;
		}

		return 1.0 - saturate(contactShadow);
	}

	bool IsLightIgnored(Light light)
	{
		if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
			return !(ShadowBitMask & (1 << light.shadowLightIndex));
		}

		bool lightIgnored = false;
		if ((light.lightFlags & LightFlags::PortalStrict) && RoomIndex >= 0) {
			lightIgnored = true;
			int roomIndex = RoomIndex;
			[unroll] for (int flagsIndex = 0; flagsIndex < 4; ++flagsIndex)
			{
				if (roomIndex < 32) {
					if (((light.roomFlags[flagsIndex] >> roomIndex) & 1) == 1) {
						lightIgnored = false;
					}
					break;
				}
				roomIndex -= 32;
			}
		}
		return lightIgnored;
	}
}
