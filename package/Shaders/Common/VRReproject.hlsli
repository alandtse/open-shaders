#ifndef __VR_REPROJECT_HLSL__
#define __VR_REPROJECT_HLSL__

#include "Common/VR.hlsli"

namespace Stereo
{

	/**
	* @brief Returns the maximum absolute depth difference between a center depth and four neighbors.
	*
	* Used for depth-discontinuity edge detection in stereo sync passes.
	* Works with both NDC depths (fixed absolute threshold) and linear view-space depths
	* (relative threshold: divide result by max(center, 1.0)).
	*
	* @param[in] center    Depth at the pixel being tested.
	* @param[in] neighbors Depths at four neighboring pixels (e.g. ±1 or ±2 cross pattern).
	* @return Maximum of |center - neighbor| across all four samples.
	*/
	float MaxDepthDiff(float center, float4 neighbors)
	{
		return max(max(abs(center - neighbors.x), abs(center - neighbors.y)),
			max(abs(center - neighbors.z), abs(center - neighbors.w)));
	}

#if defined(PSHADER) || defined(FRAMEBUFFER)

	/**
	* @brief Checks if the color is non zero by testing if the color is greater than 0 by epsilon.
	*
	* This function check is a color is non black. It uses a small epsilon value to allow for
	* floating point imprecision.
	*
	* For screen-space reflection (SSR), this acts as a mask and checks for an invalid reflection by
	* checking if the reflection color is essentially black (close to zero).
	*
	* @param[in] ssrColor The color to check.
	* @param[in] epsilon Small tolerance value used to determine if the color is close to zero.
	* @return True if color is non zero, otherwise false.
	*/
	bool IsNonZeroColor(float4 ssrColor, float epsilon = 0.001)
	{
		return dot(ssrColor.xyz, ssrColor.xyz) > epsilon * epsilon;
	}

#	ifdef VR
	/**
	* @brief Converts mono UV coordinates from one eye to the corresponding mono UV coordinates of the other eye.
	*
	* This function is used to transition UV coordinates from one eye's perspective to the other eye in a stereo rendering setup.
	* It operates by converting the mono UV to clip space, transforming it into world space, and then reprojecting it
	* into the other eye's clip space before converting back to UV coordinates. It supports dynamic resolution adjustments
	* and applies eye offset adjustments for correct stereo separation.
	*
	* The function considers the aspect of VR by modifying the NDC to view space conversion based on the stereo setup,
	* ensuring accurate rendering across both eyes.
	*
	* @param[in] monoUV The UV coordinates and depth value (Z component) for the current eye, in the range [0,1].
	* @param[in] eyeIndex Index of the source/current eye (0 for left, 1 for right).
	* @param[in] dynamicres Optional flag indicating whether dynamic resolution is applied. Default is false.
	* @return UV coordinates adjusted to the other eye, with depth.
	*/
	float3 ConvertMonoUVToOtherEye(float3 monoUV, uint eyeIndex, bool dynamicres = false)
	{
		// Convert from dynamic res to true UV space if necessary
		if (dynamicres)
			monoUV.xy *= FrameBuffer::DynamicResolutionParams2.xy;

		// Convert UV to Clip Space
		float4 clipPos = float4(monoUV.xy * float2(2, -2) - float2(1, -1), monoUV.z, 1);

		// Convert Clip Space to World Space for the current eye
		float4 worldPos = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], clipPos);
		worldPos /= worldPos.w;

		// Apply eye offset adjustment in world space
		worldPos.xyz += FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[1 - eyeIndex].xyz;

		// Convert World Space to Clip Space for the other eye
		float4 clipPosOtherEye = mul(FrameBuffer::CameraViewProj[1 - eyeIndex], worldPos);
		clipPosOtherEye /= clipPosOtherEye.w;

		// Convert Clip Space to UV (Y is flipped: clip +1 = top, UV 0 = top)
		float3 monoUVOtherEye = float3(clipPosOtherEye.xy * float2(0.5f, -0.5f) + 0.5f, clipPosOtherEye.z);

		// Convert back to dynamic res space if necessary
		if (dynamicres)
			monoUVOtherEye.xy *= FrameBuffer::DynamicResolutionParams1.xy;

		return monoUVOtherEye;
	}
#	endif  // VR

	/**
	* @brief Resolves a mono UV to the eye that can see it, crossing to the other eye if needed.
	*
	* When a screen-space ray or sample position leaves the current eye's screen bounds,
	* this function tries to find the corresponding location in the other eye via
	* ConvertMonoUVToOtherEye.  On flat (non-VR) this is a no-op: sampleUV and
	* sampleEyeIndex are set to the input values unchanged.
	*
	* Based on concepts from https://cuteloong.github.io/publications/scssr24/
	* Wu, X., Xu, Y., & Wang, L. (2024). Stereo-consistent Screen Space Reflection. Computer Graphics Forum, 43(4).
	*
	* @param[in]    monoUV          Mono UV coordinates with depth in Z, [0-1]. Must not be dynamic resolution adjusted.
	* @param[in]    eyeIndex        Index of the originating eye (0 or 1).
	* @param[out]   sampleUV        Mono UV that should be used for sampling (may be in the other eye).
	* @param[out]   sampleEyeIndex  Eye index that owns sampleUV.
	*/
	void ResolveMonoUVForEye(float3 monoUV, uint eyeIndex, out float2 sampleUV, out uint sampleEyeIndex)
	{
		sampleUV = monoUV.xy;
		sampleEyeIndex = eyeIndex;
#	ifdef VR
		if (FrameBuffer::IsOutsideFrame(monoUV.xy, false)) {
			float3 otherEyeUV = ConvertMonoUVToOtherEye(monoUV, eyeIndex);
			if (!FrameBuffer::IsOutsideFrame(otherEyeUV.xy, false)) {
				sampleUV = otherEyeUV.xy;
				sampleEyeIndex = 1 - eyeIndex;
			}
		}
#	endif
	}

#	ifdef VR
	/**
	* @brief Adjusts UV coordinates for VR stereo rendering when transitioning between eyes or handling boundary conditions.
	*
	* This function is used in raymarching to check the next UV coordinate. It checks if the current UV coordinates are outside
	* the frame. If so, it transitions the UV coordinates to the other eye and adjusts them if they are within the frame of the other eye.
	* If the UV coordinates are outside the frame of both eyes, it returns the adjusted UV coordinates for the current eye.
	*
	* The function ensures that the UV coordinates are correctly adjusted for stereo rendering, taking into account boundary conditions
	* and preserving accurate reflections.
	* Based on concepts from https://cuteloong.github.io/publications/scssr24/
	* Wu, X., Xu, Y., & Wang, L. (2024). Stereo-consistent Screen Space Reflection. Computer Graphics Forum, 43(4).
	*
	* We do not have a backface depth so we may be ray marching even though the ray is in an object.

	* @param[in] monoUV Current UV coordinates with depth information, [0-1]. Must not be dynamic resolution adjusted.
	* @param[in] eyeIndex Index of the current eye (0 or 1).
	* @param[out] fromOtherEye Boolean indicating if the result UV coordinates are from the other eye.
	*
	* @return Adjusted stereo UV coordinates for rendering, [0-1]. Must be dynamic resolution adjusted later.
	*/
	float3 ConvertStereoRayMarchUV(float3 monoUV, uint eyeIndex, out bool fromOtherEye)
	{
		float2 resolvedUV;
		uint resolvedEye;
		ResolveMonoUVForEye(monoUV, eyeIndex, resolvedUV, resolvedEye);
		fromOtherEye = (resolvedEye != eyeIndex);
		return ConvertToStereoUV(float3(resolvedUV, monoUV.z), resolvedEye);
	}

	/**
	* @brief Converts stereo UV coordinates from one eye to the corresponding stereo UV coordinates of the other eye.
	*
	* This function is used to transition UV coordinates from one eye's perspective to the other eye in a stereo rendering setup.
	* It works by converting the stereo UV to mono UV, then to clip space, transforming it into view space, and then reprojecting it into the other eye's
	* clip space before converting back to stereo UV coordinates. It also supports dynamic resolution.
	*
	* @param[in] stereoUV The UV coordinates and depth value (Z component) for the current eye, in the range [0,1].
	* @param[in] eyeIndex Index of the current eye (0 or 1).
	* @param[in] dynamicres Optional flag indicating whether dynamic resolution is applied. Default is false.
	* @return UV coordinates adjusted to the other eye, with depth.
	*/
	float3 ConvertStereoUVToOtherEyeStereoUV(float3 stereoUV, uint eyeIndex, bool dynamicres = false)
	{
		// Convert from dynamic res to true UV space
		if (dynamicres)
			stereoUV.xy *= FrameBuffer::DynamicResolutionParams2.xy;

		stereoUV.xy = ConvertFromStereoUV(stereoUV.xy, eyeIndex);
		stereoUV.xyz = ConvertMonoUVToOtherEye(stereoUV.xyz, eyeIndex);
		stereoUV.xy = ConvertToStereoUV(stereoUV.xy, 1 - eyeIndex);

		// Convert back to dynamic res space if necessary
		if (dynamicres)
			stereoUV.xy *= FrameBuffer::DynamicResolutionParams1.xy;
		return stereoUV;
	}

	/**
	* @brief Returns a smooth fade factor for UVs near the edge of the frame.
	*
	* This helps avoid abrupt transitions when one eye's SSGI is out of frame or occluded.
	* Fade width is tunable; 0.02 is 2% of the frame.
	*/
	float IsOutsideFrameFade(float2 uv, bool dynamicres = false)
	{
		float2 max = dynamicres ? FrameBuffer::DynamicResolutionParams1.xy : float2(1, 1);
		float2 min = float2(0, 0);
		float fadeWidth = 0.02;
		float edgeFade = 1.0;
		edgeFade *= smoothstep(min.x, min.x + fadeWidth, uv.x);
		edgeFade *= smoothstep(max.x, max.x - fadeWidth, uv.x);
		edgeFade *= smoothstep(min.y, min.y + fadeWidth, uv.y);
		edgeFade *= smoothstep(max.y, max.y - fadeWidth, uv.y);
		return edgeFade;
	}

	/**
	* @brief Blends color data from two eyes based on their UV coordinates and validity.
	*
	* This function checks the validity of the colors based on their UV coordinates and
	* alpha values. It blends the colors while ensuring proper handling of transparency.
	* If one eye sees the first person model (depth < VR_FP_Z) and the other sees world geometry (depth > VR_FP_Z),
	* the first person model's color is dropped from the blend to avoid outlines.
	*
	* @param uv1 UV coordinates for the first eye.
	* @param color1 Color from the first eye.
	* @param uv2 UV coordinates for the second eye.
	* @param color2 Color from the second eye.
	* @param dynamicres Whether the uvs have dynamic resolution applied
	* @return Blended color, including the maximum alpha from both inputs.
	*/
	float4 BlendEyeColors(
		float3 uv1,
		float4 color1,
		float3 uv2,
		float4 color2,
		bool dynamicres = false)
	{
		// Use smooth fade at edge for each eye
		float fade1 = IsOutsideFrameFade(uv1.xy, dynamicres);
		float fade2 = IsOutsideFrameFade(uv2.xy, dynamicres);

		// Stereo-consistent edge fade: use maximum fade so either eye can keep color if in bounds
		float edgeFade = max(fade1, fade2);

		// Occlusion-aware confidence based on depth difference
		float depthDiff = abs(uv1.z - uv2.z);
		float confidence = 1.0 - smoothstep(0.01, 0.05, depthDiff);

		// Soft first person model mask: fade out FP model near threshold
		float fp_fade1 = 1.0 - smoothstep(VR_FP_Z - 1.0, VR_FP_Z + 1.0, uv1.z);  // fades from 1 to 0 as depth crosses VR_FP_Z
		float fp_fade2 = 1.0 - smoothstep(VR_FP_Z - 1.0, VR_FP_Z + 1.0, uv2.z);

		// If one eye is world and the other is FP, fade out FP smoothly
		bool eye1_is_fp = uv1.z < VR_FP_Z;
		bool eye2_is_fp = uv2.z < VR_FP_Z;
		bool eyes_disagree = eye1_is_fp != eye2_is_fp;
		if (eyes_disagree) {
			if (eye1_is_fp)
				fade1 *= fp_fade1;
			if (eye2_is_fp)
				fade2 *= fp_fade2;
		}

		fade1 *= confidence * edgeFade;
		fade2 *= confidence * edgeFade;

		float totalFade = fade1 + fade2 + 1e-5;
		float4 blendedColor = (color1 * fade1 + color2 * fade2) / totalFade;
		blendedColor.a = max(color1.a * fade1, color2.a * fade2);
		return blendedColor;
	}

	float4 BlendEyeColors(float2 uv1, float4 color1, float2 uv2, float4 color2, bool dynamicres = false)
	{
		return BlendEyeColors(float3(uv1, 0), color1, float3(uv2, 0), color2, dynamicres);
	}

	/**
	* @brief Result of a stereo bilateral reprojection: other-eye pixel coords and blend weight.
	*/
	struct StereoBilateralResult
	{
		float2 otherStereoUV;  ///< Stereo UV in the other eye
		int2 otherPx;          ///< Pixel coordinate in the other eye
		float blendWeight;     ///< [0, maxBlend] bilateral blend weight
		bool valid;            ///< True if reprojection succeeded
		bool backCheckPassed;  ///< True if round-trip reprojection validated
		uint skipReason;       ///< 0 = blended, 1 = source edge, 2 = dest edge/mask, 3 = out of bounds
	};

	/**
	* @brief Reprojects a pixel to the other eye and computes pixel coordinates.
	*
	* First stage of the stereo bilateral filter from Shi, Billeter, Eisemann 2022.
	* Returns the other-eye pixel location; the caller must sample depth at that
	* location and call FinalizeStereoBlend to complete the bilateral weight.
	*
	* @param[in] stereoUV     Stereo UV of the source pixel [0,1]
	* @param[in] depth        Depth at the source pixel
	* @param[in] eyeIndex     Eye index of the source pixel (0 or 1)
	* @param[in] frameDim     Dimensions of the buffer (for pixel coord conversion)
	* @return StereoBilateralResult with valid=false if reprojection is out of bounds.
	*/
	StereoBilateralResult ReprojectToOtherEye(
		float2 stereoUV,
		float depth,
		uint eyeIndex,
		float2 frameDim)
	{
		StereoBilateralResult result;
		result.otherStereoUV = 0;
		result.otherPx = int2(0, 0);
		result.blendWeight = 0;
		result.valid = false;
		result.backCheckPassed = false;
		result.skipReason = 0;

		uint otherEyeIndex = 1 - eyeIndex;

		float2 monoUV = ConvertFromStereoUV(stereoUV, eyeIndex);
		float3 otherEyeUV = ConvertMonoUVToOtherEye(float3(monoUV, depth), eyeIndex);

		if (FrameBuffer::IsOutsideFrame(otherEyeUV.xy, false)) {
			result.skipReason = 3;  // out of bounds
			return result;
		}

		result.otherStereoUV = ConvertToStereoUV(otherEyeUV.xy, otherEyeIndex);
		result.otherPx = clamp(int2(result.otherStereoUV * frameDim), int2(0, 0), int2(frameDim) - 1);
		result.valid = true;
		return result;
	}

	/**
	* @brief Canonical view-independent reprojection test: can this pixel take
	* the other eye's value exactly?
	*
	* True only when the reprojection landed in-frame (result.valid) and the other eye
	* shows the same surface (depths agree within agreeThreshold). Consumers use the one
	* test both to skip the eye's own compute and to gate the transfer in the reproject
	* pass, so the hit/miss decisions cannot drift. Depths must be in the same comparable
	* space (raw/NDC); the caller converts and applies mask/sky rejection first.
	*/
	bool IsReprojectionExact(StereoBilateralResult result, float depth, float otherEyeDepth, float agreeThreshold)
	{
		return result.valid && (abs(otherEyeDepth - depth) <= agreeThreshold);
	}

	/**
	* @brief Computes bilateral blend weight with depth comparison and back-check.
	*
	* Second stage of the stereo bilateral filter from Shi, Billeter, Eisemann 2022.
	* Compares the sampled depth at the other eye's pixel against the expected depth,
	* and performs the back-check (round-trip reprojection validation).
	*
	* @param[in,out] result   Result from ReprojectToOtherEye; blendWeight and backCheckPassed are filled in.
	* @param[in] stereoUV     Stereo UV of the source pixel (same as passed to ReprojectToOtherEye)
	* @param[in] depth        Depth at the source pixel
	* @param[in] otherEyeDepth  Actual depth sampled at the other eye's pixel
	* @param[in] eyeIndex     Eye index of the source pixel
	* @param[in] frameDim     Dimensions of the buffer
	* @param[in] depthSigma   Gaussian sigma for bilateral depth weight
	* @param[in] maxBlend     Maximum blend factor
	* @param[in] backCheckThreshold  Max pixel distance for back-check (0 to disable). Default 8.0.
	*/
	void FinalizeStereoBlend(
		inout StereoBilateralResult result,
		float2 stereoUV,
		float depth,
		float otherEyeDepth,
		uint eyeIndex,
		float2 frameDim,
		float depthSigma,
		float maxBlend,
		float backCheckThreshold = 8.0)
	{
		// Bilateral weight: compare sampled depth at other eye against source depth
		float depthDiff = abs(depth - otherEyeDepth);
		float depthWeight = exp(-depthDiff * depthDiff / (depthSigma * depthSigma + 1e-8));

		// Back-check: reproject Q (in eye B) back to eye A and verify round-trip.
		// Two VP matrix multiplications accumulate float32 error (~3-5px at medium range),
		// so the threshold must be generous enough to pass valid surfaces while catching
		// true occlusion discontinuities (which produce errors of tens to hundreds of pixels).
		uint otherEyeIndex = 1 - eyeIndex;
		result.backCheckPassed = true;
		if (backCheckThreshold > 0) {
			float2 otherMonoUV = ConvertFromStereoUV(result.otherStereoUV, otherEyeIndex);
			float3 roundTripUV = ConvertMonoUVToOtherEye(float3(otherMonoUV, otherEyeDepth), otherEyeIndex);
			float2 roundTripStereoUV = ConvertToStereoUV(roundTripUV.xy, eyeIndex);
			float2 pixelDist = abs(roundTripStereoUV * frameDim - (stereoUV * frameDim));
			// Use max component so a large error in either axis triggers the check
			result.backCheckPassed = max(pixelDist.x, pixelDist.y) < backCheckThreshold;
			if (!result.backCheckPassed)
				depthWeight *= 0.1;  // Heavily penalize but don't fully reject
		}

		result.blendWeight = depthWeight * maxBlend;
	}

	/**
	* @brief Tunable parameters shared by the stereo-sync gated bilateral ladder.
	*
	* Bundles the per-pass constants so StereoSyncReproject/StereoSyncWeight present
	* one consistent interface. Depths are NDC; edge tests are absolute-NDC.
	*/
	struct StereoSyncParams
	{
		float edgeThreshold;       ///< NDC depth-diff above which a pixel is a discontinuity (skip)
		float depthSigma;          ///< Gaussian sigma for the bilateral depth weight
		float maxBlend;            ///< Cap on the cross-eye blend weight
		float backCheckThreshold;  ///< Round-trip validation distance in pixels (0 = off)
		float maskEpsilon;         ///< Other-eye neighbor depth below this = HMD mask boundary (reject)
	};

	/**
	* @brief Source-edge gate + reproject: stage one of the unified stereo-sync ladder.
	*
	* Rejects depth discontinuities at the source pixel, then reprojects to the other
	* eye. Sets skipReason (1 = source edge, 3 = out of bounds) so callers can drive
	* debug visualization. The caller samples srcNeighbors in its own resolution/space.
	*
	* @param[in] stereoUV       Stereo UV of the source pixel [0,1].
	* @param[in] centerDepthNDC NDC depth at the source pixel.
	* @param[in] srcNeighbors   NDC depths of the four cross neighbors (for edge detection).
	* @param[in] eyeIndex       Eye index of the source pixel (0 or 1).
	* @param[in] frameDim       Stereo buffer dimensions.
	* @param[in] p              Shared sync parameters.
	* @return Reprojection result; valid=false when the pixel is an edge or out of bounds.
	*/
	StereoBilateralResult StereoSyncReproject(
		float2 stereoUV,
		float centerDepthNDC,
		float4 srcNeighbors,
		uint eyeIndex,
		float2 frameDim,
		StereoSyncParams p)
	{
		StereoBilateralResult r = (StereoBilateralResult)0;
		if (MaxDepthDiff(centerDepthNDC, srcNeighbors) > p.edgeThreshold) {
			r.skipReason = 1;  // source edge
			return r;
		}
		return ReprojectToOtherEye(stereoUV, centerDepthNDC, eyeIndex, frameDim);
	}

	/**
	* @brief Destination-edge/mask gate + bilateral weight: stage two of the ladder.
	*
	* Rejects when the reprojected pixel lands on the HMD mask boundary or a depth
	* discontinuity in the other eye; otherwise computes the bilateral blend weight
	* (with optional back-check). On rejection sets blendWeight=0 and skipReason=2 so
	* the caller's blend collapses to a passthrough.
	*
	* @param[in,out] r           Result from StereoSyncReproject (must be valid).
	* @param[in] stereoUV        Stereo UV of the source pixel (same as stage one).
	* @param[in] centerDepthNDC  NDC depth at the source pixel.
	* @param[in] otherDepthNDC   NDC depth sampled at the reprojected pixel.
	* @param[in] otherNeighbors  NDC depths of the reprojected pixel's four cross neighbors.
	* @param[in] eyeIndex        Eye index of the source pixel.
	* @param[in] frameDim        Stereo buffer dimensions.
	* @param[in] p               Shared sync parameters.
	*/
	void StereoSyncWeight(
		inout StereoBilateralResult r,
		float2 stereoUV,
		float centerDepthNDC,
		float otherDepthNDC,
		float4 otherNeighbors,
		uint eyeIndex,
		float2 frameDim,
		StereoSyncParams p)
	{
		if (any(otherNeighbors < p.maskEpsilon) ||
			MaxDepthDiff(otherDepthNDC, otherNeighbors) > p.edgeThreshold) {
			r.blendWeight = 0;
			r.skipReason = 2;  // dest edge / mask
			return;
		}
		FinalizeStereoBlend(r, stereoUV, centerDepthNDC, otherDepthNDC, eyeIndex, frameDim,
			p.depthSigma, p.maxBlend, p.backCheckThreshold);
	}
#	endif  // VR
#endif      // PSHADER
}
#endif  //__VR_REPROJECT_HLSL__
