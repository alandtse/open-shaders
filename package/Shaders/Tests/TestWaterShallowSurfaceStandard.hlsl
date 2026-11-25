#define COMPUTESHADER
#include "/Shaders/Tests/WaterShallowSurfaceCases.hlsli"

/// @tags water, shallow-surface
[numthreads(1, 1, 1)] void TestShallowStandardDepthClassification() {
	CheckShallowDepthClassification();
}

	/// @tags water, shallow-surface
	[numthreads(1, 1, 1)] void TestShallowStandardNativeReconstruction()
{
	CheckShallowNativeReconstruction();
}

/// @tags water, shallow-surface
[numthreads(1, 1, 1)] void TestShallowStandardBoundsAndConnection() {
	CheckShallowBoundsAndConnection();
}

	/// @tags water, shallow-surface
	[numthreads(1, 1, 1)] void TestShallowStandardWeightBounds()
{
	CheckShallowWeightBounds();
}
