
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

	// Per-eye stereo-stable IGN coord. In VR we use screenUV (per-eye via
	// CameraProj[eye]) instead of SV_Position so both eyes hash the same
	// value at the same world pixel — SV_Position differs between eyes in
	// a packed stereo buffer, producing per-eye jitter that reads as flicker
	// on contact-shadow recipients.
	//
	// BufferDim.x is the full packed stereo width (kMAIN spans both eyes
	// side-by-side), so the 0.5 factor lands us on the per-eye integer
	// pixel grid — same IGN frequency as flat mode for a buffer sized
	// (BufferDim.x/2, BufferDim.y). Do not drop the 0.5: that over-samples
	// IGN by ~2x in X, giving a higher-frequency noise pattern, not lower.
	float2 GetContactShadowNoiseCoord(float2 screenPosition, float2 screenUV)
	{
#if defined(VR)
		return screenUV * float2(SharedData::BufferDim.x * 0.5, SharedData::BufferDim.y);
#else
		return screenPosition;
#endif
	}

	// Skyrim's first-person viewmodel renders in a compressed depth range below this
	// linearized value; reject occluders there since the viewmodel isn't in the world.
	static const float CONTACT_SHADOW_FIRST_PERSON_MAX_DEPTH = 16.5;

	// Reference view-space depth for perspective-correct stride. At/below this depth,
	// stride matches its prior view-space meaning; beyond it, stride and the depth-delta
	// band scale linearly with depth so each step covers ~constant screen-space distance
	// and the shadow-thickness band tracks the same screen-space extent.
	static const float CONTACT_SHADOW_REFERENCE_DEPTH = 100.0;

	float ContactShadows(float3 viewPosition, float noise2D, float3 lightDirectionVS, uint contactShadowSteps, uint a_eyeIndex = 0)
	{
		if (contactShadowSteps == 0)
			return 1.0;

		// Perspective-correct stride: scale view-space step length with depth so each step
		// covers ~constant screen-space distance. Inverse-scale the thickness/fade band so
		// the depth-delta window tracks the same screen-space extent across depths.
		float perspectiveScale = max(viewPosition.z, CONTACT_SHADOW_REFERENCE_DEPTH) / CONTACT_SHADOW_REFERENCE_DEPTH;
		float depthDeltaThickness = SharedData::lightLimitFixSettings.ContactShadowThickness / perspectiveScale;
		float depthDeltaFade = SharedData::lightLimitFixSettings.ContactShadowDepthFade / perspectiveScale;
		lightDirectionVS *= SharedData::lightLimitFixSettings.ContactShadowStride * perspectiveScale;

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
			if (rayDepth > CONTACT_SHADOW_FIRST_PERSON_MAX_DEPTH)
				contactShadow = max(contactShadow, saturate(depthDelta * depthDeltaThickness) - saturate(depthDelta * depthDeltaFade));
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
