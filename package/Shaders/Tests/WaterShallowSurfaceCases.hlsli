#include "/Shaders/Common/WaterShallowSurfaceMath.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

void CheckShallowDepthClassification()
{
	const float farDepth = FrameBuffer::FarPlaneDepth();
	ASSERT(IsTrue, !WaterShallowSurface::IsSceneDepthValid(farDepth, farDepth));
	ASSERT(IsTrue, !WaterShallowSurface::IsSceneDepthValid(FrameBuffer::NearPlaneDepth(), farDepth));
	ASSERT(IsTrue, !WaterShallowSurface::IsSceneDepthValid(FrameBuffer::ToNativeDepth(1e-6), farDepth));
	ASSERT(IsTrue, WaterShallowSurface::IsSceneDepthValid(FrameBuffer::ToNativeDepth(0.5), farDepth));
	ASSERT(IsTrue, !WaterShallowSurface::IsSceneDepthValid(-0.1, farDepth));
	ASSERT(IsTrue, !WaterShallowSurface::IsSceneDepthValid(1.1, farDepth));
	ASSERT(IsTrue, !WaterShallowSurface::IsSceneDepthValid(asfloat(0x7fc00000u), farDepth));
	ASSERT(IsTrue, !WaterShallowSurface::IsSceneDepthValid(asfloat(0x7f800000u), farDepth));
#if defined(REVERSE_Z)
	const float farGeometry = 1e-8;
	const float foldedBackdrop = 8.0 / 16777216.0;
	const float emptyDepth = 64.0 / 16777216.0;
#else
	const float farGeometry = asfloat(0x3f7fffffu);
	const float foldedBackdrop = 1.0 - 8.0 / 16777216.0;
	const float emptyDepth = 1.0 - 64.0 / 16777216.0;
#endif
	ASSERT(IsTrue, WaterShallowSurface::IsSceneDepthValid(farGeometry, farDepth));
	ASSERT(IsTrue, !WaterShallowSurface::IsSceneDepthValid(emptyDepth, emptyDepth));
	ASSERT(IsTrue, !WaterShallowSurface::IsSceneDepthValid(foldedBackdrop, emptyDepth));
	ASSERT(IsTrue, WaterShallowSurface::IsSceneDepthValid(FrameBuffer::ToNativeDepth(0.9), emptyDepth));
}

void CheckShallowNativeReconstruction()
{
	const float4x4 identity = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
	float3 position;
#if defined(VR)
	ASSERT(IsTrue, WaterShallowSurface::TryReconstructScenePosition(float2(0.25, 0.25), 1e-8, 0, identity, position));
	ASSERT(IsTrue, all(position == float3(0.0, 0.5, 1e-8)));
	ASSERT(IsTrue, WaterShallowSurface::TryReconstructScenePosition(float2(0.75, 0.25), 1e-8, 1, identity, position));
#else
	ASSERT(IsTrue, WaterShallowSurface::TryReconstructScenePosition(float2(0.5, 0.25), 1e-8, 0, identity, position));
#endif
	ASSERT(IsTrue, all(position == float3(0.0, 0.5, 1e-8)));
	ASSERT(IsTrue, !WaterShallowSurface::TryReconstructScenePosition(0.5.xx, 0.5, 0, (float4x4)0, position));
	ASSERT(IsTrue, !WaterShallowSurface::TryReconstructScenePosition(float2(asfloat(0x7fc00000u), 0.5), 0.5, 0, identity, position));
}

void CheckShallowBoundsAndConnection()
{
	const int2 dimensions = int2(200, 100);
	ASSERT(IsTrue, !WaterShallowSurface::IsDepthPixelInEyeBounds(int2(-1, 20), 0, dimensions));
	ASSERT(IsTrue, !WaterShallowSurface::IsDepthPixelInEyeBounds(int2(10, 100), 0, dimensions));
	ASSERT(IsTrue, WaterShallowSurface::IsDepthPixelInEyeBounds(int2(99, 20), 0, dimensions));
#if defined(VR)
	ASSERT(IsTrue, !WaterShallowSurface::IsDepthPixelInEyeBounds(int2(100, 20), 0, dimensions));
	ASSERT(IsTrue, !WaterShallowSurface::IsDepthPixelInEyeBounds(int2(99, 20), 1, dimensions));
	ASSERT(IsTrue, WaterShallowSurface::IsDepthPixelInEyeBounds(int2(100, 20), 1, dimensions));
	ASSERT(IsTrue, WaterShallowSurface::IsDepthPixelInEyeBounds(int2(199, 99), 1, dimensions));
#else
	ASSERT(IsTrue, WaterShallowSurface::IsDepthPixelInEyeBounds(int2(199, 99), 0, dimensions));
#endif
	int2 midpoint;
	int2 endpoint;
	ASSERT(IsTrue, !WaterShallowSurface::TryGetProbeOffsets(float2(1, 0), 1.0, midpoint, endpoint));
	ASSERT(IsTrue, WaterShallowSurface::TryGetProbeOffsets(float2(1, 0), 4.0, midpoint, endpoint));
	ASSERT(IsTrue, all(midpoint == int2(2, 0)) && all(endpoint == int2(4, 0)));
	ASSERT(IsTrue, WaterShallowSurface::GetConnectedDeepWeight(false, -1, true, 400, 192, 128, 1) == 0.0);
	ASSERT(IsTrue, WaterShallowSurface::GetConnectedDeepWeight(true, 32, false, -1, 192, 128, 1) == 0.0);
	ASSERT(IsTrue, WaterShallowSurface::GetConnectedDeepWeight(true, 192, false, -1, 192, 128, 1) == 1.0);
	ASSERT(IsTrue, WaterShallowSurface::GetConnectedDeepWeight(true, 32, true, 192, 192, 128, 0.5) == 0.5);
}

void CheckShallowWeightBounds()
{
	ASSERT(IsTrue, WaterShallowSurface::GetShoreContactWeight(-1, 0, 0, 0) == 0.0);
	ASSERT(IsTrue, WaterShallowSurface::GetShoreContactWeight(0, 10, 2, 1) == 0.0);
	ASSERT(IsTrue, WaterShallowSurface::GetShoreContactWeight(10, 10, 2, 1) == 1.0);
	ASSERT(IsTrue, WaterShallowSurface::GetShoreContactWeight(10, 10, 2, 10) == 0.5);
	ASSERT(IsTrue, WaterShallowSurface::GetDepthWeight(-1, 256) == 0.0);
	ASSERT(IsTrue, WaterShallowSurface::GetDepthWeight(0, 256) == 1.0);
	ASSERT(IsTrue, WaterShallowSurface::GetDepthWeight(128, 256) == 0.5);
	ASSERT(IsTrue, WaterShallowSurface::GetDepthWeight(256, 256) == 0.0);
	ASSERT(IsTrue, WaterShallowSurface::GetCandidateWeight(1, 1, 1, 0, 1) == 0.5);
	ASSERT(IsTrue, WaterShallowSurface::GetCandidateWeight(1, 1, 0, 0, 1) == 0.0);
	ASSERT(IsTrue, WaterShallowSurface::GetCandidateWeight(1, 1, 1, 1, 1) == 0.0);
	ASSERT(IsTrue, WaterShallowSurface::GetCandidateWeight(1, 1, 1, asfloat(0x7fc00000u), 1) == 0.0);
	ASSERT(IsTrue, WaterShallowSurface::GetAvailability(0, 0) == 0.0);
	ASSERT(IsTrue, WaterShallowSurface::GetAvailability(0, 26000) == 1.0);
	ASSERT(IsTrue, WaterShallowSurface::GetAvailability(26000, 26000) == 0.0);
}
