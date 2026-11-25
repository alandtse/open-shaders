#define VR
#define REVERSE_Z
#define COMPUTESHADER
#include "/Shaders/Tests/WaterShallowSurfaceCases.hlsli"

/// @tags water, shallow-surface
[numthreads(1, 1, 1)] void TestShallowVRReverseDepthClassification() {
	CheckShallowDepthClassification();
}

	/// @tags water, shallow-surface
	[numthreads(1, 1, 1)] void TestShallowVRReverseNativeReconstruction()
{
	CheckShallowNativeReconstruction();
}

/// @tags water, shallow-surface
[numthreads(1, 1, 1)] void TestShallowVRReverseBoundsAndConnection() {
	CheckShallowBoundsAndConnection();
}

	/// @tags water, shallow-surface
	[numthreads(1, 1, 1)] void TestShallowVRReverseWeightBounds()
{
	CheckShallowWeightBounds();
}
