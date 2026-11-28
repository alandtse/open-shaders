#define VR
#define COMPUTESHADER
#include "/Shaders/Tests/WaterShallowSurfaceCases.hlsli"

/// @tags water, shallow-surface
[numthreads(1, 1, 1)] void TestShallowVRStandardDepthClassification() {
	CheckShallowDepthClassification();
}

	/// @tags water, shallow-surface
	[numthreads(1, 1, 1)] void TestShallowVRStandardNativeReconstruction()
{
	CheckShallowNativeReconstruction();
}

/// @tags water, shallow-surface
[numthreads(1, 1, 1)] void TestShallowVRStandardBoundsAndConnection() {
	CheckShallowBoundsAndConnection();
}

	/// @tags water, shallow-surface
	[numthreads(1, 1, 1)] void TestShallowVRStandardWeightBounds()
{
	CheckShallowWeightBounds();
}
