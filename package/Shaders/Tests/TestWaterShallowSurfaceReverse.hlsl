#define REVERSE_Z
#define COMPUTESHADER
#include "/Shaders/Tests/WaterShallowSurfaceCases.hlsli"

/// @tags water, shallow-surface
[numthreads(1, 1, 1)] void TestShallowReverseDepthClassification() {
	CheckShallowDepthClassification();
}

	/// @tags water, shallow-surface
	[numthreads(1, 1, 1)] void TestShallowReverseNativeReconstruction()
{
	CheckShallowNativeReconstruction();
}

/// @tags water, shallow-surface
[numthreads(1, 1, 1)] void TestShallowReverseBoundsAndConnection() {
	CheckShallowBoundsAndConnection();
}

	/// @tags water, shallow-surface
	[numthreads(1, 1, 1)] void TestShallowReverseWeightBounds()
{
	CheckShallowWeightBounds();
}
