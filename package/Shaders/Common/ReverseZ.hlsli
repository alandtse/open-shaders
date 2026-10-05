#ifndef __REVERSE_Z_DEPENDENCY_HLSL__
#define __REVERSE_Z_DEPENDENCY_HLSL__

#include "Common/FrameBuffer.hlsli"

#ifdef REVERSE_Z
#	define SCENE_DEPTH_FORMAT float
#else
#	define SCENE_DEPTH_FORMAT unorm float
#endif

// Under REVERSE_Z the depth buffer holds 1 - standard depth: convert samples to standard before
// comparing them with projected depth, and back to native before unprojecting.
namespace FrameBuffer
{
	float FarPlaneDepth()
	{
#ifdef REVERSE_Z
		return 0.0;
#else
		return 1.0;
#endif
	}

	float NearPlaneDepth()
	{
		return 1.0 - FarPlaneDepth();
	}

	/** @brief The nearer of two raw depths under the active convention. */
	float NearerDepth(float a, float b)
	{
#ifdef REVERSE_Z
		return max(a, b);
#else
		return min(a, b);
#endif
	}

	/** @brief The farther of two raw depths under the active convention. */
	float FartherDepth(float a, float b)
	{
#ifdef REVERSE_Z
		return min(a, b);
#else
		return max(a, b);
#endif
	}

	/** @brief The nearest of four raw depths under the active convention. */
	float NearestDepth(float4 v)
	{
		return NearerDepth(NearerDepth(v.x, v.y), NearerDepth(v.z, v.w));
	}

	/** @brief The farthest of four raw depths under the active convention. */
	float FarthestDepth(float4 v)
	{
		return FartherDepth(FartherDepth(v.x, v.y), FartherDepth(v.z, v.w));
	}

	/** @brief True when a is strictly nearer than b under the active convention. */
	bool IsNearerDepth(float a, float b)
	{
#ifdef REVERSE_Z
		return a > b;
#else
		return a < b;
#endif
	}

	bool IsReverseProjection(float4x4 projection)
	{
#ifdef REVERSE_Z
		return projection[2][2] * projection[3][2] < 0.0;
#else
		return false;
#endif
	}

	bool IsReverseProjection(uint a_eyeIndex = 0)
	{
		return IsReverseProjection(CameraProj[a_eyeIndex]);
	}

	float FarPlaneClipZ(float clipW, bool reverseProjection)
	{
		return reverseProjection ? 0.0 : clipW;
	}

	float FarPlaneClipZ(float clipW)
	{
		return FarPlaneClipZ(clipW, IsReverseProjection());
	}

	float ToStandardClipZ(float4 clipPosition, bool reverseProjection)
	{
		return reverseProjection ? clipPosition.w - clipPosition.z : clipPosition.z;
	}

	float ToStandardClipZ(float4 clipPosition)
	{
		return ToStandardClipZ(clipPosition, IsReverseProjection());
	}

	float3 ToStandardClip(float4 clipPosition, bool reverseProjection)
	{
		return float3(clipPosition.xy, ToStandardClipZ(clipPosition, reverseProjection));
	}

	float3 ToStandardClip(float4 clipPosition)
	{
		return ToStandardClip(clipPosition, IsReverseProjection());
	}

	float4 OffsetClipDepth(float4 clipPosition, float standardOffset, bool reverseProjection)
	{
		clipPosition.z += reverseProjection ? -standardOffset : standardOffset;
		return clipPosition;
	}

	float4 OffsetClipDepth(float4 clipPosition, float standardOffset)
	{
		return OffsetClipDepth(clipPosition, standardOffset, IsReverseProjection());
	}

	float ToNativeDepth(float standardDepth)
	{
#ifdef REVERSE_Z
		return 1.0 - standardDepth;
#else
		return standardDepth;
#endif
	}

	float4 ToNativeDepth(float4 standardDepth)
	{
#ifdef REVERSE_Z
		return 1.0 - standardDepth;
#else
		return standardDepth;
#endif
	}

	float ToStandardDepth(float depth)
	{
#ifdef REVERSE_Z
		return 1.0 - depth;
#else
		return depth;
#endif
	}

	float2 ToStandardDepth(float2 depth)
	{
#ifdef REVERSE_Z
		return 1.0 - depth;
#else
		return depth;
#endif
	}

	float4 ToStandardDepth(float4 depth)
	{
#ifdef REVERSE_Z
		return 1.0 - depth;
#else
		return depth;
#endif
	}
}

#endif  //__REVERSE_Z_DEPENDENCY_HLSL__
