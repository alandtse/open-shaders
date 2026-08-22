#ifndef __VR_DEPENDENCY_HLSL__
#define __VR_DEPENDENCY_HLSL__
#ifdef VR

// First person model depth threshold for VR occlusion logic
#	ifndef VR_FP_Z
#		define VR_FP_Z 18.0
#	endif

#	if defined(VSHADER)
#		include "Common/Math.hlsli"
#	endif  // VSHADER

#	if (!defined(COMPUTESHADER) && !defined(CSHADER)) || defined(FRAMEBUFFER)
#		include "Common/FrameBuffer.hlsli"
#	endif
cbuffer VRValues : register(b13)
{
	float AlphaTestRefRS : packoffset(c0);
	float StereoEnabled : packoffset(c0.y);
	float2 EyeOffsetScale : packoffset(c0.z);
	float4 EyeClipEdge[2] : packoffset(c1);
}
#endif

namespace Stereo
{
	/**
	Converts to the eye specific uv [0,1].
	In VR, texture buffers include the left and right eye in the same buffer. Flat
	only has a single camera for the entire width. This means the x value [0, .5]
	represents the left eye, and the x value (.5, 1] are the right eye. This returns
	the adjusted value
	@param uv - uv coords [0,1] to be encoded for VR
	@param a_eyeIndex The eyeIndex; 0 is left, 1 is right
	@param a_invertY Whether to invert the Y direction
	@returns uv with x coords adjusted for the VR texture buffer
	*/
	float2 ConvertToStereoUV(float2 uv, uint a_eyeIndex, uint a_invertY = 0)
	{
#ifdef VR
		// convert [0,1] to eye specific [0,.5] and [.5, 1] dependent on a_eyeIndex
		uv.x = saturate(uv.x);
		uv.x = (uv.x + (float)a_eyeIndex) / 2;
		if (a_invertY)
			uv.y = 1 - uv.y;
#endif
		return uv;
	}

	float3 ConvertToStereoUV(float3 uv, uint a_eyeIndex, uint a_invertY = 0)
	{
		uv.xy = ConvertToStereoUV(uv.xy, a_eyeIndex, a_invertY);
		return uv;
	}

	float4 ConvertToStereoUV(float4 uv, uint a_eyeIndex, uint a_invertY = 0)
	{
		uv.xy = ConvertToStereoUV(uv.xy, a_eyeIndex, a_invertY);
		return uv;
	}

	/**
	Converts from eye specific uv to general uv [0,1].
	In VR, texture buffers include the left and right eye in the same buffer.
	This means the x value [0, .5] represents the left eye, and the x value (.5, 1] are the right eye.
	This returns the adjusted value
	@param uv - eye specific uv coords [0,1]; if uv.x < 0.5, it's a left eye; otherwise right
	@param a_eyeIndex The eyeIndex; 0 is left, 1 is right
	@param a_invertY Whether to invert the Y direction
	@returns uv with x coords adjusted to full range for either left or right eye
	*/
	float2 ConvertFromStereoUV(float2 uv, uint a_eyeIndex, uint a_invertY = 0)
	{
#ifdef VR
		// convert [0,.5] to [0, 1] and [.5, 1] to [0,1]
		uv.x = 2 * uv.x - (float)a_eyeIndex;
		if (a_invertY)
			uv.y = 1 - uv.y;
#endif
		return uv;
	}

	float3 ConvertFromStereoUV(float3 uv, uint a_eyeIndex, uint a_invertY = 0)
	{
		uv.xy = ConvertFromStereoUV(uv.xy, a_eyeIndex, a_invertY);
		return uv;
	}

	float4 ConvertFromStereoUV(float4 uv, uint a_eyeIndex, uint a_invertY = 0)
	{
		uv.xy = ConvertFromStereoUV(uv.xy, a_eyeIndex, a_invertY);
		return uv;
	}

	/**
	Gets the eyeIndex for Compute Shaders
	@param texCoord Texcoord on the screen [0,1]
	@returns eyeIndex (0 left, 1 right)
	*/
	uint GetEyeIndexFromTexCoord(float2 texCoord)
	{
#ifdef VR
		return (texCoord.x >= 0.5) ? 1 : 0;
#endif  // VR
		return 0;
	}

	/** Returns the eye index for a pixel in a packed stereo texture. */
	uint GetEyeIndexFromPixel(uint2 pixel, uint2 frameDim)
	{
#ifdef VR
		return pixel.x >= (frameDim.x >> 1) ? 1 : 0;
#endif
		return 0;
	}

	struct EyeUV
	{
		uint index;
		float2 uv;
	};

	/**
	* @brief Unpacks a packed side-by-side texcoord into its eye index and per-eye mono UV.
	*
	* Shorthand for the GetEyeIndexFromTexCoord + ConvertFromStereoUV pair every shader that
	* samples a per-eye-array resource (FrameBuffer::CameraViewProjInverse[eyeIndex], etc.)
	* off a packed SBS input needs. Flat builds: index is always 0, uv is unchanged.
	*/
	EyeUV UnpackEyeUV(float2 texCoord)
	{
		EyeUV result;
		result.index = GetEyeIndexFromTexCoord(texCoord);
		result.uv = ConvertFromStereoUV(texCoord, result.index);
		return result;
	}

	/**
	* @brief Returns an eye-stable pixel coordinate for seeding screen-space noise (IGN).
	*
	* Seeding interleaved-gradient noise from the raw side-by-side pixel coordinate makes
	* the two eyes hash decorrelated values for the same world point — the half-buffer
	* x-offset dominates — which reads as per-eye rivalry on whatever the noise drives
	* (shadow/dither/LOD shimmer). This maps the coordinate to the eye-local mono pixel
	* grid so both eyes hash the same value for the same screen-relative position, at the
	* same noise frequency (the 0.5 width factor preserves the per-eye pixel pitch).
	* Flat builds have no seam and return the coordinate unchanged (byte-identical).
	*
	* @param[in] sbsPixel  Raw SV_Position-style pixel coordinate in the packed buffer.
	* @param[in] bufferDim Full packed buffer dimensions (both eyes wide in VR).
	* @return Pixel coordinate to pass to Random::InterleavedGradientNoise.
	*/
	float2 EyeStableNoiseCoord(float2 sbsPixel, float2 bufferDim)
	{
#ifdef VR
		float2 stereoUV = sbsPixel / bufferDim;
		uint eyeIndex = GetEyeIndexFromTexCoord(stereoUV);
		float2 monoUV = ConvertFromStereoUV(stereoUV, eyeIndex);
		return monoUV * float2(bufferDim.x * 0.5, bufferDim.y);
#else
		return sbsPixel;
#endif
	}

	/**
	* @brief Applies motion velocity to UV coordinates and determines if the resulting mono UV is out of screen bounds.
	* @param uv Screen UV coordinates (stereo in VR, mono in SE)
	* @param velocity Delta motion mapping
	* @param isOutOfBounds Output flag indicating if the motion went out of bounds
	* @return Newly displaced UV coordinate mapped back to correct space (stereo in VR, mono in SE). Clamped if necessary.
	*/
	float2 ApplyVelocityToUV(float2 uv, float2 velocity, out bool isOutOfBounds)
	{
		uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
		float2 prevUVmono = Stereo::ConvertFromStereoUV(uv, eyeIndex) + velocity;
		float2 clampedMono = prevUVmono;

#ifdef VR
		// Reject the left edge (mono.x <= 0) too, not clamp-and-sample: clamping smears a
		// stretched history column at each eye's left/centre-seam edge on fast head turns.
		isOutOfBounds = (prevUVmono.x >= 1.0) || (prevUVmono.x <= 0.0) || (prevUVmono.y <= 0.0) || (prevUVmono.y >= 1.0);
		clampedMono.x = saturate(prevUVmono.x);
#else
		// SE logic: inclusive boundaries on both sides.
		isOutOfBounds = any(prevUVmono >= 1.0) || any(prevUVmono <= 0.0);
#endif

		return Stereo::ConvertToStereoUV(clampedMono, eyeIndex);
	}

	/**
	Converts to the eye specific screenposition [0,Resolution].
	In VR, texture buffers include the left and right eye in the same buffer. Flat only has a single camera for the entire width.
	This means the x value [0, resx/2] represents the left eye, and the x value (resx/2, x] are the right eye.
	This returns the adjusted value
	@param screenPosition - Screenposition coords ([0,resx], [0,resy]) to be encoded for VR
	@param a_eyeIndex The eyeIndex; 0 is left, 1 is right
	@param a_resolution The resolution of the screen
	@returns screenPosition with x coords adjusted for the VR texture buffer
	*/
	float2 ConvertToStereoSP(float2 screenPosition, uint a_eyeIndex, float2 a_resolution)
	{
		screenPosition.x /= a_resolution.x;
		float2 stereoUV = ConvertToStereoUV(screenPosition, a_eyeIndex);
		return stereoUV * a_resolution;
	}

	float3 ConvertToStereoSP(float3 screenPosition, uint a_eyeIndex, float2 a_resolution)
	{
		float2 xy = screenPosition.xy / a_resolution;
		xy = ConvertToStereoUV(xy, a_eyeIndex);
		return float3(xy * a_resolution, screenPosition.z);
	}

	float4 ConvertToStereoSP(float4 screenPosition, uint a_eyeIndex, float2 a_resolution)
	{
		float2 xy = screenPosition.xy / a_resolution;
		xy = ConvertToStereoUV(xy, a_eyeIndex);
		return float4(xy * a_resolution, screenPosition.zw);
	}

	/**
	* @brief Converts UV coordinates from the range [0, 1] to normalized screen space [-1, 1].
	*
	* This function takes texture coordinates and transforms them into a normalized
	* coordinate system centered at the origin. This is useful for various graphical
	* calculations, especially in shaders that require symmetry around the center.
	*
	* @param uv The input UV coordinates in the range [0, 1].
	* @return float2 Normalized screen space coordinates in the range [-1, 1].
	*/
	float2 ConvertUVToNormalizedScreenSpace(float2 uv)
	{
		float2 normalizedCoord;
		normalizedCoord.x = 2.0 * (-0.5 + abs(2.0 * (uv.x - 0.5)));  // Convert UV.x
		normalizedCoord.y = 2.0 * uv.y - 1.0;                        // Convert UV.y
		return normalizedCoord;
	}

	/**
	* @brief Clamps a stereo UV coordinate to the eye-local X range of the packed stereo buffer.
	*
	* Prevents cross-neighbor UV samples from crossing the x=0.5 seam into the other eye's
	* region of the side-by-side stereo texture. Y is not clamped; sampler address modes
	* handle vertical out-of-bounds. In flat (non-VR) builds there is no seam, so X is just
	* clamped to [0,1] — callers can invoke it unconditionally.
	*
	* @param[in] uv        Stereo UV coordinate to clamp.
	* @param[in] eyeIndex  Eye index (0 = left [0, 0.5], 1 = right [0.5, 1]).
	* @return UV with x restricted to eyeIndex's half of the stereo buffer.
	*/
	float2 ClampToEyeUV(float2 uv, uint eyeIndex)
	{
#ifdef VR
		uv.x = clamp(uv.x, eyeIndex == 0 ? 0.0f : 0.5f, eyeIndex == 0 ? 0.5f : 1.0f);
#else
		// Flat: the whole screen is one eye spanning [0,1]; clamp X to the full range.
		uv.x = saturate(uv.x);
#endif
		return uv;
	}

	/** Clamps a UV to texel centers within one eye for bilinear sampling. */
	float2 ClampToEyeUV(float2 uv, uint eyeIndex, uint2 frameDim)
	{
#ifdef VR
		const uint width = max(frameDim.x, 2u);
		const uint leftWidth = width >> 1;
		const float minCenter = eyeIndex == 0 ? 0.5f : leftWidth + 0.5f;
		const float maxCenter = eyeIndex == 0 ? leftWidth - 0.5f : width - 0.5f;
		uv.x = clamp(uv.x, minCenter / width, maxCenter / width);
#endif
		return uv;
	}

	/**
	* @brief Clamps a pixel coordinate to the eye-local X bounds of the packed stereo buffer.
	*
	* Prevents cross-neighbor pixel reads from crossing the half-width seam into the
	* other eye's region of the side-by-side stereo texture. In flat (non-VR) builds
	* there is no seam, so X is just clamped to [0, frameDim.x-1] — callers can
	* invoke it unconditionally.
	*
	* @param[in] px        Pixel coordinate to clamp.
	* @param[in] eyeIndex  Eye index (0 = left, 1 = right).
	* @param[in] frameDim  Full stereo buffer dimensions (width covers both eyes).
	* @return Clamped pixel coordinate, restricted to eyeIndex's half of the buffer.
	*/
	int2 ClampToEyeBounds(int2 px, uint eyeIndex, float2 frameDim)
	{
#ifdef VR
		int halfWidth = (int)((uint)frameDim.x >> 1);
		px.x = clamp(px.x, eyeIndex == 0 ? 0 : halfWidth, eyeIndex == 0 ? (halfWidth - 1) : ((int)frameDim.x - 1));
#else
		px.x = clamp(px.x, 0, (int)frameDim.x - 1);
#endif
		px.y = clamp(px.y, 0, (int)frameDim.y - 1);
		return px;
	}

#if defined(PSHADER) || defined(FRAMEBUFFER)
	// These functions require the framebuffer which is typically provided with the PSHADER
	/**
	Gets the eyeIndex for PSHADER
	@returns eyeIndex (0 left, 1 right)
	*/
	uint GetEyeIndexPS(float4 position, float4 offset = 0.0.xxxx)
	{
#	if !defined(VR)
		uint eyeIndex = 0;
#	else
		float4 stereoUV;
		stereoUV.xy = position.xy * offset.xy + offset.zw;
		stereoUV.x = FrameBuffer::DynamicResolutionParams2.x * stereoUV.x;
		stereoUV.x = (stereoUV.x >= 0.5);
		uint eyeIndex = (uint)(((int)((uint)StereoEnabled)) * (int)stereoUV.x);
#	endif
		return eyeIndex;
	}
#endif  // PSHADER

#ifdef VSHADER
	struct VR_OUTPUT
	{
		float4 VRPosition;
		float ClipDistance;
		float CullDistance;
	};

	/**
	Gets the eyeIndex for VSHADER
	@returns eyeIndex (0 left, 1 right)
	*/
	uint GetEyeIndexVS(uint instanceID = 0)
	{
#	ifdef VR
		return StereoEnabled * (instanceID & 1);
#	endif  // VR
		return 0;
	}

	/**
	Gets VR Output
	@param clipPos clipPosition. Typically the VSHADER position at SV_POSITION0
	@param a_eyeIndex The eyeIndex; 0 is left, 1 is right
	@returns VR_OUTPUT with VR values
	*/
	VR_OUTPUT GetVRVSOutput(float4 clipPos, uint a_eyeIndex = 0)
	{
		VR_OUTPUT vsout = {
			0.0.xxxx,  // VRPosition
			0.0f,      // ClipDistance
			0.0f       // CullDistance
		};

#	ifdef VR
		bool isStereoEnabled = (StereoEnabled != 0);
		float2 clipEdges;

		if (isStereoEnabled) {
			clipEdges.x = dot(clipPos, EyeClipEdge[a_eyeIndex]);
			clipEdges.y = clipEdges.x;  // Both use the same calculation
		} else {
			clipEdges = float2(1.0f, 1.0f);
		}

		float stereoAdjustment = 2.0f - StereoEnabled;
		float eyeOffset = dot(EyeOffsetScale, Math::IdentityMatrix[a_eyeIndex].xy);

		float xPositionOffset = eyeOffset * clipPos.w * (isStereoEnabled ? 1.0f : 0.0f);
		float xPositionBase = stereoAdjustment * clipPos.x;

		vsout.VRPosition.x = xPositionBase * 0.5f + xPositionOffset;
		vsout.VRPosition.y = clipPos.y;
		vsout.VRPosition.z = clipPos.z;
		vsout.VRPosition.w = clipPos.w;

		vsout.ClipDistance = clipEdges.y;
		vsout.CullDistance = clipEdges.x;
#	endif  // VR
		return vsout;
	}
#endif

}
#endif  //__VR_DEPENDENCY_HLSL__
