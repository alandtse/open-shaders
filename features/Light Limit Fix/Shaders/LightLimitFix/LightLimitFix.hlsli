
namespace LightLimitFix
{

#include "LightLimitFix/Common.hlsli"

	static const float DirectionalBias = 0.5f * (0.00025f) / 3.0f;

	// Shadow Radius for PCF
	static const float PCFRadius2D = 0.002;

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

	struct ShadowLightData
	{
		column_major float4x4 ShadowProj;
		column_major float4x4 InvShadowProj;
		float4 ShadowLightParam;
	};

	// t100/t101 are reserved for Grass Collision (its Collision texture binds at
	// t100, and shaders like RunGrass include both features). LLF shadow data
	// uses t102/t103 to avoid the collision; keep the C++ PSSetShaderResources
	// slots in src/Features/LightLimitFix/ShadowRenderer.cpp in sync.
	StructuredBuffer<ShadowLightData> Shadows : register(t102);
	Texture2DArray<float> ShadowMaps : register(t103);
	Texture2DArray<float> DirectionalShadowCascades : register(t99);

	// engineMaskShadow: the engine's pre-rendered 4-cascade shadow mask sample
	// at this pixel (TexShadowMaskSampler.Load(int3(Position.xy, 0)).x). LLF's
	// DirectionalShadowLightData carries only cascades 0/1 (ShadowProj[2] /
	// EndSplitDistances.xy); past EndSplitDistances.y we have no LLF data and
	// must fall through to the engine mask. Returning 1.0 there leaves distant
	// pixels fully lit -- visible as global scene brightening with shadows
	// disappearing past a depth that varies with camera position.
	float GetDirectionalShadow(float3 worldPosition, float3 worldPositionWS, float2x2 rotationMatrix, uint eyeIndex, float engineMaskShadow, bool useEngineMaskShadow)
	{
#if defined(VR)
		// VR returns the engine mask for all cascades: the LLF two-cascade PCF
		// path bands once VR drops the caster-side global rasterizer swap.
		if (useEngineMaskShadow)
			return engineMaskShadow;
#endif

		DirectionalShadowLightData shadowLightData = DirectionalShadowLights[0];

		float shadowMapDepth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(worldPosition, eyeIndex));

		// Past cascade 1 -- defer to the engine's 4-cascade mask.
		if (shadowMapDepth > shadowLightData.EndSplitDistances.y)
			return engineMaskShadow;

		// Blend from LLF PCF deep in cascade 1 toward the engine mask as we
		// approach cascade 1's far edge, avoiding a hard discontinuity at the
		// boundary where LLF stops and engine sampling takes over.
		//
		// Previous formula used `dot(worldPosition, worldPosition) /
		// EndSplitDistances.y` -- dimensionally wrong (length^2 / length)
		// AND inverted (close pixels got engineMaskShadow, far got LLF).
		// Because `worldPosition` is camera-relative in Skyrim's vertex
		// output, that produced a visible ~sqrt(EndSplitDistances.y)-radius
		// ring around the camera that moved with the player -- a clear
		// HMD-tracked artifact in VR. Switching to linear `shadowMapDepth`
		// and reversing the blend direction makes the handoff a smooth
		// world-anchored transition at the cascade boundary.
		float fadeFactor = smoothstep(shadowLightData.EndSplitDistances.y * 0.8,
			shadowLightData.EndSplitDistances.y,
			shadowMapDepth);

		// Compute cascade blend factor
		float cascadeSelect = smoothstep(shadowLightData.StartSplitDistances.y, shadowLightData.EndSplitDistances.x, shadowMapDepth);

		// Determine which cascade(s) to sample
		uint primaryCascade = cascadeSelect;
		bool needsBlending = (cascadeSelect > 0.0) && (cascadeSelect < 1.0);

		// Transform ray to light space for primary cascade
		float3 positionLS = mul(shadowLightData.ShadowProj[primaryCascade], float4(worldPositionWS, 1)).xyz;
		positionLS.z -= DirectionalBias;

		// Sample primary cascade
		float shadow = 0.0;

		[unroll] for (int i = 0; i < 8; i++)
		{
			float2 sampleOffset = mul(Random::SpiralSampleOffsets8[i], rotationMatrix);
			float2 sampleUV = positionLS.xy + sampleOffset * PCFRadius2D;
			shadow += dot(float4(DirectionalShadowCascades.GatherRed(LinearSampler, float3(saturate(sampleUV), primaryCascade)) > positionLS.z), 0.25);
		}

		shadow /= 8.0;

		// Blend with secondary cascade if needed
		[branch] if (needsBlending)
		{
			uint secondaryCascade = 1 - primaryCascade;

			positionLS = mul(shadowLightData.ShadowProj[secondaryCascade], float4(worldPositionWS, 1)).xyz;
			positionLS.z -= DirectionalBias;

			float shadowBlend = 0.0;

			[unroll] for (int i = 0; i < 8; i++)
			{
				float2 sampleOffset = mul(Random::SpiralSampleOffsets8[i], rotationMatrix);
				float2 sampleUV = positionLS.xy + sampleOffset * PCFRadius2D;
				shadowBlend += dot(float4(DirectionalShadowCascades.GatherRed(LinearSampler, float3(saturate(sampleUV), secondaryCascade)) > positionLS.z), 0.25);
			}

			shadowBlend /= 8.0;

			shadow = lerp(shadow, shadowBlend, cascadeSelect);
		}

		// Within cascade 1's far edge, blend LLF's PCF toward the engine
		// mask instead of fading to fully-lit -- avoids a hard brightness
		// discontinuity at the cascade boundary.
		shadow = lerp(shadow, engineMaskShadow, fadeFactor);

		// Focus shadows: high-resolution actor shadows the engine renders to
		// kSHADOWMAPS slices [kFocusShadowBaseSlotIndex .. +FocusShadowCount).
		// Each focus matrix projects worldPositionWS into the actor's clip
		// space; pixels outside [0,1] UV or [0,1] depth aren't covered and
		// contribute no occlusion. Combine via min() so any occluding actor
		// wins. Without this, the player's own shadow vanishes when LLF is
		// on (the cascade has it at lower resolution; focus made it visible).
		//
		// Guards:
		// - SharedData::lightLimitFixSettings.ShadowMapSlots bounds the slice
		//   index so we never sample past the texture's allocated array size
		//   (a real concern with ShadowLightCount < 8 in extended mode).
		// - focusClip.w > EPSILON_DIVISION avoids div-by-zero NaN when the focus
		//   matrix hasn't been populated yet (first frame of a scene load
		//   before the engine's RenderShadowmaps has run the focus loop).
		[unroll] for (uint fi = 0; fi < 4; fi++)
		{
			[branch] if (fi >= shadowLightData.FocusShadowCount) break;
			const uint focusSlice = 4 + fi;  // kFocusShadowBaseSlotIndex
			[branch] if (focusSlice >= SharedData::lightLimitFixSettings.ShadowMapSlots) break;
			float4 focusClip = mul(shadowLightData.FocusShadowProj[fi], float4(worldPositionWS, 1));
			[branch] if (focusClip.w <= EPSILON_DIVISION) continue;
			focusClip.xyz /= focusClip.w;
			float2 focusUV = focusClip.xy * 0.5 + 0.5;
			[branch] if (all(focusUV >= 0.0) && all(focusUV <= 1.0) && focusClip.z >= 0.0 && focusClip.z <= 1.0)
			{
				float focusDepth = focusClip.z - DirectionalBias;
				float focusVis = 0.0;
				[unroll] for (int fs = 0; fs < 8; fs++)
				{
					float2 fsOffset = mul(Random::SpiralSampleOffsets8[fs], rotationMatrix);
					float2 fsUV = focusUV + fsOffset * PCFRadius2D;
					focusVis += dot(float4(ShadowMaps.GatherRed(LinearSampler, float3(saturate(fsUV), focusSlice)) > focusDepth), 0.25);
				}
				focusVis /= 8.0;
				shadow = min(shadow, focusVis);
				// Fully occluded -- remaining focus actors can only multiply
				// by zero, so skip their 8-tap GatherRed work on this pixel.
				[branch] if (shadow <= 0.0) break;
			}
		}

		return shadow;
	}

	float GetDirectionalShadow(float3 worldPosition, float3 worldPositionWS, float2x2 rotationMatrix, uint eyeIndex, float engineMaskShadow)
	{
		return GetDirectionalShadow(worldPosition, worldPositionWS, rotationMatrix, eyeIndex, engineMaskShadow, true);
	}

	// Convenience overload for callers without TexShadowMaskSampler bound
	// (e.g. particles): route through the direct LLF sampler, no engine mask.
	float GetDirectionalShadow(float3 worldPosition, float3 worldPositionWS, float2x2 rotationMatrix, uint eyeIndex)
	{
		return GetDirectionalShadow(worldPosition, worldPositionWS, rotationMatrix, eyeIndex, 1.0, false);
	}

	float GetDirectionalShadow(float3 worldPosition, float3 worldPositionWS, float2x2 rotationMatrix)
	{
		return GetDirectionalShadow(worldPosition, worldPositionWS, rotationMatrix, 0, 1.0, false);
	}

	float SampleShadowGather(uint shadowIndex, float2 uv, float receiverDepth)
	{
		float4 samples = ShadowMaps.GatherRed(LinearSampler, float3(uv, shadowIndex));
		return dot(float4(samples > receiverDepth), 0.25);
	}

	float GetSpotlightShadow(ShadowLightData shadowLightData, uint shadowIndex, float4 positionLS, float2x2 rotationMatrix)
	{
		positionLS.xyz /= positionLS.w;
		positionLS.xy = positionLS.xy * 0.5 + 0.5;
		positionLS.z -= shadowLightData.ShadowLightParam.z;

		float shadow = 0.0;

		[unroll] for (int i = 0; i < 8; i++)
		{
			float2 sampleOffset = mul(Random::SpiralSampleOffsets8[i], rotationMatrix);
			float2 sampleUV = positionLS.xy + sampleOffset * PCFRadius2D;
			shadow += SampleShadowGather(shadowIndex, sampleUV, positionLS.z);
		}

		return shadow / 8.0;
	}

	// PCF sample around a paraboloid UV.
	//   isDualParaboloid = true  : the slice contains two stacked paraboloids
	//                              (omni: upper in y∈[0,0.5], lower in y∈[0.5,1]).
	//                              Clamp PCF samples to the originating half so we
	//                              don't bleed across the seam.
	//   isDualParaboloid = false : the slice contains a single paraboloid filling
	//                              the whole y∈[0,1] (hemi). No clamping needed —
	//                              the entire slice is valid shadow data.
	float SampleParaboloidShadow(uint shadowIndex, float2 sampleUV, float depth, float2x2 rotationMatrix, bool isDualParaboloid)
	{
		float shadow = 0.0;

		[unroll] for (int i = 0; i < 8; i++)
		{
			float2 offset = mul(Random::SpiralSampleOffsets8[i], rotationMatrix) * PCFRadius2D;
			float2 uv = sampleUV + offset;

			if (isDualParaboloid) {
				// Clamp PCF samples to the originating paraboloid half.
				uv.y = (sampleUV.y >= 0.5) ? max(uv.y, 0.5) : min(uv.y, 0.5);
			}

			shadow += SampleShadowGather(shadowIndex, uv, depth);
		}

		return shadow / 8.0;
	}

	float GetOmnidirectionalShadow(ShadowLightData shadowLightData, uint shadowIndex, float4 positionLS, float2x2 rotationMatrix)
	{
		// ShadowLightParam.x:
		//   0 = spot/frustum (handled in GetShadowLightShadow before reaching here)
		//   1 = hemisphere   — engine renders ONE paraboloid filling the slice
		//   2 = omnidirectional (dual paraboloid) — TWO paraboloids stacked in slice
		//
		// Verified against kSHADOWMAPS slice contents in RenderDoc: hemi slices show
		// a single continuous depth gradient across y=0.5 with no seam, while omni
		// slices show two distinct paraboloid renderings stacked. Treating hemi
		// like omni applies a Y-axis compression / mirror that visibly distorts
		// (the "inverted or rotated 90°" symptom).
		const bool isOmni = (shadowLightData.ShadowLightParam.x == 2);

		bool lowerHalf = positionLS.z < 0;

		// Hemi only renders the +Z paraboloid; behind the light has no shadow data.
		// Returning 1.0 (fully lit) lets the light's own attenuation handle falloff
		// for points the engine never wrote shadow data for.
		if (!isOmni && lowerHalf)
			return 1.0;

		positionLS.xyz /= positionLS.w;

		float3 posOffset = lowerHalf ? float3(0, 0, -1) : float3(0, 0, 1);
		float3 lightDirection = normalize(normalize(positionLS.xyz) + posOffset);
		float2 sampleUV = lightDirection.xy / lightDirection.z * 0.5 + 0.5;

		// Y compression only applies to omni's dual layout. Hemi fills the whole
		// slice so its sampleUV.y stays in [0, 1] directly.
		if (isOmni)
			sampleUV.y = lowerHalf ? 1.0 - 0.5 * sampleUV.y : 0.5 * sampleUV.y;

		float depth = saturate(length(positionLS.xyz) / shadowLightData.ShadowLightParam.y);
		depth -= shadowLightData.ShadowLightParam.z;

		return SampleParaboloidShadow(shadowIndex, sampleUV, depth, rotationMatrix, isOmni);
	}

	// Single-assignment of hasCoverage at function entry keeps FXC's flow
	// analyser quiet: prior versions used an early-return overflow guard
	// that wrote hasCoverage on two paths, which tripped X4000 "potentially
	// uninitialized" warnings at the post-merge point across 360 permutations
	// (both `out` and `inout` signatures hit the same false positive).
	//
	// Overflow handling: `shadowIndex >= ShadowMapSlots` can occur transiently
	// when a light was promoted to shadow on a frame where the texture-array
	// allocation hadn't extended to cover it yet. StructuredBuffer reads beyond
	// declared bounds return zero per the D3D11 spec, so the Shadows[shadowIndex]
	// read is safe -- it falls into the `ShadowLightParam.y == 0` branch below
	// and returns 1.0. The `hasCoverage` flag tells the caller whether the
	// sample was real, so suppression still works correctly upstream.
	float GetShadowLightShadow(uint shadowIndex, float3 worldPositionWS, float2x2 rotationMatrix, out bool hasCoverage)
	{
		hasCoverage = shadowIndex < SharedData::lightLimitFixSettings.ShadowMapSlots;

		ShadowLightData shadowLightData = Shadows[shadowIndex];

		[flatten] if (shadowLightData.ShadowLightParam.y == 0) return 1.0;
		[flatten] if (shadowLightData.ShadowLightParam.y < 0) return 0.0;

		float4 positionLS = mul(shadowLightData.ShadowProj, float4(worldPositionWS, 1));

		[branch] if (shadowLightData.ShadowLightParam.x == 0)
		{
			float shadowBaseVisibility = GetSpotlightShadow(shadowLightData, shadowIndex, positionLS, rotationMatrix);
			positionLS.xyz /= positionLS.w;

			float spotFalloff = saturate(1.0 - dot(positionLS.xy, positionLS.xy));

			return shadowBaseVisibility * spotFalloff;
		}

		return GetOmnidirectionalShadow(shadowLightData, shadowIndex, positionLS, rotationMatrix);
	}
}
